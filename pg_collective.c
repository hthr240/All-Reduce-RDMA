#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pg_internal.h"

typedef struct {
    int count;
    DATATYPE datatype;
    OPERATION operation;
    pg_transport_mode_t mode;
    size_t element_size;
    size_t segment_size;
    int pipelined;
    int first_step;
    int end_step;
} pg_collective_schedule_t;

size_t pg_datatype_size(DATATYPE datatype)
{
    switch (datatype) {
        case PG_INT32:
            return sizeof(int32_t);
        case PG_DOUBLE:
            return sizeof(double);
        default:
            return 0;
    }
}

int pg_validate_reduction(DATATYPE datatype, OPERATION operation)
{
    return pg_datatype_size(datatype) != 0 &&
           (operation == PG_SUM || operation == PG_PROD) ? 0 : -1;
}

int pg_chunk_nelem(int count, int nranks, int chunk)
{
    if (count < 0 || nranks <= 0 || chunk < 0 || chunk >= nranks) {
        return -1;
    }
    return count / nranks + (chunk < count % nranks ? 1 : 0);
}

int pg_chunk_offset(int count, int nranks, int chunk)
{
    int remainder;

    if (count < 0 || nranks <= 0 || chunk < 0 || chunk >= nranks) {
        return -1;
    }
    remainder = count % nranks;
    return chunk * (count / nranks) + (chunk < remainder ? chunk : remainder);
}

int pg_reduce(void *dst, const void *src, int count,
              DATATYPE datatype, OPERATION operation)
{
    int index;

    if (count < 0 || pg_validate_reduction(datatype, operation) != 0 ||
        (count > 0 && (!dst || !src))) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    if (datatype == PG_INT32) {
        int32_t *destination = dst;
        const int32_t *source = src;

        if (operation == PG_SUM) {
            for (index = 0; index < count; ++index) {
                destination[index] += source[index];
            }
        } else {
            for (index = 0; index < count; ++index) {
                destination[index] *= source[index];
            }
        }
    } else {
        double *destination = dst;
        const double *source = src;

        if (operation == PG_SUM) {
            for (index = 0; index < count; ++index) {
                destination[index] += source[index];
            }
        } else {
            for (index = 0; index < count; ++index) {
                destination[index] *= source[index];
            }
        }
    }
    return 0;
}

int pg_send_chunk(int rank, int step, int nranks)
{
    if (nranks <= 0 || rank < 0 || rank >= nranks || step < 0) {
        return -1;
    }
    return ((rank - step) % nranks + nranks) % nranks;
}

int pg_receive_chunk(int rank, int step, int nranks)
{
    if (nranks <= 0 || rank < 0 || rank >= nranks || step < 0) {
        return -1;
    }
    return ((rank - step - 1) % nranks + nranks) % nranks;
}

static size_t segment_count(size_t bytes, size_t segment_size)
{
    return bytes == 0 ? 1 : (bytes + segment_size - 1) / segment_size;
}

static size_t segment_length(size_t bytes, size_t segment_size,
                             size_t segment)
{
    size_t offset = segment * segment_size;
    size_t remaining;

    if (offset >= bytes) {
        return 0;
    }
    remaining = bytes - offset;
    return remaining < segment_size ? remaining : segment_size;
}

static int step_send_chunk(const pg_handle_t *pg, int step)
{
    return pg_send_chunk(pg->rank, step, pg->size);
}

static int step_receive_chunk(const pg_handle_t *pg, int step)
{
    return pg_receive_chunk(pg->rank, step, pg->size);
}

static int chunk_geometry(const pg_handle_t *pg,
                          const pg_collective_schedule_t *schedule,
                          int chunk, int *offset, size_t *bytes)
{
    int nelem = pg_chunk_nelem(schedule->count, pg->size, chunk);
    int element_offset = pg_chunk_offset(schedule->count, pg->size, chunk);

    if (nelem < 0 || element_offset < 0) {
        return -1;
    }
    *offset = element_offset;
    *bytes = (size_t)nelem * schedule->element_size;
    return 0;
}

