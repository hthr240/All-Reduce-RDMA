#include <string.h>

#include "pg_collective.h"
#include "pg_log.h"
#include "pg_reduction.h"
#include "pg_verbs.h"

#define PG_EAGER_IMM(seq, round, segment) \
    ((((uint32_t)(seq) & 0xffu) << 24) | \
    (((uint32_t)(round) & 0xffu) << 16) | \
    ((uint32_t)(segment) & 0xffffu))

static int wait_for_eager_round(pg_handle_t *pg, uint32_t expected_imm,
                                size_t expected_bytes)
{
    struct ibv_wc recv_wc;
    struct ibv_wc send_wc;
    int got_recv = 0;
    int got_send = 0;

    while (!got_recv || !got_send) {
        int count;

        if (!got_recv) {
            count = poll_eager_completion(pg, 1, &recv_wc);
            if (count < 0) {
                return -1;
            }
            if (count == 1) {
                if (recv_wc.status != IBV_WC_SUCCESS ||
                    recv_wc.opcode != IBV_WC_RECV ||
                    recv_wc.imm_data != expected_imm ||
                    recv_wc.byte_len != expected_bytes) {
                    PG_LOG_ERROR("pg_collective",
                                 "Invalid eager receive completion: status=%s imm=0x%x bytes=%u expected_imm=0x%x expected_bytes=%zu",
                                 ibv_wc_status_str(recv_wc.status),
                                 recv_wc.imm_data, recv_wc.byte_len,
                                 expected_imm, expected_bytes);
                    return -1;
                }
                got_recv = 1;
            }
        }

        if (!got_send) {
            count = poll_eager_completion(pg, 0, &send_wc);
            if (count < 0) {
                return -1;
            }
            if (count == 1) {
                if (send_wc.status != IBV_WC_SUCCESS) {
                    PG_LOG_ERROR("pg_collective",
                                 "Eager send completion failed: status=%s",
                                 ibv_wc_status_str(send_wc.status));
                    return -1;
                }
                got_send = 1;
            }
        }
    }
    return 0;
}

static int wait_for_rendezvous_round(pg_handle_t *pg, uint32_t expected_imm,
                                     size_t expected_bytes)
{
    struct ibv_wc recv_wc;
    struct ibv_wc send_wc;
    int got_recv = 0;
    int got_send = 0;

    while (!got_recv || !got_send) {
        int count;

        if (!got_recv) {
            count = poll_eager_completion(pg, 1, &recv_wc);
            if (count < 0) {
                return -1;
            }
            if (count == 1) {
                if (recv_wc.status != IBV_WC_SUCCESS ||
                    recv_wc.opcode != IBV_WC_RECV_RDMA_WITH_IMM ||
                    recv_wc.imm_data != expected_imm ||
                    recv_wc.byte_len != expected_bytes) {
                    PG_LOG_ERROR("pg_collective",
                                 "Invalid rendezvous receive completion: status=%s opcode=%d imm=0x%x bytes=%u expected_imm=0x%x expected_bytes=%zu",
                                 ibv_wc_status_str(recv_wc.status),
                                 recv_wc.opcode, recv_wc.imm_data,
                                 recv_wc.byte_len, expected_imm,
                                 expected_bytes);
                    return -1;
                }
                got_recv = 1;
            }
        }

        if (!got_send) {
            count = poll_eager_completion(pg, 0, &send_wc);
            if (count < 0) {
                return -1;
            }
            if (count == 1) {
                if (send_wc.status != IBV_WC_SUCCESS) {
                    PG_LOG_ERROR("pg_collective",
                                 "Rendezvous send completion failed: status=%s",
                                 ibv_wc_status_str(send_wc.status));
                    return -1;
                }
                got_send = 1;
            }
        }
    }
    return 0;
}

static size_t eager_segment_count(int count, int nranks, size_t element_size)
{
    size_t largest_chunk_bytes;

    largest_chunk_bytes = (size_t)((count + nranks - 1) / nranks) *
                          element_size;
    return (largest_chunk_bytes + PG_EAGER_BUFFER_SIZE - 1) /
           PG_EAGER_BUFFER_SIZE;
}

