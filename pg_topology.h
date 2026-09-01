#ifndef PG_TOPOLOGY_H
#define PG_TOPOLOGY_H

#include "pg_common.h"

/*
 * validate_host_list:
 *  Check the host list before it is used to define the process-group layout.
 *
 *  The list order is significant: list[i] identifies rank i. Every host must
 *  be non-empty and appear only once so that all ranks can agree on one ring.
 */
int validate_host_list(char **host_list, int host_count);

/*
 * configure_process_group_topology:
 *  Store a rank's position in the logical ring and calculate its neighbors.
 *
 *  The modulo operation makes the ring wrap around: rank 0 receives from the
 *  last rank, and the last rank sends to rank 0. This function only configures
 *  local metadata; it does not open sockets or communicate with peers.
 */
int configure_process_group_topology(pg_handle_t *pg, int rank, int size);

#endif /* PG_TOPOLOGY_H */
