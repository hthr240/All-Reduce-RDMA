#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pg_internal.h"

static pg_log_level_t g_log_level = PG_LOG_INFO;

void pg_log_set_level(pg_log_level_t level)
{
    g_log_level = level;
}

void pg_log_phase(int rank, int size, int phase, int total,
                  const char *name)
{
    if (rank >= 0 && size > 0) {
        fprintf(stderr, "\n[PHASE %d/%d] [rank %d/%d] %s\n",
                phase, total, rank, size, name);
    } else {
        fprintf(stderr, "\n[PHASE %d/%d] %s\n", phase, total, name);
    }
    fflush(stderr);
}

static const char *log_level_name(pg_log_level_t level)
{
    switch (level) {
        case PG_LOG_DEBUG: return "DEBUG";
        case PG_LOG_INFO:  return "INFO";
        case PG_LOG_WARN:  return "WARN";
        case PG_LOG_ERROR: return "ERROR";
        default:           return "UNKNOWN";
    }
}

void pg_log_impl(pg_log_level_t level, const char *module, const char *file,
                 int line, const char *fmt, ...)
{
    va_list args;
    const char *filename;

    if (level < g_log_level) {
        return;
    }

    filename = strrchr(file, '/');
    if (!filename) {
        filename = strrchr(file, '\\');
    }
    filename = filename ? filename + 1 : file;

    fprintf(stderr, "[%s] %s:%d (%s) ",
            log_level_name(level), filename, line, module);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

static void configure_logging(void)
{
    const char *level = getenv("PG_LOG_LEVEL");

    if (!level) {
        return;
    }
    if (strcmp(level, "debug") == 0) {
        pg_log_set_level(PG_LOG_DEBUG);
    } else if (strcmp(level, "info") == 0) {
        pg_log_set_level(PG_LOG_INFO);
    } else if (strcmp(level, "warn") == 0) {
        pg_log_set_level(PG_LOG_WARN);
    } else if (strcmp(level, "error") == 0) {
        pg_log_set_level(PG_LOG_ERROR);
    }
}

/*
 * destroy_process_group:
 *  Release every resource that may have been created for a process group.
 *
 * Initialization can fail after any individual step. All fields in pg start
 * as NULL because the structure is allocated with calloc, so this same helper
 * can clean up both a complete and a partially initialized process group.
 */
static void destroy_process_group(pg_handle_t *pg)
{
    if (!pg) {
        return;
    }

    if (pg->is_connected && pg->sock_previous >= 0 && pg->sock_next >= 0) {
        (void)bootstrap_ring_barrier(pg);
    }
    if (pg->sock_previous >= 0) {
        close(pg->sock_previous);
    }
    if (pg->sock_next >= 0) {
        close(pg->sock_next);
    }
    destroy_rdma_resources(pg);
    free(pg->hostname);
    free(pg);
}

int configure_transport_mode(pg_handle_t *pg)
{
    const char *mode;
    const char *no_pipeline;
    const char *threshold;

    if (!pg) {
        return -1;
    }
    pg->transport_mode = PG_TRANSPORT_AUTO;
    pg->eager_threshold = PG_EAGER_THRESHOLD;
    no_pipeline = getenv("PG_NOPIPE");
    pg->pipeline_enabled = !no_pipeline || strcmp(no_pipeline, "1") != 0;
    threshold = getenv("PG_EAGER_THRESHOLD");
    if (threshold) {
        char *end = NULL;
        unsigned long long value;

        errno = 0;
        value = strtoull(threshold, &end, 10);
        if (errno != 0 || threshold[0] == '-' || end == threshold ||
            *end != '\0' || value > SIZE_MAX) {
            fprintf(stderr, "Invalid PG_EAGER_THRESHOLD value: %s\n",
                    threshold);
            return -1;
        }
        pg->eager_threshold = (size_t)value;
    }
    mode = getenv("PG_MODE");
    if (!mode || strcmp(mode, "auto") == 0) {
        return 0;
    }
    if (strcmp(mode, "eager") == 0) {
        pg->transport_mode = PG_TRANSPORT_EAGER;
        return 0;
    }
    if (strcmp(mode, "rdvz") == 0) {
        pg->transport_mode = PG_TRANSPORT_RDVZ;
        return 0;
    }
    fprintf(stderr, "Invalid PG_MODE value: %s\n", mode);
    return -1;
}

static int use_rendezvous_transport(const pg_handle_t *pg, int count,
                                    size_t element_size)
{
    size_t largest_chunk_bytes;

    largest_chunk_bytes = ((size_t)count + (size_t)pg->size - 1) /
                          (size_t)pg->size * element_size;
    return pg->transport_mode == PG_TRANSPORT_RDVZ ||
           (pg->transport_mode == PG_TRANSPORT_AUTO &&
            largest_chunk_bytes > pg->eager_threshold);
}

/*
 * connect_process_group:
 *  Initialize the local process-group handle and local RDMA resources.
 *
 *  Current phase flow:
 *   1. Allocate the opaque handle and copy the hostname.
 *   2. Discover and open the first available Verbs device.
 *   3. Find an active physical port.
 *   4. Allocate a protection domain and registered communication buffer.
 *   5. Create a completion queue and a reliable-connected queue pair.
 *   6. Move the new QP from RESET to INIT.
 *
 *  This function does not contact another process yet. The QP remains in INIT
 *  until a later phase exchanges remote metadata and moves it through RTR and
 *  RTS. Therefore is_connected currently means that local setup succeeded,
 *  not that a remote rank is connected.
 *
 *  Parameters:
 *   - servername: the host associated with this rank in the group
 *   - pg_handle: output pointer where the initialized handle is stored
 *
 *  Returns:
 *   - 0 on success
 *   - -1 on failure
 */
int connect_process_group(char *servername, void **pg_handle)
{
    pg_handle_t *pg = NULL;
    char **host_list = NULL;
    int rank = 0;
    int host_count = 1;
    int distributed = 0;

    configure_logging();

    /* Without this output address there is nowhere to return the new handle. */
    if (!servername || !pg_handle) {
        fprintf(stderr, "Invalid process-group handle pointer\n");
        return -1;
    }
    *pg_handle = NULL;

    distributed = strchr(servername, ':') != NULL;
    if (distributed &&
        parse_process_group_spec(servername, &rank, &host_list, &host_count) != 0) {
        fprintf(stderr, "Invalid process-group specification\n");
        return -1;
    }

    /* calloc gives every resource pointer a known NULL value for cleanup. */
    pg = calloc(1, sizeof(*pg));
    if (!pg) {
        fprintf(stderr, "Could not allocate process-group handle\n");
        return -1;
    }

    pg->rank = rank;
    pg->size = host_count;
    pg->is_connected = 0;
    pg->sock_previous = -1;
    pg->sock_next = -1;
    if (configure_transport_mode(pg) != 0) {
        destroy_process_group(pg);
        free_process_group_hosts(host_list, host_count);
        return -1;
    }

    /* Keep an owned hostname copy; the caller retains ownership of its input. */
    pg->hostname = strdup(distributed ? host_list[rank] : servername);
    if (!pg->hostname ||
        configure_process_group_topology(pg, rank, host_count) != 0) {
        fprintf(stderr, "Could not copy process-group hostname\n");
        destroy_process_group(pg);
        free_process_group_hosts(host_list, host_count);
        return -1;
    }

    /* Initialize all RDMA resources via the verbs module. */
    if (create_rdma_resources(pg) != 0) {
        destroy_process_group(pg);
        free_process_group_hosts(host_list, host_count);
        return -1;
    }

    if (distributed) {
        if (bootstrap_ring(pg, host_list, host_count) != 0) {
            destroy_process_group(pg);
            free_process_group_hosts(host_list, host_count);
            return -1;
        }
    } else {
        pg->is_connected = 1;
    }

    *pg_handle = pg;
    free_process_group_hosts(host_list, host_count);
    return 0;
}

/*
 * pg_all_reduce:
 *  Public collective API for the all-reduce operation.
 *
 *  This function is the main collective entry point. In the final design it will:
 *  - validate the datatype and operation
 *  - divide the data into ring chunks
 *  - run Reduce Scatter
 *  - run All Gather
 *  - write the final reduced result into recvbuf
 *
 *  Parameters:
 *   - sendbuf: input data for the local rank
 *   - recvbuf: output buffer for the reduced result
 *   - count: number of elements to reduce
 *   - datatype: element type
 *   - op: reduction operation
 *   - pg_handle: process-group handle for the local rank
 *
 *  Returns:
 *   - 0 on success
 *   - -1 on failure
 */
int pg_all_reduce(void *sendbuf, void *recvbuf, int count, DATATYPE datatype, OPERATION op, void *pg_handle)
{
    pg_handle_t *pg = (pg_handle_t *)pg_handle;
    size_t element_size;
    int use_rendezvous;

    if (!pg || !sendbuf || !recvbuf || count < 0 ||
        pg_validate_reduction(datatype, op) != 0 ||
        pg->size <= 0 || pg->rank < 0 || pg->rank >= pg->size) {
        fprintf(stderr, "Invalid all-reduce arguments\n");
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    element_size = pg_datatype_size(datatype);
    if (element_size == 0) {
        fprintf(stderr, "Unsupported datatype for all-reduce\n");
        return -1;
    }

    if (pg->size == 1) {
        memcpy(recvbuf, sendbuf, (size_t)count * element_size);
        return 0;
    }

    use_rendezvous = use_rendezvous_transport(pg, count, element_size);
    if (use_rendezvous) {
        if (pg_run_rendezvous_reduce_scatter(pg, sendbuf, recvbuf, count,
                                             datatype, op) != 0 ||
            pg_run_rendezvous_all_gather(pg, recvbuf, count, datatype) != 0) {
            fprintf(stderr, "Rendezvous all-reduce failed\n");
            return -1;
        }
        return 0;
    }

    if (pg_run_eager_reduce_scatter(pg, sendbuf, recvbuf, count,
                                    datatype, op) != 0) {
        fprintf(stderr, "Reduce Scatter stage failed\n");
        return -1;
    }
    if (pg_run_eager_all_gather(pg, recvbuf, count, datatype) != 0) {
        fprintf(stderr, "All Gather stage failed\n");
        return -1;
    }
    return 0;
}

int pg_chunk(void *pg_handle, int count, int *offset, int *nelem)
{
    pg_handle_t *pg = (pg_handle_t *)pg_handle;
    int chunk;

    if (!pg || count < 0 || !offset || !nelem || pg->size <= 0) {
        return -1;
    }
    chunk = (pg->rank + 1) % pg->size;
    *offset = pg_chunk_offset(count, pg->size, chunk);
    *nelem = pg_chunk_nelem(count, pg->size, chunk);
    return *offset < 0 || *nelem < 0 ? -1 : 0;
}

int pg_rank(void *pg_handle)
{
    pg_handle_t *pg = (pg_handle_t *)pg_handle;

    return pg ? pg->rank : -1;
}

int pg_nranks(void *pg_handle)
{
    pg_handle_t *pg = (pg_handle_t *)pg_handle;

    return pg ? pg->size : -1;
}

int pg_reduce_scatter(void *sendbuf, void *recvbuf, int count,
                      DATATYPE datatype, OPERATION op, void *pg_handle)
{
    pg_handle_t *pg = (pg_handle_t *)pg_handle;
    size_t element_size;
    int rc;

    if (!pg || count < 0 || pg_validate_reduction(datatype, op) != 0 ||
        pg->size <= 0 || pg->rank < 0 || pg->rank >= pg->size) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (!sendbuf || !recvbuf) {
        return -1;
    }
    element_size = pg_datatype_size(datatype);
    if (pg->size == 1) {
        memcpy(recvbuf, sendbuf, (size_t)count * element_size);
        return 0;
    }
    if (use_rendezvous_transport(pg, count, element_size)) {
        rc = pg_run_rendezvous_reduce_scatter(pg, sendbuf, recvbuf, count,
                                              datatype, op);
    } else {
        rc = pg_run_eager_reduce_scatter(pg, sendbuf, recvbuf, count,
                                         datatype, op);
    }
    if (rc == 0) {
        ++pg->collective_sequence;
    }
    return rc;
}

int pg_all_gather(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, void *pg_handle)
{
    pg_handle_t *pg = (pg_handle_t *)pg_handle;
    size_t element_size;
    int owned_chunk;
    int owned_count;
    int owned_offset;

    if (!pg || !sendbuf || !recvbuf || count < 0 ||
        pg_datatype_size(datatype) == 0 || pg->size <= 0 ||
        pg->rank < 0 || pg->rank >= pg->size) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    element_size = pg_datatype_size(datatype);
    if (pg->size == 1) {
        memcpy(recvbuf, sendbuf, (size_t)count * element_size);
        return 0;
    }
    if (!pg->buf || !pg->is_connected) {
        return -1;
    }

    owned_chunk = (pg->rank + 1) % pg->size;
    owned_count = pg_chunk_nelem(count, pg->size, owned_chunk);
    owned_offset = pg_chunk_offset(count, pg->size, owned_chunk);
    if (owned_count < 0 || owned_offset < 0 ||
        (size_t)count * element_size > PG_WORK_BUFFER_SIZE) {
        return -1;
    }
    memcpy((unsigned char *)pg->buf +
               (size_t)owned_offset * element_size,
           sendbuf, (size_t)owned_count * element_size);
    if (use_rendezvous_transport(pg, count, element_size)) {
        return pg_run_rendezvous_all_gather(pg, recvbuf, count, datatype);
    }
    return pg_run_eager_all_gather(pg, recvbuf, count, datatype);
}

/* Phase 4 connectivity smoke test; collective data movement comes later. */
int pg_ring_token(void *pg_handle, int laps)
{
    pg_handle_t *pg = (pg_handle_t *)pg_handle;

    if (!pg || !pg->is_connected) {
        fprintf(stderr, "Invalid or disconnected process-group handle\n");
        return -1;
    }
    if (pg->sock_previous >= 0 && pg->sock_next >= 0 &&
        bootstrap_ring_barrier(pg) != 0) {
        fprintf(stderr, "Ring token setup barrier failed\n");
        return -1;
    }
    return ring_token(pg, laps);
}

/*
 * pg_close:
 *  Release the process-group handle and any associated local resources.
 *
 *  In the final implementation this function must free all registered memory,
 *  destroy queue pairs, and clean up the verbs objects in reverse dependency order.
 */
int pg_close(void *pg_handle)
{
    pg_handle_t *pg = (pg_handle_t *)pg_handle;

    if (!pg) {
        return 0;
    }

    destroy_process_group(pg);
    return 0;
}

#ifndef PG_LIBRARY_ONLY
static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s -myindex <one-based-rank> -list <host1> [host2 ...]\n"
            "       %s -myindex <one-based-rank> -list <host1> [host2 ...] -token\n"
            "       %s <hostname>\n",
            program, program, program);
}

