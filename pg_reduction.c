#include <stdint.h>

#include "pg_reduction.h"

size_t pg_datatype_size(DATATYPE datatype)
{
    switch (datatype) {
        case PG_INT32:
            return sizeof(int32_t);
        case PG_DOUBLE:
            return sizeof(double);
        default:
            return 0;
    }
}

int pg_validate_reduction(DATATYPE datatype, OPERATION operation)
{
    return pg_datatype_size(datatype) != 0 &&
           (operation == PG_SUM || operation == PG_PROD) ? 0 : -1;
}

int pg_chunk_nelem(int count, int nranks, int chunk)
{
    if (count < 0 || nranks <= 0 || chunk < 0 || chunk >= nranks) {
        return -1;
    }
    return count / nranks + (chunk < count % nranks ? 1 : 0);
}

int pg_chunk_offset(int count, int nranks, int chunk)
{
    int remainder;

    if (count < 0 || nranks <= 0 || chunk < 0 || chunk >= nranks) {
        return -1;
    }
    remainder = count % nranks;
    return chunk * (count / nranks) + (chunk < remainder ? chunk : remainder);
}

int pg_reduce(void *dst, const void *src, int count,
              DATATYPE datatype, OPERATION operation)
{
    int i;

    if (count < 0 || pg_validate_reduction(datatype, operation) != 0 ||
        (count > 0 && (!dst || !src))) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    if (datatype == PG_INT32) {
        int32_t *destination = dst;
        const int32_t *source = src;

        if (operation == PG_SUM) {
            for (i = 0; i < count; ++i) {
                destination[i] += source[i];
            }
        } else {
            for (i = 0; i < count; ++i) {
                destination[i] *= source[i];
            }
        }
    } else {
        double *destination = dst;
        const double *source = src;

        if (operation == PG_SUM) {
            for (i = 0; i < count; ++i) {
                destination[i] += source[i];
            }
        } else {
            for (i = 0; i < count; ++i) {
                destination[i] *= source[i];
            }
        }
    }
    return 0;
}