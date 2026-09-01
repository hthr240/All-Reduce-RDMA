#ifndef PG_COMMON_H
#define PG_COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <infiniband/verbs.h>

/*
 * DATATYPE:
 *  Supported element types for the collective operation.
 *  These values define what the application may reduce and how each element is interpreted.
 */
typedef enum {
    PG_INT32 = 0,
    PG_INT64 = 1,
    PG_FLOAT = 2,
    PG_DOUBLE = 3
} DATATYPE;

/*
 * OPERATION:
 *  Supported reduction operations applied element-wise across ranks.
 *  The implementation will later combine local chunks using the chosen operator.
 */
typedef enum {
    PG_SUM = 0,
    PG_MAX = 1,
    PG_MIN = 2
} OPERATION;

/*
 * pg_metadata_t:
 *  The local information a rank will exchange with a peer during bootstrap.
 *
 * This is the host-side representation. It is converted to a fixed-size wire
 * representation before being sent over TCP; the C struct itself is never
 * sent because compilers may add padding and hosts may use different byte
 * orderings.
 */
typedef struct {
    uint32_t rank;
    uint32_t size;
    uint32_t qpn;
    uint32_t psn;
    uint16_t lid;
    union ibv_gid gid;
    uint64_t buffer_addr;
    uint32_t rkey;
} pg_metadata_t;

/*
 * pg_handle_t:
 *  Opaque process-group state for one rank.
 *
 *  This structure holds all per-rank state needed for the eventual RDMA collective:
 *  - local rank identity and group size
 *  - connectivity state
 *  - hostname / rank mapping
 *  - verbs objects (device context, PD, CQ, QP, MR)
 *  - registered local buffer and buffer metadata
 *
 *  The final implementation will extend this with neighbor metadata and ring state.
 */
typedef struct pg_handle {
    int rank;
    int size;
    /* Logical ring neighbors; these are rank numbers, not hostnames. */
    int previous_rank;
    int next_rank;
    int is_connected;
    char *hostname;
    /* Device objects are created in this order: device -> context -> PD. */
    struct ibv_context *context;
    struct ibv_device *device;
    struct ibv_pd *pd;
    /* The CQ is shared by this QP's send and receive operations for now. */
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    void *buf;
    size_t buf_size;
    /* Selected physical port on the opened device. */
    int ib_port;
    /* Remote metadata will be filled during the TCP bootstrap phase. */
    pg_metadata_t previous_peer;
    pg_metadata_t next_peer;
} pg_handle_t;

/* Small initial values for the first local Verbs setup milestone. */
#define PG_BUFFER_SIZE 4096
#define PG_CQ_CAPACITY 16
#define PG_QP_DEPTH 8
#define PG_METADATA_WIRE_SIZE 48
#define PG_BOOTSTRAP_BASE_PORT 18515
#define PG_BOOTSTRAP_RETRIES 50

#define PG_TRACE(rank, ...) do { \
    fprintf(stderr, "[bootstrap rank %d] ", (rank)); \
    fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); \
    fflush(stderr); \
} while (0)

#endif /* PG_COMMON_H */
