#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PG_INTERNAL
#include "pg.h"

static pg_log_level_t g_log_level = PG_LOG_INFO;

void pg_log_set_level(pg_log_level_t level)
{
    g_log_level = level;
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
    free(pg);
}

/* Returns 0 when unset, 1 when parsed into *out, -1 on an invalid value. */
static int parse_env_int(const char *name, long minimum, long maximum,
                         int *out)
{
    const char *text = getenv(name);
    char *end = NULL;
    long value;

    if (!text) {
        return 0;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < minimum || value > maximum) {
        fprintf(stderr, "Invalid %s value: %s\n", name, text);
        return -1;
    }
    *out = (int)value;
    return 1;
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
    pg->gid_index = -1;
    pg->bootstrap_base_port = PG_BOOTSTRAP_BASE_PORT;
    no_pipeline = getenv("PG_NOPIPE");
    pg->pipeline_enabled = !no_pipeline || strcmp(no_pipeline, "1") != 0;
    if (parse_env_int("PG_GID_IDX", 0, 255, &pg->gid_index) < 0 ||
        parse_env_int("PG_TCP_PORT", 1024, 65535,
                      &pg->bootstrap_base_port) < 0) {
        return -1;
    }
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
 *  Build the ring: allocate the handle, open the Verbs device, create the
 *  registered buffer and both directional RC QPs, then run the TCP bootstrap
 *  that exchanges QP metadata with the ring neighbours and drives the QPs to
 *  RTS. On success the handle is fully connected and ready for collectives.
 *
 *  A bare hostname (no ':' group spec) yields a local single-process handle
 *  with no fabric connection, which the unit tests rely on.
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

    if (configure_process_group_topology(pg, rank, host_count) != 0) {
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
 *  Element-wise reduction of count elements across all ranks; every rank
 *  ends with the full reduced vector in recvbuf. Runs ring Reduce Scatter
 *  followed by ring All Gather over the transport picked by PG_MODE and the
 *  eager threshold. sendbuf == recvbuf (in place) is allowed.
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
    pg_transport_mode_t mode;

    if (!pg || count < 0 ||
        pg_validate_reduction(datatype, op) != 0 ||
        pg->size <= 0 || pg->rank < 0 || pg->rank >= pg->size) {
        fprintf(stderr, "Invalid all-reduce arguments\n");
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (!sendbuf || !recvbuf) {
        fprintf(stderr, "Invalid all-reduce buffers\n");
        return -1;
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

    mode = use_rendezvous_transport(pg, count, element_size) ?
           PG_TRANSPORT_RDVZ : PG_TRANSPORT_EAGER;
    if (pg_run_all_reduce(pg, sendbuf, recvbuf, count,
                          datatype, op, mode) != 0) {
        fprintf(stderr, "All-reduce failed\n");
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
    memcpy((unsigned char *)pg->buf + pg->work_offset +
               (size_t)owned_offset * element_size,
           sendbuf, (size_t)owned_count * element_size);
    if (use_rendezvous_transport(pg, count, element_size)) {
        return pg_run_rendezvous_all_gather(pg, recvbuf, count, datatype);
    }
    return pg_run_eager_all_gather(pg, recvbuf, count, datatype);
}

/* Connectivity smoke test: circulate a one-byte token around the ring. */
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
 *  Barrier with the ring neighbours so no rank tears down while a peer is
 *  still mid-collective, then release every socket and Verbs resource in
 *  reverse dependency order.
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