int pg_run_eager_reduce_scatter(pg_handle_t *pg, const void *sendbuf,
                                void *recvbuf, int count,
                                DATATYPE datatype, OPERATION operation)
{
    size_t element_size;
    size_t total_bytes;
    uint32_t sequence;
    size_t segment_count;
    int step;

    if (!pg || !pg->is_connected || !pg->buf || !sendbuf || !recvbuf ||
        count < 0 || pg_validate_reduction(datatype, operation) != 0 ||
        pg->size < 2 || pg->rank < 0 || pg->rank >= pg->size) {
        PG_LOG_ERROR("pg_collective", "Invalid eager Reduce Scatter arguments");
        return -1;
    }
    element_size = pg_datatype_size(datatype);
    total_bytes = (size_t)count * element_size;
    if (total_bytes > PG_WORK_BUFFER_SIZE) {
        PG_LOG_ERROR("pg_collective", "Reduce Scatter input exceeds work buffer: %zu bytes",
                     total_bytes);
        return -1;
    }
    PG_LOG_INFO("pg_collective",
                "Starting eager Reduce Scatter: rank=%d size=%d count=%d bytes=%zu",
                pg->rank, pg->size, count, total_bytes);
    sequence = pg->collective_sequence;
    segment_count = eager_segment_count(count, pg->size, element_size);
    memcpy(pg->buf, sendbuf, total_bytes);

    for (step = 0; step < pg->size - 1; ++step) {
        int send_chunk = pg_send_chunk(pg->rank, step, pg->size);
        int receive_chunk = pg_receive_chunk(pg->rank, step, pg->size);
        int receive_count = pg_chunk_nelem(count, pg->size, receive_chunk);
        int receive_offset = pg_chunk_offset(count, pg->size, receive_chunk);
        int send_count = pg_chunk_nelem(count, pg->size, send_chunk);
        int send_offset = pg_chunk_offset(count, pg->size, send_chunk);
        size_t receive_bytes = (size_t)receive_count * element_size;
        size_t send_bytes = (size_t)send_count * element_size;
        size_t segment_index;

        PG_LOG_DEBUG("pg_collective",
                 "Reduce Scatter round %d: send_chunk=%d send_bytes=%zu receive_chunk=%d receive_bytes=%zu",
                 step, send_chunk, send_bytes, receive_chunk, receive_bytes);
        if (receive_count < 0 || send_count < 0) {
            PG_LOG_ERROR("pg_collective", "Eager Reduce Scatter failed at round %d", step);
            return -1;
        }
        for (segment_index = 0; segment_index < segment_count; ++segment_index) {
            size_t segment_offset = segment_index * PG_EAGER_BUFFER_SIZE;
            size_t receive_segment_bytes = receive_bytes > segment_offset ?
                receive_bytes - segment_offset : 0;
            size_t send_segment_bytes = send_bytes > segment_offset ?
                send_bytes - segment_offset : 0;
            int receive_segment_count;

            if (receive_segment_bytes > PG_EAGER_BUFFER_SIZE) {
                receive_segment_bytes = PG_EAGER_BUFFER_SIZE;
            }
            if (send_segment_bytes > PG_EAGER_BUFFER_SIZE) {
                send_segment_bytes = PG_EAGER_BUFFER_SIZE;
            }
            receive_segment_count = (int)(receive_segment_bytes / element_size);
            if (post_eager_receive(pg, receive_segment_bytes, (uint64_t)step) != 0 ||
                post_eager_send(pg,
                                (unsigned char *)pg->buf +
                                    (size_t)send_offset * element_size + segment_offset,
                                send_segment_bytes,
                                PG_EAGER_IMM(sequence, step, segment_index),
                                (uint64_t)step) != 0 ||
                wait_for_eager_round(pg,
                                     PG_EAGER_IMM(sequence, step, segment_index),
                                     receive_segment_bytes) != 0 ||
                pg_reduce((unsigned char *)pg->buf +
                              (size_t)receive_offset * element_size + segment_offset,
                          (unsigned char *)pg->buf + pg->eager_offset,
                          receive_segment_count, datatype, operation) != 0) {
                PG_LOG_ERROR("pg_collective",
                             "Eager Reduce Scatter failed at round %d segment %zu",
                             step, segment_index);
                return -1;
            }
        }
    }

    {
        int owned_chunk = (pg->rank + 1) % pg->size;
        int owned_count = pg_chunk_nelem(count, pg->size, owned_chunk);
        int owned_offset = pg_chunk_offset(count, pg->size, owned_chunk);
        memcpy(recvbuf, (unsigned char *)pg->buf +
                              (size_t)owned_offset * element_size,
               (size_t)owned_count * element_size);
        PG_LOG_INFO("pg_collective",
                "Eager Reduce Scatter complete: owned_chunk=%d offset=%d count=%d",
                owned_chunk, owned_offset, owned_count);
    }
    return 0;
}

