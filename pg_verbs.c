#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <infiniband/verbs.h>

#include "pg_common.h"
#include "pg_log.h"
#include "pg_verbs.h"

int find_active_port(struct ibv_context *context, int *port_num)
{
    struct ibv_device_attr device_attr;
    struct ibv_port_attr port_attr;
    int port;

    if (ibv_query_device(context, &device_attr) != 0) {
        fprintf(stderr, "Could not query device attributes\n");
        return -1;
    }

    for (port = 1; port <= device_attr.phys_port_cnt; ++port) {
        if (ibv_query_port(context, (uint8_t)port, &port_attr) == 0 &&
            port_attr.state == IBV_PORT_ACTIVE) {
            *port_num = port;
            return 0;
        }
    }

    fprintf(stderr, "No active Verbs port found\n");
    return -1;
}

int create_rdma_resources(pg_handle_t *pg)
{
    struct ibv_device **device_list = NULL;
    int device_count = 0;

    if (!pg) {
        PG_LOG_ERROR("pg_verbs", "Invalid process-group handle");
        return -1;
    }

    /* Ask libibverbs which RDMA devices are visible on this host. */
    PG_LOG_DEBUG("pg_verbs", "Discovering RDMA devices");
    device_list = ibv_get_device_list(&device_count);
    if (!device_list || device_count == 0) {
        PG_LOG_ERROR("pg_verbs", "No InfiniBand Verbs device found");
        if (device_list) {
            ibv_free_device_list(device_list);
        }
        return -1;
    }
    PG_LOG_INFO("pg_verbs", "Found %d RDMA device(s)", device_count);

    /* Device selection is intentionally simple for this first milestone. */
    pg->device = device_list[0];
    PG_LOG_DEBUG("pg_verbs", "Selected device: %s", ibv_get_device_name(pg->device));

    /* The context is the process's active handle for using the device. */
    pg->context = ibv_open_device(pg->device);
    if (!pg->context) {
        PG_LOG_ERROR("pg_verbs", "Could not open Verbs device %s", ibv_get_device_name(pg->device));
        ibv_free_device_list(device_list);
        return -1;
    }
    PG_LOG_DEBUG("pg_verbs", "Device context opened successfully");
    /* The opened context remains valid after this temporary list is freed. */
    ibv_free_device_list(device_list);
    device_list = NULL;

    PG_LOG_DEBUG("pg_verbs", "Finding active port");
    if (find_active_port(pg->context, &pg->ib_port) != 0) {
        PG_LOG_ERROR("pg_verbs", "Could not find active port");
        return -1;
    }
    PG_LOG_INFO("pg_verbs", "Active port found: %d", pg->ib_port);

    /* The PD groups the QP and memory region under one access boundary. */
    PG_LOG_DEBUG("pg_verbs", "Allocating protection domain");
    pg->pd = ibv_alloc_pd(pg->context);
    if (!pg->pd) {
        PG_LOG_ERROR("pg_verbs", "Could not allocate Verbs protection domain");
        return -1;
    }
    PG_LOG_DEBUG("pg_verbs", "Protection domain allocated");

    /* This buffer is a placeholder for future send/receive/chunk buffers. */
    PG_LOG_DEBUG("pg_verbs", "Allocating %zu byte buffer", (size_t)PG_BUFFER_SIZE);
    pg->buf = calloc(1, PG_BUFFER_SIZE);
    pg->buf_size = PG_BUFFER_SIZE;
    if (!pg->buf) {
        PG_LOG_ERROR("pg_verbs", "Could not allocate process-group buffer");
        return -1;
    }
    PG_LOG_DEBUG("pg_verbs", "Buffer allocated at %p", pg->buf);

    /* Registration makes the buffer accessible to the RDMA hardware. */
    PG_LOG_DEBUG("pg_verbs", "Registering memory region");
    pg->mr = ibv_reg_mr(pg->pd, pg->buf, pg->buf_size,
                        IBV_ACCESS_LOCAL_WRITE |
                        IBV_ACCESS_REMOTE_WRITE |
                        IBV_ACCESS_REMOTE_READ);
    if (!pg->mr) {
        PG_LOG_ERROR("pg_verbs", "Could not register process-group buffer");
        return -1;
    }
    PG_LOG_INFO("pg_verbs", "Memory region registered: rkey=0x%x, lkey=0x%x",
                pg->mr->rkey, pg->mr->lkey);

    /* The CQ will report completion of future SEND/RECV/RDMA operations. */
    PG_LOG_DEBUG("pg_verbs", "Creating completion queue (capacity=%d)", PG_CQ_CAPACITY);
    pg->cq = ibv_create_cq(pg->context, PG_CQ_CAPACITY, NULL, NULL, 0);
    if (!pg->cq) {
        PG_LOG_ERROR("pg_verbs", "Could not create process-group completion queue");
        return -1;
    }
    PG_LOG_DEBUG("pg_verbs", "Completion queue created");

    /* Create one RC QP; later phases will connect it to a ring neighbor. */
    PG_LOG_DEBUG("pg_verbs", "Creating reliable-connected queue pair");
    {
        struct ibv_qp_init_attr qp_attr = {
            .send_cq = pg->cq,
            .recv_cq = pg->cq,
            .cap = {
                .max_send_wr = PG_QP_DEPTH,
                .max_recv_wr = PG_QP_DEPTH,
                .max_send_sge = 1,
                .max_recv_sge = 1,
                .max_inline_data = 0
            },
            .qp_type = IBV_QPT_RC,
            .sq_sig_all = 1
        };

        pg->qp = ibv_create_qp(pg->pd, &qp_attr);
        if (!pg->qp) {
            PG_LOG_ERROR("pg_verbs", "Could not create process-group queue pair");
            return -1;
        }
        PG_LOG_INFO("pg_verbs", "Queue pair created: qp_num=0x%x", pg->qp->qp_num);
    }

    /* A QP must be in INIT before it can be connected to a remote QP. */
    PG_LOG_DEBUG("pg_verbs", "Moving QP to INIT state");
    {
        struct ibv_qp_attr qp_attr = {
            .qp_state = IBV_QPS_INIT,
            .pkey_index = 0,
            .port_num = (uint8_t)pg->ib_port,
            .qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
                               IBV_ACCESS_REMOTE_READ
        };

        if (ibv_modify_qp(pg->qp, &qp_attr,
                          IBV_QP_STATE |
                          IBV_QP_PKEY_INDEX |
                          IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS) != 0) {
            PG_LOG_ERROR("pg_verbs", "Could not move process-group queue pair to INIT");
            return -1;
        }
        PG_LOG_INFO("pg_verbs", "QP moved to INIT state successfully");
    }

    PG_LOG_INFO("pg_verbs", "All RDMA resources created successfully");
    return 0;
}

void destroy_rdma_resources(pg_handle_t *pg)
{
    if (!pg) {
        return;
    }

    if (pg->qp) {
        ibv_destroy_qp(pg->qp);
    }
    if (pg->cq) {
        ibv_destroy_cq(pg->cq);
    }
    if (pg->mr) {
        ibv_dereg_mr(pg->mr);
    }
    if (pg->pd) {
        ibv_dealloc_pd(pg->pd);
    }
    if (pg->context) {
        ibv_close_device(pg->context);
    }

    free(pg->buf);
}
