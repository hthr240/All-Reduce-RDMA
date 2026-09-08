#define _POSIX_C_SOURCE 200809L

/*
 * Phase 4 test:
 *  Verify the directional RDMA transport layout and invalid token handling.
 *  The complete QP connection and token test require two live RDMA ranks and
 *  are exercised with the executable's -token action on course nodes.
 */
#define main ring_allreduce_program_main
#include "../ring_allreduce.c"
#undef main

#include <stdio.h>
#include <stdlib.h>

static int test_invalid_token_arguments(void)
{
    pg_handle_t pg = {0};

    if (ring_token(NULL, 1) != -1 ||
        ring_token(&pg, 0) != -1 ||
        pg_ring_token(NULL, 1) != -1) {
        fprintf(stderr, "invalid ring token arguments were accepted\n");
        return -1;
    }
    return 0;
}

static int test_directional_resources(void)
{
    void *handle = NULL;
    pg_handle_t *pg;
    struct ibv_device **devices;
    int device_count = 0;

    devices = ibv_get_device_list(&device_count);
    if (!devices || device_count == 0) {
        fprintf(stderr, "SKIP: no RDMA device available for Phase 4 test\n");
        if (devices) {
            ibv_free_device_list(devices);
        }
        return 0;
    }
    ibv_free_device_list(devices);

    if (connect_process_group("phase4-test", &handle) != 0) {
        fprintf(stderr, "connect_process_group failed with an RDMA device\n");
        return -1;
    }

    pg = (pg_handle_t *)handle;
    if (!pg->send_cq || !pg->recv_cq || !pg->qp_send || !pg->qp_recv ||
        pg->qp_send->state != IBV_QPS_INIT ||
        pg->qp_recv->state != IBV_QPS_INIT ||
        pg->qp != pg->qp_send || pg->cq != pg->send_cq) {
        fprintf(stderr, "directional RDMA resources are incomplete\n");
        pg_close(handle);
        return -1;
    }

    pg_close(handle);
    return 0;
}

int main(void)
{
    if (test_invalid_token_arguments() != 0 ||
        test_directional_resources() != 0) {
        return EXIT_FAILURE;
    }

    printf("Phase 4 transport tests passed\n");
    return EXIT_SUCCESS;
}