#define _POSIX_C_SOURCE 200809L

/*
 * Setup test:
 *  Verify the public handle API and the local RDMA Verbs initialization.
 *
 * This test uses the private header to inspect local resource state. The
 * production API remains opaque to normal callers.
 *
 * The Verbs portion is skipped when no RDMA device is visible. This lets the
 * same test run on development machines without RDMA hardware while still
 * exercising the full initialization on a course node.
 */
#define PG_INTERNAL
#include "../pg.h"

#include <stdio.h>
#include <stdlib.h>

static int test_public_api_failures(void)
{
    int value = 0;

    /* Invalid arguments must fail without attempting RDMA initialization. */
    if (connect_process_group(NULL, NULL) != -1) {
        fprintf(stderr, "connect_process_group accepted a NULL output pointer\n");
        return -1;
    }
    if (pg_all_reduce(&value, &value, 1, PG_INT32, PG_SUM, NULL) != -1) {
        fprintf(stderr, "pg_all_reduce accepted a NULL process-group handle\n");
        return -1;
    }
    if (pg_close(NULL) != 0) {
        fprintf(stderr, "pg_close rejected a NULL process-group handle\n");
        return -1;
    }

    return 0;
}

static int test_local_verbs_setup(void)
{
    void *handle = NULL;
    pg_handle_t *pg;
    int device_count = 0;
    struct ibv_device **devices;
    int rc;

    /* Check availability before calling the hardware-dependent API. */
    devices = ibv_get_device_list(&device_count);
    if (!devices || device_count == 0) {
        fprintf(stderr, "SKIP: no RDMA device available for local Verbs test\n");
        if (devices) {
            ibv_free_device_list(devices);
        }
        return 0;
    }
    ibv_free_device_list(devices);

    /* This exercises the complete local setup path on an RDMA-capable host. */
    rc = connect_process_group("local-test", &handle);
    if (rc != 0) {
        fprintf(stderr, "connect_process_group failed with an RDMA device\n");
        return -1;
    }

    /* Every local resource must exist and the QP must be ready for peering. */
    pg = (pg_handle_t *)handle;
    if (!pg->context || !pg->pd || !pg->send_cq || !pg->recv_cq ||
        !pg->qp_send || !pg->qp_recv || !pg->mr ||
        !pg->buf || pg->buf_size != PG_REGISTERED_BUFFER_SIZE(pg->size) ||
        pg->ib_port <= 0 ||
        pg->qp_send->state != IBV_QPS_INIT ||
        pg->qp_recv->state != IBV_QPS_INIT || !pg->is_connected) {
        fprintf(stderr, "local Verbs handle is incomplete or not in INIT\n");
        pg_close(handle);
        return -1;
    }

    pg_close(handle);
    return 0;
}

int main(void)
{
    /* Keep the test result based on return values, not diagnostic text. */
    if (test_public_api_failures() != 0 || test_local_verbs_setup() != 0) {
        return EXIT_FAILURE;
    }

    printf("Setup tests passed\n");
    return EXIT_SUCCESS;
}
