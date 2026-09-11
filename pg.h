#ifndef PG_H
#define PG_H

typedef enum {
    PG_INT32 = 0,
    PG_DOUBLE = 1
} DATATYPE;

typedef enum {
    PG_SUM = 0,
    PG_PROD = 1
} OPERATION;

int connect_process_group(char *servername, void **pg_handle);
int pg_all_reduce(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op, void *pg_handle);
int pg_close(void *pg_handle);

/* Optional diagnostics used by the course test driver. */
int pg_ring_token(void *pg_handle, int laps);

#endif /* PG_H */