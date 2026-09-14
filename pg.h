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

/*
 * connect_process_group - parse the group spec "<myindex>:<host0>,...,<hostN-1>"
 * (1-based index), build the TCP bootstrap ring, and connect the RC queue
 * pairs. Optional environment knobs (must match on all ranks):
 *   PG_MODE=auto|eager|rdvz   force a protocol; auto picks by chunk size
 *   PG_EAGER_THRESHOLD=bytes  eager->rendezvous switch point (per-chunk bytes)
 *   PG_NOPIPE=1               disable rendezvous pipelining (step-synchronous)
 *   PG_TCP_PORT=port          base TCP port for the bootstrap exchange
 *   PG_IB_DEV=name            Verbs device to open (default: first device)
 *   PG_GID_IDX=n              GID index for RoCE/GRH routing (default: LID only)
 *   PG_LOG_LEVEL=debug|info|warn|error   diagnostic verbosity
 */
int connect_process_group(char *servername, void **pg_handle);
int pg_all_reduce(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op, void *pg_handle);
int pg_reduce_scatter(void *sendbuf, void *recvbuf, int count,
                      DATATYPE datatype, OPERATION op, void *pg_handle);
int pg_all_gather(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, void *pg_handle);
int pg_chunk(void *pg_handle, int count, int *offset, int *nelem);
int pg_rank(void *pg_handle);
int pg_nranks(void *pg_handle);
int pg_close(void *pg_handle);

/* Optional diagnostics used by the course test driver. */
int pg_ring_token(void *pg_handle, int laps);

#endif /* PG_H */