#ifndef PG_REDUCTION_H
#define PG_REDUCTION_H

#include <stddef.h>

#include "pg_common.h"

/* Return the size in bytes of one supported element, or 0 if unsupported. */
size_t pg_datatype_size(DATATYPE datatype);

/* Validate a datatype/operation pair accepted by the reference contract. */
int pg_validate_reduction(DATATYPE datatype, OPERATION operation);

/* Partition count elements into n chunks, allowing a one-element remainder. */
int pg_chunk_nelem(int count, int nranks, int chunk);
int pg_chunk_offset(int count, int nranks, int chunk);

/* Reduce src element-wise into dst. dst may alias src. */
int pg_reduce(void *dst, const void *src, int count,
              DATATYPE datatype, OPERATION operation);

/* Ring schedule helpers shared by eager and rendezvous collectives. */
int pg_send_chunk(int rank, int step, int nranks);
int pg_receive_chunk(int rank, int step, int nranks);
int pg_all_gather_send_chunk(int rank, int step, int nranks);
int pg_all_gather_receive_chunk(int rank, int step, int nranks);

#endif /* PG_REDUCTION_H */