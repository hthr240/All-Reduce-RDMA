#ifndef PG_INTERNAL_H
#define PG_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <infiniband/verbs.h>

#include "pg.h"

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

typedef enum {
    PG_TRANSPORT_AUTO = 0,
    PG_TRANSPORT_EAGER,
    PG_TRANSPORT_RDVZ
} pg_transport_mode_t;

typedef struct pg_handle {
    int rank;
    int size;
    int previous_rank;
    int next_rank;
    int is_connected;
    char *hostname;

    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_qp *qp_send;
    struct ibv_qp *qp_recv;
    struct ibv_mr *mr;

    void *buf;
    size_t buf_size;
    size_t work_offset;
    size_t staging_offset;
    size_t staging_slot_size;
    size_t eager_offset;

    pg_transport_mode_t transport_mode;
    size_t eager_threshold;
    int pipeline_enabled;
    int ib_port;
    int gid_index;
    int bootstrap_base_port;
    int max_inline;

    pg_metadata_t previous_peer;
    pg_metadata_t next_peer;
    int sock_previous;
    int sock_next;
    uint8_t collective_sequence;
} pg_handle_t;

#define PG_WORK_BUFFER_SIZE (4u << 20)
#define PG_EAGER_BUFFER_SIZE 8192
#define PG_EAGER_THRESHOLD (16u << 10)
#define PG_RDVZ_SEGMENT_SIZE (128u << 10)
#define PG_CQ_CAPACITY 256
#define PG_QP_DEPTH 64
#define PG_RQ_DEPTH 160
#define PG_EAGER_SLOTS PG_RQ_DEPTH
#define PG_WC_BATCH 16
#define PG_MAX_INLINE_REQ 512
#define PG_STAGING_ALIGNMENT 64
#define PG_METADATA_WIRE_SIZE 48
#define PG_BOOTSTRAP_BASE_PORT 18515
#define PG_BOOTSTRAP_RETRIES 600

/* One staging slot must hold the largest chunk of any datatype: 8-byte
 * elements round the per-rank ceiling up past ceil(WORK/size) bytes, so the
 * slot is the 8-byte-element ceiling scaled back to bytes. */
#define PG_ALIGN_UP(value, alignment) \
        (((size_t)(value) + (size_t)(alignment) - 1) / \
         (size_t)(alignment) * (size_t)(alignment))
#define PG_RDVZ_SLOT_SIZE(pg_size) \
        PG_ALIGN_UP((size_t)8 * \
                                (((size_t)PG_WORK_BUFFER_SIZE / 8 + \
                                    (size_t)(pg_size) - 1) / (size_t)(pg_size)), \
                                PG_STAGING_ALIGNMENT)
#define PG_RDVZ_STAGING_SIZE(pg_size) \
    ((size_t)((pg_size) - 1) * PG_RDVZ_SLOT_SIZE(pg_size))
#define PG_REGISTERED_BUFFER_SIZE(pg_size) \
    ((size_t)PG_WORK_BUFFER_SIZE + PG_RDVZ_STAGING_SIZE(pg_size) + \
     (size_t)PG_EAGER_SLOTS * (size_t)PG_EAGER_BUFFER_SIZE)

#define PG_EAGER_IMM(seq, round, segment) \
    ((((uint32_t)(seq) & 0xffu) << 24) | \
     (((uint32_t)(round) & 0xffu) << 16) | \
     ((uint32_t)(segment) & 0xffffu))

typedef enum {
    PG_LOG_DEBUG = 0,
    PG_LOG_INFO,
    PG_LOG_WARN,
    PG_LOG_ERROR
} pg_log_level_t;

void pg_log_set_level(pg_log_level_t level);
void pg_log_phase(int rank, int size, int phase, int total,
                  const char *name);
void pg_log_impl(pg_log_level_t level, const char *module, const char *file,
                 int line, const char *fmt, ...);

#define PG_LOG_DEBUG(module, ...) \
    pg_log_impl(PG_LOG_DEBUG, (module), __FILE__, __LINE__, __VA_ARGS__)
