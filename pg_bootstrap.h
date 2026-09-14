#ifndef PG_BOOTSTRAP_H
#define PG_BOOTSTRAP_H

#include "pg_common.h"

/* Validate and parse the ordered host list that defines the logical ring. */
int validate_host_list(char **host_list, int host_count);
int parse_process_group_spec(const char *spec, int *rank,
                             char ***host_list, int *host_count);
void free_process_group_hosts(char **host_list, int host_count);

/* Store the local rank, group size, and previous/next ring neighbors. */
int configure_process_group_topology(pg_handle_t *pg, int rank, int size);

/*
 * serialize_metadata / deserialize_metadata:
 *  Convert bootstrap metadata to and from a 48-byte TCP message.
 *
 * Each integer is written in network byte order. Fixed offsets make the wire
 * format independent of compiler padding and structure alignment.
 */
void serialize_metadata(const pg_metadata_t *metadata, unsigned char wire[PG_METADATA_WIRE_SIZE]);

int deserialize_metadata(const unsigned char wire[PG_METADATA_WIRE_SIZE], pg_metadata_t *metadata);

/*
 * write_full / read_full:
 *  Transfer exactly length bytes over a TCP socket.
 *
 * TCP is a byte stream: one write is not guaranteed to match one read. These
 * helpers continue until the complete message is transferred or an error or
 * orderly peer close occurs. EINTR is retried because signals may interrupt
 * system calls without indicating a connection failure.
 */
int write_full(int fd, const void *buffer, size_t length);

int read_full(int fd, void *buffer, size_t length);

/* Extract endpoint metadata for a specific directional QP. */
int metadata_from_qp(const pg_handle_t *pg, struct ibv_qp *qp,
                     uint32_t psn, pg_metadata_t *metadata);

/*
 * validate_peer_metadata:
 *  Check that received peer metadata has expected rank and group size.
 */
int validate_peer_metadata(const pg_metadata_t *metadata,
                          uint32_t expected_rank, uint32_t group_size);

/*
 * bootstrap_ring:
 *  Create one TCP control connection for each logical ring edge.
 *
 * Every rank listens on its own rank-derived port and connects to its next
 * rank. The accepted connection comes from the previous rank. This avoids
 * opening two TCP connections for the same ring edge while allowing metadata
 * to flow in both directions on one full-duplex socket.
 */
int bootstrap_ring(pg_handle_t *pg, char **host_list, int host_count);

/* Synchronize all ranks after setup and before resource teardown. */
int bootstrap_ring_barrier(pg_handle_t *pg);

#endif /* PG_BOOTSTRAP_H */