int pg_run_eager_all_gather(pg_handle_t *pg, void *recvbuf, int count,
                           DATATYPE datatype)
{
    size_t element_size;
    size_t total_bytes;
    uint32_t sequence;
    size_t segment_count;
    int step;

    if (!pg || !pg->is_connected || !pg->buf || !recvbuf || count < 0 ||
        pg_datatype_size(datatype) == 0 || pg->size < 2 ||
        pg->rank < 0 || pg->rank >= pg->size) {
        PG_LOG_ERROR("pg_collective", "Invalid eager All Gather arguments");
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    element_size = pg_datatype_size(datatype);
    total_bytes = (size_t)count * element_size;
    if (total_bytes > PG_WORK_BUFFER_SIZE) {
        PG_LOG_ERROR("pg_collective", "All Gather output exceeds work buffer: %zu bytes",
                     total_bytes);
        return -1;
    }
    PG_LOG_INFO("pg_collective",
                "Starting eager All Gather: rank=%d size=%d count=%d bytes=%zu",
                pg->rank, pg->size, count, total_bytes);
    sequence = pg->collective_sequence;
    segment_count = eager_segment_count(count, pg->size, element_size);
    for (step = 0; step < pg->size - 1; ++step) {
        int send_chunk = pg_all_gather_send_chunk(pg->rank, step, pg->size);
        int receive_chunk = pg_all_gather_receive_chunk(pg->rank, step,
                                                        pg->size);
        int send_count = pg_chunk_nelem(count, pg->size, send_chunk);
        int send_offset = pg_chunk_offset(count, pg->size, send_chunk);
        int receive_count = pg_chunk_nelem(count, pg->size, receive_chunk);
        int receive_offset = pg_chunk_offset(count, pg->size, receive_chunk);
        size_t send_bytes = (size_t)send_count * element_size;
        size_t receive_bytes = (size_t)receive_count * element_size;
        size_t segment_index;

        if (send_count < 0 || send_offset < 0 ||
            receive_count < 0 || receive_offset < 0) {
            PG_LOG_ERROR("pg_collective",
                         "All Gather schedule produced invalid chunk geometry at step %d",
                         step);
            return -1;
        }

        PG_LOG_DEBUG("pg_collective",
                     "All Gather round %d: send_chunk=%d send_bytes=%zu receive_chunk=%d receive_bytes=%zu",
                     step, send_chunk, send_bytes, receive_chunk,
                     receive_bytes);
        for (segment_index = 0; segment_index < segment_count; ++segment_index) {
            size_t segment_offset = segment_index * PG_EAGER_BUFFER_SIZE;
            size_t receive_segment_bytes = receive_bytes > segment_offset ?
                receive_bytes - segment_offset : 0;
            size_t send_segment_bytes = send_bytes > segment_offset ?
                send_bytes - segment_offset : 0;

            if (receive_segment_bytes > PG_EAGER_BUFFER_SIZE) {
                receive_segment_bytes = PG_EAGER_BUFFER_SIZE;
            }
            if (send_segment_bytes > PG_EAGER_BUFFER_SIZE) {
                send_segment_bytes = PG_EAGER_BUFFER_SIZE;
            }
            if (post_eager_receive(pg, receive_segment_bytes, (uint64_t)step) != 0 ||
                post_eager_send(pg,
                                (unsigned char *)pg->buf +
                                    (size_t)send_offset * element_size + segment_offset,
                                send_segment_bytes,
                                PG_EAGER_IMM(sequence, pg->size - 1 + step,
                                             segment_index),
                                (uint64_t)step) != 0 ||
                wait_for_eager_round(pg,
                                     PG_EAGER_IMM(sequence, pg->size - 1 + step,
                                                  segment_index),
                                     receive_segment_bytes) != 0) {
                PG_LOG_ERROR("pg_collective",
                             "Eager All Gather failed at round %d segment %zu",
                             step, segment_index);
                return -1;
            }
            memcpy((unsigned char *)pg->buf +
                       (size_t)receive_offset * element_size + segment_offset,
                   (unsigned char *)pg->buf + pg->eager_offset,
                   receive_segment_bytes);
        }
    }

    memcpy(recvbuf, pg->buf, total_bytes);
    ++pg->collective_sequence;
    PG_LOG_INFO("pg_collective",
                "Eager All Gather complete: rank=%d size=%d count=%d bytes=%zu",
                pg->rank, pg->size, count, total_bytes);
    return 0;
}

int pg_run_rendezvous_reduce_scatter(pg_handle_t *pg, const void *sendbuf,
                                     void *recvbuf, int count,
                                     DATATYPE datatype, OPERATION operation)
{
    size_t element_size;
    size_t total_bytes;
    uint32_t sequence;
    int step;

    if (!pg || !pg->is_connected || !pg->buf || !sendbuf || !recvbuf ||
        count < 0 || pg_validate_reduction(datatype, operation) != 0 ||
        pg->size < 2 || pg->rank < 0 || pg->rank >= pg->size) {
        PG_LOG_ERROR("pg_collective", "Invalid rendezvous Reduce Scatter arguments");
        return -1;
    }
    element_size = pg_datatype_size(datatype);
    total_bytes = (size_t)count * element_size;
    if (total_bytes > PG_WORK_BUFFER_SIZE) {
        PG_LOG_ERROR("pg_collective",
                     "Rendezvous Reduce Scatter input exceeds work buffer: %zu bytes",
                     total_bytes);
        return -1;
    }

    sequence = pg->collective_sequence;
    memcpy((unsigned char *)pg->buf + pg->work_offset, sendbuf, total_bytes);
    for (step = 0; step < pg->size - 1; ++step) {
        int send_chunk = pg_send_chunk(pg->rank, step, pg->size);
        int receive_chunk = pg_receive_chunk(pg->rank, step, pg->size);
        int send_count = pg_chunk_nelem(count, pg->size, send_chunk);
        int receive_count = pg_chunk_nelem(count, pg->size, receive_chunk);
        int send_offset = pg_chunk_offset(count, pg->size, send_chunk);
        int receive_offset = pg_chunk_offset(count, pg->size, receive_chunk);
        size_t send_bytes;
        size_t receive_bytes;
        size_t staging_offset;

        if (send_count < 0 || receive_count < 0 || send_offset < 0 ||
            receive_offset < 0) {
            return -1;
        }
        send_bytes = (size_t)send_count * element_size;
        receive_bytes = (size_t)receive_count * element_size;
        staging_offset = pg->staging_offset +
                         (size_t)step * pg->staging_slot_size;
        if (receive_bytes > pg->staging_slot_size ||
            post_eager_receive(pg, 0, (uint64_t)step) != 0 ||
            post_rendezvous_write(pg,
                                  (unsigned char *)pg->buf + pg->work_offset +
                                      (size_t)send_offset * element_size,
                                  send_bytes, staging_offset,
                                  PG_EAGER_IMM(sequence, step, 0),
                                  (uint64_t)step) != 0 ||
            wait_for_rendezvous_round(pg, PG_EAGER_IMM(sequence, step, 0),
                                      receive_bytes) != 0 ||
            pg_reduce((unsigned char *)pg->buf + pg->work_offset +
                          (size_t)receive_offset * element_size,
                      (unsigned char *)pg->buf + staging_offset,
                      receive_count, datatype, operation) != 0) {
            PG_LOG_ERROR("pg_collective",
                         "Rendezvous Reduce Scatter failed at round %d", step);
            return -1;
        }
    }

    {
        int owned_chunk = (pg->rank + 1) % pg->size;
        int owned_count = pg_chunk_nelem(count, pg->size, owned_chunk);
        int owned_offset = pg_chunk_offset(count, pg->size, owned_chunk);

        memcpy(recvbuf, (unsigned char *)pg->buf + pg->work_offset +
                            (size_t)owned_offset * element_size,
               (size_t)owned_count * element_size);
    }
    return 0;
}

int pg_run_rendezvous_all_gather(pg_handle_t *pg, void *recvbuf, int count,
                                 DATATYPE datatype)
{
    size_t element_size;
    size_t total_bytes;
    uint32_t sequence;
    int step;

    if (!pg || !pg->is_connected || !pg->buf || !recvbuf || count < 0 ||
        pg_datatype_size(datatype) == 0 || pg->size < 2 || pg->rank < 0 ||
        pg->rank >= pg->size) {
        PG_LOG_ERROR("pg_collective", "Invalid rendezvous All Gather arguments");
        return -1;
    }
    element_size = pg_datatype_size(datatype);
    total_bytes = (size_t)count * element_size;
    if (total_bytes > PG_WORK_BUFFER_SIZE) {
        PG_LOG_ERROR("pg_collective",
                     "Rendezvous All Gather output exceeds work buffer: %zu bytes",
                     total_bytes);
        return -1;
    }

    sequence = pg->collective_sequence;
    for (step = 0; step < pg->size - 1; ++step) {
        int send_chunk = pg_all_gather_send_chunk(pg->rank, step, pg->size);
        int receive_chunk = pg_all_gather_receive_chunk(pg->rank, step,
                                                        pg->size);
        int send_count = pg_chunk_nelem(count, pg->size, send_chunk);
        int receive_count = pg_chunk_nelem(count, pg->size, receive_chunk);
        int send_offset = pg_chunk_offset(count, pg->size, send_chunk);
        int receive_offset = pg_chunk_offset(count, pg->size, receive_chunk);
        size_t send_bytes;
        size_t receive_bytes;
        size_t remote_offset;

        if (send_count < 0 || receive_count < 0 || send_offset < 0 ||
            receive_offset < 0) {
            return -1;
        }
        send_bytes = (size_t)send_count * element_size;
        receive_bytes = (size_t)receive_count * element_size;
        remote_offset = pg->work_offset + (size_t)send_offset * element_size;
        if (post_eager_receive(pg, 0, (uint64_t)step) != 0 ||
            post_rendezvous_write(pg,
                                  (unsigned char *)pg->buf + pg->work_offset +
                                      (size_t)send_offset * element_size,
                                  send_bytes, remote_offset,
                                  PG_EAGER_IMM(sequence, pg->size - 1 + step, 0),
                                  (uint64_t)step) != 0 ||
            wait_for_rendezvous_round(pg,
                                      PG_EAGER_IMM(sequence, pg->size - 1 + step,
                                                   0),
                                      receive_bytes) != 0) {
            PG_LOG_ERROR("pg_collective",
                         "Rendezvous All Gather failed at round %d", step);
            return -1;
        }
    }

    memcpy(recvbuf, (unsigned char *)pg->buf + pg->work_offset, total_bytes);
    ++pg->collective_sequence;
    return 0;
}