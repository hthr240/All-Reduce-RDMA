#define _POSIX_C_SOURCE 200809L

/* Rendezvous transport test: staging geometry, validation, and size limits. */
#include "../pg_internal.h"

#include <stdio.h>
#include <stdlib.h>

static int test_invalid_rendezvous_transport(void)
{
    pg_handle_t pg = {0};
    int value = 1;

    if (post_rendezvous_write(NULL, &value, sizeof(value), 0, 0, 0) != -1 ||
        pg_run_rendezvous_reduce_scatter(NULL, &value, &value, 1,
                                         PG_INT32, PG_SUM) != -1 ||
        pg_run_rendezvous_reduce_scatter(&pg, NULL, &value, 1,
                                         PG_INT32, PG_SUM) != -1 ||
        pg_run_rendezvous_all_gather(NULL, &value, 1, PG_INT32) != -1 ||
        pg_run_rendezvous_all_gather(&pg, NULL, 1, PG_INT32) != -1) {
        fprintf(stderr, "invalid rendezvous transport arguments were accepted\n");
        return -1;
    }
    return 0;
}

/* Every chunk of a full work buffer must fit one staging slot, for both
 * element sizes and for group sizes that do not divide the buffer evenly. */
static int test_staging_geometry(void)
{
    int size;

    for (size = 2; size <= 8; ++size) {
        size_t slot = PG_RDVZ_SLOT_SIZE(size);
        int int_count = (int)(PG_WORK_BUFFER_SIZE / sizeof(int32_t));
        int double_count = (int)(PG_WORK_BUFFER_SIZE / sizeof(double));
        size_t largest_int_chunk =
            (size_t)pg_chunk_nelem(int_count, size, 0) * sizeof(int32_t);
        size_t largest_double_chunk =
            (size_t)pg_chunk_nelem(double_count, size, 0) * sizeof(double);

        if (largest_int_chunk > slot || largest_double_chunk > slot) {
            fprintf(stderr,
                    "staging slot too small for %d ranks: slot=%zu int=%zu double=%zu\n",
                    size, slot, largest_int_chunk, largest_double_chunk);
            return -1;
        }
    }
    return 0;
}

static int test_rendezvous_work_buffer_limit(void)
{
    pg_handle_t pg = {0};
    /* Never touched: the size validation fires before any buffer access. */
    static unsigned char workspace[16];
    int too_many = (int)(PG_WORK_BUFFER_SIZE / sizeof(int32_t)) + 1;
    int dummy = 0;

    pg.rank = 0;
    pg.size = 2;
    pg.is_connected = 1;
    pg.buf = workspace;
    if (pg_run_rendezvous_reduce_scatter(&pg, &dummy, &dummy, too_many,
                                         PG_INT32, PG_SUM) != -1 ||
        pg_run_rendezvous_all_gather(&pg, &dummy, too_many, PG_INT32) != -1) {
        fprintf(stderr, "rendezvous transfer exceeded the work buffer\n");
        return -1;
    }
    return 0;
}

int main(void)
{
    if (test_invalid_rendezvous_transport() != 0 ||
        test_staging_geometry() != 0 ||
        test_rendezvous_work_buffer_limit() != 0) {
        return EXIT_FAILURE;
    }
    printf("Rendezvous transport tests passed\n");
    return EXIT_SUCCESS;
}
