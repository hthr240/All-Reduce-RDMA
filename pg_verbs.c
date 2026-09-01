#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <infiniband/verbs.h>

#include "pg_common.h"
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
        fprintf(stderr, "Invalid process-group handle\n");
        return -1;
    }

    /* Ask libibverbs which RDMA devices are visible on this host. */
    device_list = ibv_get_device_list(&device_count);
    if (!device_list || device_count == 0) {
        fprintf(stderr, "No InfiniBand Verbs device found\n");
        if (device_list) {
            ibv_free_device_list(device_list);
        }
        return -1;
    }

    /* Device selection is intentionally simple for this first milestone. */
    pg->device = device_list[0];

    /* The context is the process's active handle for using the device. */
    pg->context = ibv_open_device(pg->device);
    if (!pg->context) {
        fprintf(stderr, "Could not open Verbs device %s\n",
                ibv_get_device_name(pg->device));
        ibv_free_device_list(device_list);
        return -1;
    }
    /* The opened context remains valid after this temporary list is freed. */
    ibv_free_device_list(device_list);
    device_list = NULL;

    if (find_active_port(pg->context, &pg->ib_port) != 0) {
        return -1;
    }

    /* The PD groups the QP and memory region under one access boundary. */
    pg->pd = ibv_alloc_pd(pg->context);
    if (!pg->pd) {
        fprintf(stderr, "Could not allocate Verbs protection domain\n");
        return -1;
    }

    /* This buffer is a placeholder for future send/receive/chunk buffers. */
    pg->buf = calloc(1, PG_BUFFER_SIZE);
    pg->buf_size = PG_BUFFER_SIZE;
    if (!pg->buf) {
        fprintf(stderr, "Could not allocate process-group buffer\n");
        return -1;
    }

    /* Registration makes the buffer accessible to the RDMA hardware. */
    pg->mr = ibv_reg_mr(pg->pd, pg->buf, pg->buf_size,
                        IBV_ACCESS_LOCAL_WRITE |
                        IBV_ACCESS_REMOTE_WRITE |
                        IBV_ACCESS_REMOTE_READ);
    if (!pg->mr) {
        fprintf(stderr, "Could not register process-group buffer\n");
        return -1;
    }

    /* The CQ will report completion of future SEND/RECV/RDMA operations. */
    pg->cq = ibv_create_cq(pg->context, PG_CQ_CAPACITY, NULL, NULL, 0);
    if (!pg->cq) {
        fprintf(stderr, "Could not create process-group completion queue\n");
        return -1;
    }

    /* Create one RC QP; later phases will connect it to a ring neighbor. */
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
            fprintf(stderr, "Could not create process-group queue pair\n");
            return -1;
        }
    }

    /* A QP must be in INIT before it can be connected to a remote QP. */
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
            fprintf(stderr, "Could not move process-group queue pair to INIT\n");
            return -1;
        }
    }

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