static size_t chunk_segment_count(const pg_handle_t *pg,
                                  const pg_collective_schedule_t *schedule,
                                  int chunk)
{
    int nelem = pg_chunk_nelem(schedule->count, pg->size, chunk);

    if (nelem < 0) {
        return 0;
    }
    return segment_count((size_t)nelem * schedule->element_size,
                         schedule->segment_size);
}

static int post_segment(pg_handle_t *pg,
                        const pg_collective_schedule_t *schedule,
                        int step, size_t segment)
{
    int chunk = step_send_chunk(pg, step);
    int element_offset;
    size_t chunk_bytes;
    size_t byte_offset = segment * schedule->segment_size;
    size_t bytes;
    unsigned char *source;
    uint32_t immediate;
    uint64_t work_id;

    if (chunk_geometry(pg, schedule, chunk, &element_offset,
                       &chunk_bytes) != 0) {
        return -1;
    }
    bytes = segment_length(chunk_bytes, schedule->segment_size, segment);
    source = (unsigned char *)pg->buf + pg->work_offset +
             (size_t)element_offset * schedule->element_size + byte_offset;
    immediate = PG_EAGER_IMM(pg->collective_sequence, step, segment);
    work_id = ((uint64_t)(uint32_t)step << 32) | segment;

    if (schedule->mode == PG_TRANSPORT_EAGER) {
        return post_eager_send(pg, source, bytes, immediate, work_id);
    }

    {
        size_t remote_offset;

        if (step < pg->size - 1) {
            remote_offset = pg->staging_offset +
                            (size_t)step * pg->staging_slot_size +
                            byte_offset;
        } else {
            remote_offset = pg->work_offset +
                            (size_t)element_offset * schedule->element_size +
                            byte_offset;
        }
        return post_rendezvous_write(pg, source, bytes, remote_offset,
                                     immediate, work_id);
    }
}

static int validate_receive_completion(
    const pg_handle_t *pg, const pg_collective_schedule_t *schedule,
    const struct ibv_wc *completion, int step, size_t segment,
    size_t expected_bytes)
{
    enum ibv_wc_opcode expected_opcode =
        schedule->mode == PG_TRANSPORT_EAGER ?
        IBV_WC_RECV : IBV_WC_RECV_RDMA_WITH_IMM;
    uint32_t expected_immediate =
        PG_EAGER_IMM(pg->collective_sequence, step, segment);

    if (completion->status != IBV_WC_SUCCESS ||
        completion->opcode != expected_opcode ||
        completion->imm_data != expected_immediate ||
        completion->byte_len != expected_bytes ||
        completion->wr_id >= PG_EAGER_SLOTS) {
        PG_LOG_ERROR(
            "pg_engine",
            "Invalid receive completion: step=%d segment=%zu status=%s opcode=%d imm=0x%x bytes=%u expected_opcode=%d expected_imm=0x%x expected_bytes=%zu",
            step, segment, ibv_wc_status_str(completion->status),
            completion->opcode, completion->imm_data, completion->byte_len,
            expected_opcode, expected_immediate, expected_bytes);
        return -1;
    }
    return 0;
}

static int process_received_segment(
    pg_handle_t *pg, const pg_collective_schedule_t *schedule,
    const struct ibv_wc *completion, int step, size_t segment)
{
    int chunk = step_receive_chunk(pg, step);
    int element_offset;
    size_t chunk_bytes;
    size_t byte_offset = segment * schedule->segment_size;
    size_t bytes;
    unsigned char *destination;

    if (chunk_geometry(pg, schedule, chunk, &element_offset,
                       &chunk_bytes) != 0) {
        return -1;
    }
    bytes = segment_length(chunk_bytes, schedule->segment_size, segment);
    if (validate_receive_completion(pg, schedule, completion, step, segment,
                                    bytes) != 0) {
        return -1;
    }

    destination = (unsigned char *)pg->buf + pg->work_offset +
                  (size_t)element_offset * schedule->element_size +
                  byte_offset;
    if (step < pg->size - 1) {
        const unsigned char *source;

        if (schedule->mode == PG_TRANSPORT_EAGER) {
            source = (const unsigned char *)pg->buf + pg->eager_offset +
                     (size_t)completion->wr_id *
                         PG_EAGER_BUFFER_SIZE;
        } else {
            source = (const unsigned char *)pg->buf + pg->staging_offset +
                     (size_t)step * pg->staging_slot_size + byte_offset;
        }
        return pg_reduce(destination, source,
                         (int)(bytes / schedule->element_size),
                         schedule->datatype, schedule->operation);
    }

    if (schedule->mode == PG_TRANSPORT_EAGER && bytes > 0) {
        memcpy(destination,
               (const unsigned char *)pg->buf + pg->eager_offset +
                   (size_t)completion->wr_id *
                       PG_EAGER_BUFFER_SIZE, bytes);
    }
    return 0;
}

