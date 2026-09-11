#define _POSIX_C_SOURCE 200809L

/* Phase 6: eager transport argument and public Reduce Scatter checks. */
#define main ring_allreduce_program_main
#include "../ring_allreduce.c"
#undef main

#include <stdio.h>
#include <stdlib.h>

static int test_invalid_eager_transport(void)
{
    pg_handle_t pg = {0};
    int value = 1;

    if (post_eager_receive(NULL, 1, 0) != -1 ||
        post_eager_send(NULL, &value, sizeof(value), 0, 0) != -1 ||
        poll_eager_completion(NULL, 1, NULL) != -1 ||
        pg_reduce_scatter(NULL, &value, 1, PG_INT32, PG_SUM, &pg) != -1 ||
        pg_run_eager_all_gather(NULL, &value, 1, PG_INT32) != -1) {
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

int main(void)
{
    if (test_invalid_eager_transport() != 0 ||
        test_public_chunk_api() != 0) {
        return EXIT_FAILURE;
    }
    printf("Phase 6 eager transport tests passed\n");
    return EXIT_SUCCESS;
}