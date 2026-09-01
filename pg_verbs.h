#ifndef PG_VERBS_H
#define PG_VERBS_H

#include "pg_common.h"

/*
 * find_active_port:
 *  Find the first physical device port that is currently active.
 *
 * A device may expose multiple ports, and a valid device does not guarantee
 * that every port is usable. The selected port is needed when the QP enters
 * INIT and will also be used later when connecting the QP to a peer.
 */
int find_active_port(struct ibv_context *context, int *port_num);

/*
 * create_rdma_resources:
 *  Initialize all local RDMA resources for a process group.
 *
 *  Steps:
 *   - Discover and open the first available Verbs device
 *   - Find an active physical port
 *   - Allocate a protection domain
 *   - Allocate and register a communication buffer
 *   - Create a completion queue
 *   - Create an RC queue pair and move it to INIT
 *
 *  Returns:
 *   - 0 on success
 *   - -1 on failure
 */
int create_rdma_resources(pg_handle_t *pg);

/*
 * destroy_rdma_resources:
 *  Release every Verbs resource owned by a process group.
 *
 * Verbs objects are destroyed in reverse dependency order (QP, CQ, MR, PD, context).
 * This function is safe to call on partially initialized structures.
 */
void destroy_rdma_resources(pg_handle_t *pg);

#endif /* PG_VERBS_H */