static int send_segment_is_ready(
    const pg_handle_t *pg, const pg_collective_schedule_t *schedule,
    int receive_step, size_t receive_segment, int step, size_t segment)
{
    int dependency_step;

    if (step == schedule->first_step) {
        return 1;
    }

    dependency_step = step - 1;
    if (receive_step > dependency_step) {
        return 1;
    }
    return schedule->pipelined && receive_step == dependency_step &&
           receive_segment > segment;
}

static int post_ready_segments(
    pg_handle_t *pg, const pg_collective_schedule_t *schedule,
    int receive_step, size_t receive_segment,
    int *next_step, size_t *next_segment,
    int *sends_outstanding)
{
    while (*sends_outstanding < PG_QP_DEPTH &&
           *next_step < schedule->end_step) {
        int chunk = step_send_chunk(pg, *next_step);
        size_t count = chunk_segment_count(pg, schedule, chunk);

        if (count == 0 ||
            !send_segment_is_ready(pg, schedule,
                                   receive_step, receive_segment,
                                   *next_step, *next_segment)) {
            return count == 0 ? -1 : 0;
        }
        if (post_segment(pg, schedule, *next_step, *next_segment) != 0) {
            return -1;
        }
        ++*sends_outstanding;
        ++*next_segment;
        if (*next_segment == count) {
            *next_segment = 0;
            ++*next_step;
        }
    }
    return 0;
}

static int validate_schedule_geometry(
    const pg_handle_t *pg, const pg_collective_schedule_t *schedule)
{
    int step;

    for (step = schedule->first_step; step < schedule->end_step; ++step) {
        int chunk = step_receive_chunk(pg, step);
        int element_offset;
        size_t bytes;

        if (chunk_geometry(pg, schedule, chunk, &element_offset, &bytes) != 0) {
            return -1;
        }
        (void)element_offset;
        if (schedule->mode == PG_TRANSPORT_RDVZ &&
            step < pg->size - 1 && bytes > pg->staging_slot_size) {
            return -1;
        }
    }
    return schedule->first_step >= 0 &&
           schedule->first_step < schedule->end_step ? 0 : -1;
}

static int run_collective_steps(pg_handle_t *pg,
                                const pg_collective_schedule_t *schedule)
{
    int receive_step = schedule->first_step;
    size_t receive_segment = 0;
    int next_send_step = schedule->first_step;
    size_t next_send_segment = 0;
    int sends_outstanding = 0;

    if (validate_schedule_geometry(pg, schedule) != 0) {
        return -1;
    }

    if (post_ready_segments(pg, schedule, receive_step, receive_segment,
                            &next_send_step, &next_send_segment,
                            &sends_outstanding) != 0) {
        return -1;
    }

    while (receive_step < schedule->end_step ||
           sends_outstanding > 0 ||
           next_send_step < schedule->end_step) {
        if (receive_step < schedule->end_step) {
            struct ibv_wc completion;
            int count = poll_eager_completion(pg, 1, &completion);

            if (count < 0) {
                return -1;
            }
            if (count == 1) {
                int chunk = step_receive_chunk(pg, receive_step);
                size_t expected_segments =
                    chunk_segment_count(pg, schedule, chunk);

                if (expected_segments == 0 ||
                    receive_segment >= expected_segments ||
                    process_received_segment(pg, schedule, &completion,
                                             receive_step,
                                             receive_segment) != 0 ||
                    post_eager_receive(pg, PG_EAGER_BUFFER_SIZE,
                                       completion.wr_id) != 0) {
                    return -1;
                }
                ++receive_segment;
                if (receive_segment == expected_segments) {
                    receive_segment = 0;
                    ++receive_step;
                }
            }
        }

        if (sends_outstanding > 0) {
            struct ibv_wc completions[PG_WC_BATCH];
            int index;
            int count = poll_completions(pg, 0, completions,
                                         PG_WC_BATCH);

            if (count < 0) {
                return -1;
            }
            for (index = 0; index < count; ++index) {
                if (completions[index].status != IBV_WC_SUCCESS) {
                    PG_LOG_ERROR("pg_engine",
                                 "Send completion failed: status=%s",
                                 ibv_wc_status_str(
                                     completions[index].status));
                    return -1;
                }
            }
            sends_outstanding -= count;
        }

        if (post_ready_segments(pg, schedule,
                                receive_step, receive_segment,
                                &next_send_step, &next_send_segment,
                                &sends_outstanding) != 0) {
            return -1;
        }
    }

    return 0;
}

