#define _POSIX_C_SOURCE 200809L

/* Phase 5: reference-compatible reduction and uneven chunk geometry. */
#define main ring_allreduce_program_main
#include "../ring_allreduce.c"
#undef main

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../pg_reduction.h"

static int test_chunking(void)
{
    const int expected_lengths[] = {3, 3, 2, 2};
    const int expected_offsets[] = {0, 3, 6, 8};
    int chunk;

    for (chunk = 0; chunk < 4; ++chunk) {
        if (pg_chunk_nelem(10, 4, chunk) != expected_lengths[chunk] ||
            pg_chunk_offset(10, 4, chunk) != expected_offsets[chunk]) {
            fprintf(stderr, "uneven chunk geometry is incorrect\n");
            return -1;
        }
    }
    if (pg_chunk_nelem(2, 4, 3) != 0 || pg_chunk_offset(2, 4, 3) != 2 ||
        pg_chunk_nelem(10, 0, 0) != -1 || pg_chunk_offset(10, 4, 4) != -1) {
        fprintf(stderr, "boundary chunk geometry is incorrect\n");
        return -1;
    }
    return 0;
}

static int test_int32_reduction(void)
{
    int32_t destination[] = {2, 3, 4};
    const int32_t source[] = {5, 7, 11};
    const int32_t expected_sum[] = {7, 10, 15};
    const int32_t expected_product[] = {10, 21, 44};

    if (pg_reduce(destination, source, 3, PG_INT32, PG_SUM) != 0 ||
        destination[0] != expected_sum[0] || destination[1] != expected_sum[1] ||
        destination[2] != expected_sum[2]) {
        return -1;
    }
    destination[0] = 2;
    destination[1] = 3;
    destination[2] = 4;
    if (pg_reduce(destination, source, 3, PG_INT32, PG_PROD) != 0 ||
        destination[0] != expected_product[0] || destination[1] != expected_product[1] ||
        destination[2] != expected_product[2]) {
        return -1;
    }
    return 0;
}

static int test_double_reduction(void)
{
    double destination[] = {1.5, 2.0};
    const double source[] = {2.0, 4.5};

    if (pg_reduce(destination, source, 2, PG_DOUBLE, PG_SUM) != 0 ||
        fabs(destination[0] - 3.5) > 1e-12 ||
        fabs(destination[1] - 6.5) > 1e-12) {
        return -1;
    }
    destination[0] = 1.5;
    destination[1] = 2.0;
    if (pg_reduce(destination, source, 2, PG_DOUBLE, PG_PROD) != 0 ||
        fabs(destination[0] - 3.0) > 1e-12 ||
        fabs(destination[1] - 9.0) > 1e-12) {
        return -1;
    }
    return 0;
}

static int test_validation(void)
{
    int32_t value = 1;

    if (pg_datatype_size(PG_INT32) != sizeof(int32_t) ||
        pg_datatype_size(PG_DOUBLE) != sizeof(double) ||
        pg_validate_reduction(PG_INT32, PG_SUM) != 0 ||
        pg_validate_reduction(PG_DOUBLE, PG_PROD) != 0 ||
        pg_validate_reduction((DATATYPE)99, PG_SUM) != -1 ||
        pg_validate_reduction(PG_INT32, (OPERATION)99) != -1 ||
        pg_reduce(NULL, &value, 1, PG_INT32, PG_SUM) != -1 ||
        pg_reduce(NULL, NULL, 0, PG_INT32, PG_SUM) != 0) {
        fprintf(stderr, "reduction validation failed\n");
        return -1;
    }
    return 0;
}

int main(void)
{
    if (test_chunking() != 0 || test_int32_reduction() != 0 ||
        test_double_reduction() != 0 || test_validation() != 0) {
        fprintf(stderr, "Phase 5 reduction tests failed\n");
        return EXIT_FAILURE;
    }
    printf("Phase 5 reduction tests passed\n");
    return EXIT_SUCCESS;
}