static int parse_rank_and_hosts(int argc, char **argv, int *rank,
                                char ***host_list, int *host_count)
{
    int index;

    *rank = -1;
    *host_count = 0;
    *host_list = NULL;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "-myindex") == 0 && index + 1 < argc) {
            char *end = NULL;
            long value = strtol(argv[++index], &end, 10);

            if (end == argv[index] || *end != '\0' ||
                value < 1 || value > 65536) {
                fprintf(stderr, "Invalid one-based -myindex value\n");
                return -1;
            }
            *rank = (int)value - 1;
        } else if (strcmp(argv[index], "-list") == 0) {
            int first_host = index + 1;
            int count = 0;
            char **hosts;
            int host;

            while (index + 1 < argc && argv[index + 1][0] != '-') {
                ++index;
                ++count;
            }
            hosts = calloc((size_t)count, sizeof(*hosts));
            if (!hosts) {
                fprintf(stderr, "Out of memory while parsing host list\n");
                return -1;
            }
            for (host = 0; host < count; ++host) {
                hosts[host] = strdup(argv[first_host + host]);
                if (!hosts[host]) {
                    free_process_group_hosts(hosts, host);
                    fprintf(stderr, "Out of memory while copying host list\n");
                    return -1;
                }
            }
            *host_list = hosts;
            *host_count = count;
        }
    }
    return 0;
}

