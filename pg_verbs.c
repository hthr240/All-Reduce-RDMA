#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <infiniband/verbs.h>
#include <string.h>

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

    /* Keep send and receive completions separate across collective calls. */
    PG_LOG_DEBUG("pg_verbs", "Creating directional completion queues (capacity=%d)", PG_CQ_CAPACITY);
    pg->send_cq = ibv_create_cq(pg->context, PG_CQ_CAPACITY, NULL, NULL, 0);
    pg->recv_cq = ibv_create_cq(pg->context, PG_CQ_CAPACITY, NULL, NULL, 0);
    if (!pg->send_cq || !pg->recv_cq) {
        PG_LOG_ERROR("pg_verbs", "Could not create directional completion queues");
        return -1;
    }
    pg->cq = pg->send_cq;

    /* Create one RC QP for each ring direction. */
    PG_LOG_DEBUG("pg_verbs", "Creating directional reliable-connected queue pairs");
    {
        struct ibv_qp_init_attr qp_attr = {
            .send_cq = pg->send_cq,
            .recv_cq = pg->recv_cq,
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

        pg->qp_send = ibv_create_qp(pg->pd, &qp_attr);
        pg->qp_recv = ibv_create_qp(pg->pd, &qp_attr);
        if (!pg->qp_send || !pg->qp_recv) {
            PG_LOG_ERROR("pg_verbs", "Could not create directional queue pairs");
            return -1;
        }
        pg->qp = pg->qp_send;
        PG_LOG_INFO("pg_verbs", "Queue pairs created: send=0x%x recv=0x%x",
                    pg->qp_send->qp_num, pg->qp_recv->qp_num);
    }

    /* A QP must be in INIT before it can be connected to a remote QP. */
    {
        struct ibv_qp *qps[] = {pg->qp_send, pg->qp_recv};
        size_t i;
        for (i = 0; i < sizeof(qps) / sizeof(qps[0]); ++i) {
            struct ibv_qp_attr qp_attr = {
                .qp_state = IBV_QPS_INIT,
                .pkey_index = 0,
                .port_num = (uint8_t)pg->ib_port,
                .qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
                                   IBV_ACCESS_REMOTE_READ
            };
            if (ibv_modify_qp(qps[i], &qp_attr,
                              IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                              IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
                PG_LOG_ERROR("pg_verbs", "Could not move directional QP to INIT");
                return -1;
            }
        }
        PG_LOG_INFO("pg_verbs", "Directional QPs moved to INIT successfully");
    }

    PG_LOG_INFO("pg_verbs", "All RDMA resources created successfully");
    return 0;
}

int connect_rdma_qp(pg_handle_t *pg, struct ibv_qp *qp,
                    uint32_t local_psn, const pg_metadata_t *remote)
{
    struct ibv_port_attr port_attr;
    struct ibv_qp_attr attr;
    int flags;

    if (!pg || !qp || !remote || !pg->context || remote->qpn == 0 ||
        remote->buffer_addr == 0) {
        return -1;
    }
    if (ibv_query_port(pg->context, (uint8_t)pg->ib_port, &port_attr) != 0) {
        return -1;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = port_attr.active_mtu;
    attr.dest_qp_num = remote->qpn;
    attr.rq_psn = remote->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = remote->gid.global.interface_id != 0;
    attr.ah_attr.dlid = remote->lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = (uint8_t)pg->ib_port;
    if (attr.ah_attr.is_global) {
        attr.ah_attr.grh.dgid = remote->gid;
        attr.ah_attr.grh.hop_limit = 1;
    }
    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
            IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(qp, &attr, flags) != 0) {
        PG_LOG_ERROR("pg_verbs", "Could not move QP 0x%x to RTR", qp->qp_num);
        return -1;
    }
    PG_LOG_DEBUG("pg_verbs", "QP 0x%x moved to RTR", qp->qp_num);

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = local_psn & 0x00ffffffu;
    attr.max_rd_atomic = 1;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &attr, flags) != 0) {
        PG_LOG_ERROR("pg_verbs", "Could not move QP 0x%x to RTS", qp->qp_num);
        return -1;
    }
    PG_LOG_INFO("pg_verbs", "QP 0x%x moved to RTS", qp->qp_num);
    return 0;
}

int ring_token(pg_handle_t *pg, int laps)
{
    unsigned char token = 0x5a;
    int lap;

    if (!pg || !pg->qp_send || !pg->qp_recv || !pg->mr || !pg->buf ||
        pg->size < 2 || laps < 1) {
        return -1;
    }

    for (lap = 0; lap < laps; ++lap) {
        struct ibv_sge sge = {
            .addr = (uintptr_t)pg->buf,
            .length = 1,
            .lkey = pg->mr->lkey
        };
        struct ibv_recv_wr recv_wr = {
            .wr_id = (uint64_t)lap,
            .sg_list = &sge,
            .num_sge = 1
        };
        struct ibv_recv_wr *bad_recv = NULL;
        struct ibv_send_wr send_wr = {
            .wr_id = (uint64_t)lap,
            .sg_list = &sge,
            .num_sge = 1,
            .opcode = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE
        };
        struct ibv_send_wr *bad_send = NULL;
        int received = 0;
        int sent = pg->rank == 0 ? 1 : 0;
        int send_done = 0;

        *(unsigned char *)pg->buf = token;

        if (ibv_post_recv(pg->qp_recv, &recv_wr, &bad_recv) != 0) {
            return -1;
        }
        if (pg->rank == 0) {
            if (ibv_post_send(pg->qp_send, &send_wr, &bad_send) != 0) {
                return -1;
            }
        }

        while (!received || !sent || !send_done) {
            struct ibv_wc wc[2];
            int count = ibv_poll_cq(pg->recv_cq, 1, &wc[0]);
            if (count < 0) {
                return -1;
            }
            if (count == 1) {
                if (wc[0].status != IBV_WC_SUCCESS ||
                    wc[0].byte_len != 1 || pg->buf == NULL) {
                    return -1;
                }
                if (*(unsigned char *)pg->buf != token) {
                    PG_LOG_ERROR("pg_verbs", "Ring token payload mismatch on lap %d", lap);
                    return -1;
                }
                received = 1;
                if (!sent && ibv_post_send(pg->qp_send, &send_wr, &bad_send) != 0) {
                    return -1;
                }
                sent = 1;
            }

            count = ibv_poll_cq(pg->send_cq, 1, &wc[1]);
            if (count < 0) {
                return -1;
            }
            if (count == 1) {
                if (wc[1].status != IBV_WC_SUCCESS) {
                    return -1;
                }
                send_done = 1;
            }
        }
        PG_LOG_DEBUG("pg_verbs", "Ring token lap %d completed", lap + 1);
    }
    return 0;
}

void destroy_rdma_resources(pg_handle_t *pg)
{
    if (!pg) {
        return;
    }

    if (pg->qp_recv) {
        ibv_destroy_qp(pg->qp_recv);
    }
    if (pg->qp_send) {
        ibv_destroy_qp(pg->qp_send);
    }
    if (pg->recv_cq) {
        ibv_destroy_cq(pg->recv_cq);
    }
    if (pg->send_cq) {
        ibv_destroy_cq(pg->send_cq);
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
