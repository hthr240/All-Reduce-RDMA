#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>

#include "pg_common.h"
#include "pg_log.h"
#include "pg_verbs.h"
#include "pg_bootstrap.h"
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

    /* Without this output address there is nowhere to return the new handle. */
    if (!pg_handle) {
        fprintf(stderr, "Invalid process-group handle pointer\n");
        return -1;
    }

    /* calloc gives every resource pointer a known NULL value for cleanup. */
    pg = calloc(1, sizeof(*pg));
    if (!pg) {
        fprintf(stderr, "Could not allocate process-group handle\n");
        return -1;
    }

    /* Rank and group size are placeholders until the topology phase. */
    pg->rank = 0;
    pg->size = 1;
    pg->is_connected = 0;

    /* Keep an owned hostname copy; the caller retains ownership of its input. */
    pg->hostname = servername ? strdup(servername) : NULL;
    if (servername && !pg->hostname) {
        fprintf(stderr, "Could not copy process-group hostname\n");
        destroy_process_group(pg);
        return -1;
    }

    /* Initialize all RDMA resources via the verbs module. */
    if (create_rdma_resources(pg) != 0) {
        destroy_process_group(pg);
        return -1;
    }

    /* Only local initialization is complete; remote connection comes later. */
    pg->is_connected = 1;

    *pg_handle = pg;
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

    (void)sendbuf;
    (void)recvbuf;
    (void)count;
    (void)datatype;
    (void)op;

    if (!pg) {
        fprintf(stderr, "Invalid process-group handle\n");
        return -1;
    }

    fprintf(stderr, "pg_all_reduce not implemented in the skeleton build\n");
    return -1;
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

    PG_LOG_INFO("main", "Starting ring_allreduce (argc=%d)", argc);

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

    /* Create local RDMA state before adding the ring metadata to the handle. */
    PG_LOG_DEBUG("main", "Connecting process group (hostname=%s)", hostname);
    rc = connect_process_group(hostname, &pg_handle);
    if (rc != 0) {
        PG_LOG_ERROR("main", "Failed to initialize process group");
        free(host_list);
        return 1;
    }
    PG_LOG_INFO("main", "Process group connected successfully");

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
    PG_LOG_INFO("main", "Topology configured: rank=%d, size=%d",
                ((pg_handle_t *)pg_handle)->rank,
                ((pg_handle_t *)pg_handle)->size);

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

    PG_LOG_INFO("main", "Exercise 3 local Verbs state initialized for rank %d",
                ((pg_handle_t *)pg_handle)->rank);

    free(host_list);
    pg_close(pg_handle);
    PG_LOG_INFO("main", "Process group closed successfully");
    return 0;
}
