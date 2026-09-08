#ifndef PG_COMMON_H
#define PG_COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <infiniband/verbs.h>

/*
 * DATATYPE:
 *  Supported element types from the reference implementation.
 */
typedef enum {
    PG_INT32 = 0,
    PG_DOUBLE = 1
} DATATYPE;

/*
 * OPERATION:
 *  Supported reduction operations from the reference implementation.
 */
typedef enum {
    PG_SUM = 0,
    PG_PROD = 1
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
    /* Directional transport resources. */
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_qp *qp_send;
    struct ibv_qp *qp_recv;
    /* Compatibility aliases retained for the phase 1 API/tests. */
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
    /* Bootstrap sockets remain open for transport barriers and shutdown. */
    int sock_previous;
    int sock_next;
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