#define PG_LOG_INFO(module, ...) \
    pg_log_impl(PG_LOG_INFO, (module), __FILE__, __LINE__, __VA_ARGS__)
#define PG_LOG_WARN(module, ...) \
    pg_log_impl(PG_LOG_WARN, (module), __FILE__, __LINE__, __VA_ARGS__)
#define PG_LOG_ERROR(module, ...) \
    pg_log_impl(PG_LOG_ERROR, (module), __FILE__, __LINE__, __VA_ARGS__)

int configure_transport_mode(pg_handle_t *pg);

int validate_host_list(char **host_list, int host_count);
int parse_process_group_spec(const char *spec, int *rank,
                             char ***host_list, int *host_count);
void free_process_group_hosts(char **host_list, int host_count);
int configure_process_group_topology(pg_handle_t *pg, int rank, int size);
void serialize_metadata(const pg_metadata_t *metadata,
                        unsigned char wire[PG_METADATA_WIRE_SIZE]);
int deserialize_metadata(const unsigned char wire[PG_METADATA_WIRE_SIZE],
                         pg_metadata_t *metadata);
int write_full(int fd, const void *buffer, size_t length);
int read_full(int fd, void *buffer, size_t length);
int metadata_from_qp(const pg_handle_t *pg, struct ibv_qp *qp,
                     uint32_t psn, pg_metadata_t *metadata);
int validate_peer_metadata(const pg_metadata_t *metadata,
                           uint32_t expected_rank, uint32_t group_size);
int bootstrap_ring(pg_handle_t *pg, char **host_list, int host_count);
int bootstrap_ring_barrier(pg_handle_t *pg);

int find_active_port(struct ibv_context *context, int *port_num);
int create_rdma_resources(pg_handle_t *pg);
int connect_rdma_qp(pg_handle_t *pg, struct ibv_qp *qp,
                    uint32_t local_psn, const pg_metadata_t *remote);
int ring_token(pg_handle_t *pg, int laps);
int post_eager_receive(pg_handle_t *pg, size_t length, uint64_t work_id);
int post_eager_send(pg_handle_t *pg, const void *buffer, size_t length,
                    uint32_t immediate, uint64_t work_id);
int post_rendezvous_write(pg_handle_t *pg, const void *buffer, size_t length,
                          size_t remote_offset, uint32_t immediate,
                          uint64_t work_id);
int poll_eager_completion(pg_handle_t *pg, int receive, struct ibv_wc *wc);
int poll_completions(pg_handle_t *pg, int receive, struct ibv_wc *wc,
                     int max_completions);
void destroy_rdma_resources(pg_handle_t *pg);

size_t pg_datatype_size(DATATYPE datatype);
int pg_validate_reduction(DATATYPE datatype, OPERATION operation);
int pg_chunk_nelem(int count, int nranks, int chunk);
int pg_chunk_offset(int count, int nranks, int chunk);
int pg_reduce(void *dst, const void *src, int count,
              DATATYPE datatype, OPERATION operation);
int pg_send_chunk(int rank, int step, int nranks);
int pg_receive_chunk(int rank, int step, int nranks);
int pg_run_eager_reduce_scatter(pg_handle_t *pg, const void *sendbuf,
                                void *recvbuf, int count,
                                DATATYPE datatype, OPERATION operation);
int pg_run_eager_all_gather(pg_handle_t *pg, void *recvbuf, int count,
                            DATATYPE datatype);
int pg_run_rendezvous_reduce_scatter(pg_handle_t *pg, const void *sendbuf,
                                     void *recvbuf, int count,
                                     DATATYPE datatype, OPERATION operation);
int pg_run_rendezvous_all_gather(pg_handle_t *pg, void *recvbuf, int count,
                                 DATATYPE datatype);
int pg_run_all_reduce(pg_handle_t *pg, const void *sendbuf, void *recvbuf,
                      int count, DATATYPE datatype, OPERATION operation,
                      pg_transport_mode_t mode);

#endif /* PG_INTERNAL_H */