static char *build_process_group_spec(int rank, char **hosts, int host_count)
{
    size_t length = 32;
    char *spec;
    int index;

    for (index = 0; index < host_count; ++index) {
        length += strlen(hosts[index]) + 1;
    }
    spec = malloc(length);
    if (!spec) {
        return NULL;
    }
    snprintf(spec, length, "%d:", rank + 1);
    for (index = 0; index < host_count; ++index) {
        strcat(spec, index == 0 ? "" : ",");
        strcat(spec, hosts[index]);
    }
    return spec;
}

/*
 * main:
 *  Program entry point.
 *
 *  Responsibilities:
 *   - parse the command line
 *   - determine the rank and process-group membership
 *   - initialize the process-group handle
 *   - exercise the collective interface in the current skeleton build
 *
 *  This is the orchestration layer that connects user input to the process-group state.
 */
int main(int argc, char **argv)
{
    char **host_list = NULL;
    int host_count = 0;
    int myindex = -1;
    char *group_spec = NULL;
    const char *connect_target = NULL;
    void *pg_handle = NULL;
    int rc;
    int run_token = 0;

    pg_log_phase(-1, 0, 1, 4, "Parse process-group configuration");
    PG_LOG_INFO("main", "Starting ring_allreduce (argc=%d)", argc);

    for (rc = 1; rc < argc; ++rc) {
        if (strcmp(argv[rc], "-token") == 0) {
            run_token = 1;
        }
    }

    if (argc < 2) {
        PG_LOG_ERROR("main", "Insufficient arguments");
        usage(argv[0]);
        return 1;
    }

    PG_LOG_DEBUG("main", "Parsing command-line arguments");
    rc = parse_rank_and_hosts(argc, argv, &myindex, &host_list, &host_count);
    if (rc != 0) {
        PG_LOG_ERROR("main", "Failed to parse rank and hosts");
        free_process_group_hosts(host_list, host_count);
        return 1;
    }

    /* Distributed mode: validate the complete rank-to-host mapping first. */
    if (myindex >= 0 && host_count > 0) {
        PG_LOG_INFO("main", "Distributed mode: rank=%d, group_size=%d", myindex, host_count);
        if (validate_host_list(host_list, host_count) != 0) {
            PG_LOG_ERROR("main", "Host list validation failed");
            free_process_group_hosts(host_list, host_count);
            return 1;
        }
        if (myindex >= host_count) {
            PG_LOG_ERROR("main", "-myindex %d out of range for group size %d", myindex, host_count);
            free_process_group_hosts(host_list, host_count);
            return 1;
        }
        group_spec = build_process_group_spec(myindex, host_list, host_count);
        if (!group_spec) {
            PG_LOG_ERROR("main", "Could not build process-group specification");
            free_process_group_hosts(host_list, host_count);
            return 1;
        }
        connect_target = group_spec;
    /* Standalone mode represents a one-rank group for local development. */
    } else if (argc == 2) {
        PG_LOG_INFO("main", "Standalone mode: hostname=%s", argv[1]);
        connect_target = argv[1];
    } else {
        PG_LOG_ERROR("main", "Invalid argument combination");
        usage(argv[0]);
        free_process_group_hosts(host_list, host_count);
        return 1;
    }

    pg_log_phase(myindex >= 0 ? myindex : 0,
                 myindex >= 0 ? host_count : 1,
                 2, 4, "Create and connect process group");
    PG_LOG_DEBUG("main", "Connecting process group");
    rc = connect_process_group((char *)connect_target, &pg_handle);
    free(group_spec);
    if (rc != 0) {
        PG_LOG_ERROR("main", "Failed to initialize process group");
        free_process_group_hosts(host_list, host_count);
        return 1;
    }
    PG_LOG_INFO("main", "Process group connected successfully");
    PG_LOG_INFO("main",
                "Topology ready: rank=%d size=%d previous=%d next=%d",
                ((pg_handle_t *)pg_handle)->rank,
                ((pg_handle_t *)pg_handle)->size,
                ((pg_handle_t *)pg_handle)->previous_rank,
                ((pg_handle_t *)pg_handle)->next_rank);

    pg_log_phase(((pg_handle_t *)pg_handle)->rank,
                 ((pg_handle_t *)pg_handle)->size,
                 3, 4, "Run requested operation");
    if (run_token) {
        PG_LOG_INFO("main", "Running ring token smoke test");
        rc = pg_ring_token(pg_handle, 1);
        PG_LOG_INFO("main", "Ring token smoke test %s", rc == 0 ? "passed" : "failed");
        pg_log_phase(((pg_handle_t *)pg_handle)->rank,
                     ((pg_handle_t *)pg_handle)->size,
                     4, 4, "Synchronize and release resources");
        free_process_group_hosts(host_list, host_count);
        pg_close(pg_handle);
        return rc == 0 ? 0 : 1;
    }

    PG_LOG_INFO("main",
                "No collective action requested; process group is ready and will close");

    pg_log_phase(((pg_handle_t *)pg_handle)->rank,
                 ((pg_handle_t *)pg_handle)->size,
                 4, 4, "Synchronize and release resources");
    free_process_group_hosts(host_list, host_count);
    pg_close(pg_handle);
    PG_LOG_INFO("main", "Process group closed successfully");
    return 0;
}
#endif /* PG_LIBRARY_ONLY */