static int run_reduce_scatter(pg_handle_t *pg, const void *sendbuf,
                              void *recvbuf, int count, DATATYPE datatype,
                              OPERATION operation, pg_transport_mode_t mode)
{
    pg_collective_schedule_t schedule;
    size_t total_bytes;
    int owned_chunk;
    int owned_count;
    int owned_offset;

    if (!pg || !pg->is_connected || !pg->buf || !sendbuf || !recvbuf ||
        count < 0 || pg_validate_reduction(datatype, operation) != 0 ||
        pg->size < 2 || pg->rank < 0 || pg->rank >= pg->size ||
        (mode != PG_TRANSPORT_EAGER && mode != PG_TRANSPORT_RDVZ)) {
        PG_LOG_ERROR("pg_engine", "Invalid Reduce Scatter arguments");
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    schedule.count = count;
    schedule.datatype = datatype;
    schedule.operation = operation;
    schedule.mode = mode;
    schedule.element_size = pg_datatype_size(datatype);
    schedule.segment_size = mode == PG_TRANSPORT_EAGER ?
                            PG_EAGER_BUFFER_SIZE : PG_RDVZ_SEGMENT_SIZE;
    schedule.pipelined = pg->pipeline_enabled;
    schedule.first_step = 0;
    schedule.end_step = pg->size - 1;

    total_bytes = (size_t)count * schedule.element_size;
    if (total_bytes > PG_WORK_BUFFER_SIZE) {
        PG_LOG_ERROR("pg_engine",
                     "Reduce Scatter input exceeds work buffer: %zu bytes",
                     total_bytes);
        return -1;
    }
    memcpy((unsigned char *)pg->buf + pg->work_offset, sendbuf, total_bytes);
    if (run_collective_steps(pg, &schedule) != 0) {
        PG_LOG_ERROR("pg_engine", "Reduce Scatter progress failed");
        return -1;
    }

    owned_chunk = (pg->rank + 1) % pg->size;
    owned_count = pg_chunk_nelem(count, pg->size, owned_chunk);
    owned_offset = pg_chunk_offset(count, pg->size, owned_chunk);
    if (owned_count < 0 || owned_offset < 0) {
        return -1;
    }
    memcpy(recvbuf, (unsigned char *)pg->buf + pg->work_offset +
                        (size_t)owned_offset * schedule.element_size,
           (size_t)owned_count * schedule.element_size);
    return 0;
}

static int run_all_gather(pg_handle_t *pg, void *recvbuf, int count,
                          DATATYPE datatype, pg_transport_mode_t mode)
{
    pg_collective_schedule_t schedule;
    size_t total_bytes;

    if (!pg || !pg->is_connected || !pg->buf || !recvbuf || count < 0 ||
        pg_datatype_size(datatype) == 0 || pg->size < 2 ||
        pg->rank < 0 || pg->rank >= pg->size ||
        (mode != PG_TRANSPORT_EAGER && mode != PG_TRANSPORT_RDVZ)) {
        PG_LOG_ERROR("pg_engine", "Invalid All Gather arguments");
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    schedule.count = count;
    schedule.datatype = datatype;
    schedule.operation = PG_SUM;
    schedule.mode = mode;
    schedule.element_size = pg_datatype_size(datatype);
    schedule.segment_size = mode == PG_TRANSPORT_EAGER ?
                            PG_EAGER_BUFFER_SIZE : PG_RDVZ_SEGMENT_SIZE;
    schedule.pipelined = pg->pipeline_enabled;
    schedule.first_step = pg->size - 1;
    schedule.end_step = 2 * pg->size - 2;

    total_bytes = (size_t)count * schedule.element_size;
    if (total_bytes > PG_WORK_BUFFER_SIZE) {
        PG_LOG_ERROR("pg_engine",
                     "All Gather output exceeds work buffer: %zu bytes",
                     total_bytes);
        return -1;
    }
    if (run_collective_steps(pg, &schedule) != 0) {
        PG_LOG_ERROR("pg_engine", "All Gather progress failed");
        return -1;
    }

    memcpy(recvbuf, (unsigned char *)pg->buf + pg->work_offset, total_bytes);
    ++pg->collective_sequence;
    return 0;
}

int pg_run_eager_reduce_scatter(pg_handle_t *pg, const void *sendbuf,
                                void *recvbuf, int count,
                                DATATYPE datatype, OPERATION operation)
{
    return run_reduce_scatter(pg, sendbuf, recvbuf, count, datatype,
                              operation, PG_TRANSPORT_EAGER);
}

int pg_run_eager_all_gather(pg_handle_t *pg, void *recvbuf, int count,
                            DATATYPE datatype)
{
    return run_all_gather(pg, recvbuf, count, datatype,
                          PG_TRANSPORT_EAGER);
}

int pg_run_rendezvous_reduce_scatter(pg_handle_t *pg, const void *sendbuf,
                                     void *recvbuf, int count,
                                     DATATYPE datatype,
                                     OPERATION operation)
{
    return run_reduce_scatter(pg, sendbuf, recvbuf, count, datatype,
                              operation, PG_TRANSPORT_RDVZ);
}

int pg_run_rendezvous_all_gather(pg_handle_t *pg, void *recvbuf, int count,
                                 DATATYPE datatype)
{
    return run_all_gather(pg, recvbuf, count, datatype,
                          PG_TRANSPORT_RDVZ);
}

int pg_run_all_reduce(pg_handle_t *pg, const void *sendbuf, void *recvbuf,
                      int count, DATATYPE datatype, OPERATION operation,
                      pg_transport_mode_t mode)
{
    pg_collective_schedule_t schedule;
    size_t total_bytes;

    if (!pg || !pg->is_connected || !pg->buf || !sendbuf || !recvbuf ||
        count < 0 || pg_validate_reduction(datatype, operation) != 0 ||
        pg->size < 2 || pg->rank < 0 || pg->rank >= pg->size ||
        (mode != PG_TRANSPORT_EAGER && mode != PG_TRANSPORT_RDVZ)) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    schedule.count = count;
    schedule.datatype = datatype;
    schedule.operation = operation;
    schedule.mode = mode;
    schedule.element_size = pg_datatype_size(datatype);
    schedule.segment_size = mode == PG_TRANSPORT_EAGER ?
                            PG_EAGER_BUFFER_SIZE : PG_RDVZ_SEGMENT_SIZE;
    schedule.pipelined = pg->pipeline_enabled;
    schedule.first_step = 0;
    schedule.end_step = 2 * pg->size - 2;

    total_bytes = (size_t)count * schedule.element_size;
    if (total_bytes > PG_WORK_BUFFER_SIZE) {
        return -1;
    }
    memcpy((unsigned char *)pg->buf + pg->work_offset, sendbuf, total_bytes);
    if (run_collective_steps(pg, &schedule) != 0) {
        return -1;
    }
    memcpy(recvbuf, (unsigned char *)pg->buf + pg->work_offset, total_bytes);
    ++pg->collective_sequence;
    return 0;
}