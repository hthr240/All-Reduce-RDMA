#ifndef PG_COLLECTIVE_H
#define PG_COLLECTIVE_H

#include "pg_common.h"

/* Run the eager Reduce Scatter half of the ring collective. */
int pg_run_eager_reduce_scatter(pg_handle_t *pg, const void *sendbuf,
                                void *recvbuf, int count,
                                DATATYPE datatype, OPERATION operation);

#endif /* PG_COLLECTIVE_H */