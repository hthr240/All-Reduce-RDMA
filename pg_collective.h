#ifndef PG_COLLECTIVE_H
#define PG_COLLECTIVE_H

#include <stddef.h>

#include "pg_common.h"

#define PG_EAGER_IMM(seq, round, segment) \
    ((((uint32_t)(seq) & 0xffu) << 24) | \
    (((uint32_t)(round) & 0xffu) << 16) | \
    ((uint32_t)(segment) & 0xffffu))

size_t pg_datatype_size(DATATYPE datatype);
int pg_validate_reduction(DATATYPE datatype, OPERATION operation);
int pg_chunk_nelem(int count, int nranks, int chunk);
int pg_chunk_offset(int count, int nranks, int chunk);
int pg_reduce(void *dst, const void *src, int count,
              DATATYPE datatype, OPERATION operation);
int pg_send_chunk(int rank, int step, int nranks);
int pg_receive_chunk(int rank, int step, int nranks);

/* Run the eager Reduce Scatter half of the ring collective. */
int pg_run_eager_reduce_scatter(pg_handle_t *pg, const void *sendbuf,
                                void *recvbuf, int count,
                                DATATYPE datatype, OPERATION operation);

/* Run the eager All Gather half of the ring collective. */
int pg_run_eager_all_gather(pg_handle_t *pg, void *recvbuf, int count,
                           DATATYPE datatype);

int pg_run_rendezvous_reduce_scatter(pg_handle_t *pg, const void *sendbuf,
                                     void *recvbuf, int count,
                                     DATATYPE datatype, OPERATION operation);
int pg_run_rendezvous_all_gather(pg_handle_t *pg, void *recvbuf, int count,
                                 DATATYPE datatype);

#endif /* PG_COLLECTIVE_H */