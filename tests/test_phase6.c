#define _POSIX_C_SOURCE 200809L

/* Phase 6: bounded eager collective and public API checks. */
#include "../pg_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_invalid_eager_transport(void)
{
    pg_handle_t pg = {0};
    int value = 1;

    if (post_eager_receive(NULL, 1, 0) != -1 ||
        post_eager_send(NULL, &value, sizeof(value), 0, 0) != -1 ||
        poll_eager_completion(NULL, 1, NULL) != -1 ||
        pg_reduce_scatter(NULL, &value, 1, PG_INT32, PG_SUM, &pg) != -1 ||
        pg_run_eager_all_gather(NULL, &value, 1, PG_INT32) != -1 ||
        pg_all_reduce(NULL, &value, 1, PG_INT32, PG_SUM, &pg) != -1 ||
        pg_all_reduce(&value, NULL, 1, PG_INT32, PG_SUM, &pg) != -1 ||
        pg_all_gather(NULL, &value, 1, PG_INT32, &pg) != -1 ||
        pg_all_gather(&value, NULL, 1, PG_INT32, &pg) != -1) {
        fprintf(stderr, "invalid eager transport arguments were accepted\n");
        return -1;
    }
    return 0;
}

static int test_public_chunk_api(void)
{
    pg_handle_t pg = {0};
    int offset;
    int nelem;

    pg.rank = 1;
    pg.size = 4;
    if (pg_chunk(&pg, 10, &offset, &nelem) != 0 ||
        offset != 6 || nelem != 2 || pg_rank(&pg) != 1 ||
        pg_nranks(&pg) != 4) {
        fprintf(stderr, "public chunk API returned incorrect geometry\n");
        return -1;
    }
    return 0;
}

static int test_all_gather_schedule(void)
{
    const int expected_send[] = {1, 0, 3};
    const int expected_receive[] = {0, 3, 2};
    int step;

    for (step = 0; step < 3; ++step) {
        int global_step = 3 + step;

        if (pg_send_chunk(0, global_step, 4) != expected_send[step] ||
            pg_receive_chunk(0, global_step, 4) != expected_receive[step]) {
            fprintf(stderr, "All Gather ring schedule is incorrect\n");
            return -1;
        }
    }
    return 0;
}

static int test_single_rank_all_reduce(void)
{
    pg_handle_t pg = {0};
    int input[] = {2, 3, 5};
    int output[] = {0, 0, 0};

    pg.rank = 0;
    pg.size = 1;
    if (pg_all_reduce(input, output, 3, PG_INT32, PG_SUM, &pg) != 0 ||
        memcmp(input, output, sizeof(input)) != 0 ||
        pg_all_reduce(input, input, 3, PG_INT32, PG_PROD, &pg) != 0 ||
        input[0] != 2 || input[1] != 3 || input[2] != 5) {
        fprintf(stderr, "single-rank all-reduce failed\n");
        return -1;
    }
    return 0;
}

static int test_single_rank_all_gather(void)
{
    pg_handle_t pg = {0};
    double input[] = {1.5, 2.5};
    double output[] = {0.0, 0.0};

    pg.rank = 0;
    pg.size = 1;
    if (pg_all_gather(input, output, 2, PG_DOUBLE, &pg) != 0 ||
        memcmp(input, output, sizeof(input)) != 0) {
        fprintf(stderr, "single-rank All Gather failed\n");
        return -1;
    }
    return 0;
}

static int test_eager_work_buffer_limit(void)
{
    pg_handle_t pg = {0};
    unsigned char workspace[PG_BUFFER_SIZE];
    int input[8193] = {0};
    int output[8193] = {0};

    pg.rank = 0;
    pg.size = 2;
    pg.is_connected = 1;
    pg.buf = workspace;
    if (pg_run_eager_reduce_scatter(&pg, input, output, 8193,
                                    PG_INT32, PG_SUM) != -1 ||
        pg_run_eager_all_gather(&pg, output, 8193, PG_INT32) != -1) {
        fprintf(stderr, "eager transfer exceeded the work buffer\n");
        return -1;
    }
    return 0;
}

static int test_eager_sequence_progression(void)
{
    pg_handle_t pg = {0};

    pg.collective_sequence = UINT8_MAX;
    ++pg.collective_sequence;
    if (pg.collective_sequence != 0 ||
        PG_EAGER_IMM(7, 3, 2) != UINT32_C(0x07030002)) {
        fprintf(stderr, "eager collective sequence encoding is incorrect\n");
        return -1;
    }
    return 0;
}

int main(void)
{
    if (test_invalid_eager_transport() != 0 ||
        test_public_chunk_api() != 0 ||
        test_all_gather_schedule() != 0 ||
        test_single_rank_all_reduce() != 0 ||
        test_single_rank_all_gather() != 0 ||
        test_eager_work_buffer_limit() != 0 ||
        test_eager_sequence_progression() != 0) {
        return EXIT_FAILURE;
    }
    printf("Phase 6 eager transport tests passed\n");
    return EXIT_SUCCESS;
}