#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pg.h"
#include "pg_common.h"
#include "pg_log.h"
#include "pg_verbs.h"
#include "pg_bootstrap.h"
#include "pg_collective.h"
#include "pg_reduction.h"
#include "pg_topology.h"
#include "pg_cli.h"

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

    if (!pg || count < 0 || pg_validate_reduction(datatype, op) != 0) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (!sendbuf || !recvbuf) {
        return -1;
    }
    return pg_run_eager_reduce_scatter(pg, sendbuf, recvbuf, count,
                                       datatype, op);
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
    char *hostname = NULL;
    void *pg_handle = NULL;
    int rc;
    int run_token = 0;

    pg_log_phase(-1, 0, 1, 6, "Parse process-group configuration");
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
        free(host_list);
        return 1;
    }

    /* Distributed mode: validate the complete rank-to-host mapping first. */
    if (myindex >= 0 && host_count > 0) {
        PG_LOG_INFO("main", "Distributed mode: rank=%d, group_size=%d", myindex, host_count);
        if (validate_host_list(host_list, host_count) != 0) {
            PG_LOG_ERROR("main", "Host list validation failed");
            free(host_list);
            return 1;
        }
        if (myindex >= host_count) {
            PG_LOG_ERROR("main", "-myindex %d out of range for group size %d", myindex, host_count);
            free(host_list);
            return 1;
        }
        hostname = host_list[myindex];
    /* Standalone mode represents a one-rank group for local development. */
    } else if (argc == 2) {
        PG_LOG_INFO("main", "Standalone mode: hostname=%s", argv[1]);
        hostname = argv[1];
    } else {
        PG_LOG_ERROR("main", "Invalid argument combination");
        usage(argv[0]);
        free(host_list);
        return 1;
    }

    pg_log_phase(myindex >= 0 ? myindex : 0,
                 myindex >= 0 ? host_count : 1,
                 2, 6, "Create local RDMA resources");
    /* Create local RDMA state before adding the ring metadata to the handle. */
    PG_LOG_DEBUG("main", "Connecting process group (hostname=%s)", hostname);
    rc = connect_process_group(hostname, &pg_handle);
    if (rc != 0) {
        PG_LOG_ERROR("main", "Failed to initialize process group");
        free(host_list);
        return 1;
    }
    PG_LOG_INFO("main", "Process group connected successfully");

    pg_log_phase(myindex >= 0 ? myindex : 0,
                 myindex >= 0 ? host_count : 1,
                 3, 6, "Configure logical ring topology");
    /* The host-list rank and size now become part of the opaque handle. */
    PG_LOG_DEBUG("main", "Configuring process group topology");
    if (configure_process_group_topology((pg_handle_t *)pg_handle,
                                         myindex >= 0 ? myindex : 0,
                                         myindex >= 0 ? host_count : 1) != 0) {
        PG_LOG_ERROR("main", "Failed to configure process group topology");
        pg_close(pg_handle);
        free(host_list);
        return 1;
    }
    PG_LOG_INFO("main",
                "Topology ready: rank=%d size=%d previous=%d next=%d",
                ((pg_handle_t *)pg_handle)->rank,
                ((pg_handle_t *)pg_handle)->size,
                ((pg_handle_t *)pg_handle)->previous_rank,
                ((pg_handle_t *)pg_handle)->next_rank);

    pg_log_phase(((pg_handle_t *)pg_handle)->rank,
                 ((pg_handle_t *)pg_handle)->size,
                 4, 6, "Connect TCP bootstrap ring and RDMA queue pairs");
    if (myindex >= 0 && host_count > 1) {
        PG_LOG_DEBUG("main", "Starting ring bootstrap for multi-rank group");
        if (bootstrap_ring((pg_handle_t *)pg_handle, host_list, host_count) != 0) {
            PG_LOG_ERROR("main", "Failed to bootstrap ring peers");
            pg_close(pg_handle);
            free(host_list);
            return 1;
        }
        PG_LOG_INFO("main", "Ring bootstrap completed successfully");
    } else {
        PG_LOG_INFO("main", "Standalone or single-rank mode - skipping bootstrap");
    }

    pg_log_phase(((pg_handle_t *)pg_handle)->rank,
                 ((pg_handle_t *)pg_handle)->size,
                 5, 6, "Run requested operation");
    if (run_token) {
        PG_LOG_INFO("main", "Running ring token smoke test");
        rc = pg_ring_token(pg_handle, 1);
        PG_LOG_INFO("main", "Ring token smoke test %s", rc == 0 ? "passed" : "failed");
        pg_log_phase(((pg_handle_t *)pg_handle)->rank,
                     ((pg_handle_t *)pg_handle)->size,
                     6, 6, "Synchronize and release resources");
        free(host_list);
        pg_close(pg_handle);
        return rc == 0 ? 0 : 1;
    }

    PG_LOG_INFO("main",
                "No collective action requested; process group is ready and will close");

    pg_log_phase(((pg_handle_t *)pg_handle)->rank,
                 ((pg_handle_t *)pg_handle)->size,
                 6, 6, "Synchronize and release resources");
    free(host_list);
    pg_close(pg_handle);
    PG_LOG_INFO("main", "Process group closed successfully");
    return 0;
}
#endif /* PG_LIBRARY_ONLY */
