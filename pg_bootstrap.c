#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#include "pg_internal.h"

int validate_host_list(char **host_list, int host_count)
{
    int i;
    int j;

    if (!host_list || host_count <= 0) {
        fprintf(stderr, "Process-group host list is empty\n");
        return -1;
    }

    for (i = 0; i < host_count; ++i) {
        if (!host_list[i] || host_list[i][0] == '\0') {
            fprintf(stderr, "Process-group host list contains an empty host\n");
            return -1;
        }
        for (j = i + 1; j < host_count; ++j) {
            if (strcmp(host_list[i], host_list[j]) == 0) {
                fprintf(stderr, "Process-group host list contains a duplicate host\n");
                return -1;
            }
        }
    }

    return 0;
}

void free_process_group_hosts(char **host_list, int host_count)
{
    int index;

    if (!host_list) {
        return;
    }
    for (index = 0; index < host_count; ++index) {
        free(host_list[index]);
    }
    free(host_list);
}

int parse_process_group_spec(const char *spec, int *rank,
                             char ***host_list, int *host_count)
{
    char *copy = NULL;
    char *separator;
    char *hosts_part;
    char *token;
    char *saveptr = NULL;
    char *end = NULL;
    char **hosts = NULL;
    long external_rank;
    int count = 0;
    int capacity = 0;

    if (!spec || !rank || !host_list || !host_count) {
        return -1;
    }
    *host_list = NULL;
    *host_count = 0;

    copy = strdup(spec);
    if (!copy) {
        return -1;
    }
    separator = strchr(copy, ':');
    if (!separator || separator == copy || separator[1] == '\0') {
        goto fail;
    }
    hosts_part = separator + 1;
    if (hosts_part[0] == ',' || hosts_part[strlen(hosts_part) - 1] == ',' ||
        strstr(hosts_part, ",,") != NULL) {
        goto fail;
    }
    *separator = '\0';
    errno = 0;
    external_rank = strtol(copy, &end, 10);
    if (errno != 0 || *end != '\0' || external_rank < 1 ||
        external_rank > 65536) {
        goto fail;
    }

    for (token = strtok_r(hosts_part, ",", &saveptr); token;
         token = strtok_r(NULL, ",", &saveptr)) {
        char **grown;

        if (count == capacity) {
            capacity = capacity == 0 ? 4 : capacity * 2;
            grown = realloc(hosts, (size_t)capacity * sizeof(*hosts));
            if (!grown) {
                goto fail;
            }
            hosts = grown;
        }
        hosts[count] = strdup(token);
        if (!hosts[count]) {
            goto fail;
        }
        ++count;
    }

    if (count < 2 || external_rank > count ||
        validate_host_list(hosts, count) != 0) {
        goto fail;
    }
    *rank = (int)external_rank - 1;
    *host_list = hosts;
    *host_count = count;
    free(copy);
    return 0;

fail:
    free(copy);
    free_process_group_hosts(hosts, count);
    return -1;
}

int configure_process_group_topology(pg_handle_t *pg, int rank, int size)
{
    if (!pg || rank < 0 || size <= 0 || rank >= size) {
        fprintf(stderr, "Invalid process-group topology\n");
        return -1;
    }

    pg->rank = rank;
    pg->size = size;
    pg->previous_rank = (rank + size - 1) % size;
    pg->next_rank = (rank + 1) % size;
    return 0;
}

static uint64_t host_to_network_u64(uint64_t value)
{
    return ((uint64_t)htonl((uint32_t)(value >> 32)) << 32) |
           htonl((uint32_t)value);
}

static uint64_t network_to_host_u64(uint64_t value)
{
    return ((uint64_t)ntohl((uint32_t)(value >> 32)) << 32) |
           ntohl((uint32_t)value);
}

void serialize_metadata(const pg_metadata_t *metadata, unsigned char wire[PG_METADATA_WIRE_SIZE])
{
    uint32_t value32;
    uint64_t value64;

    if (!metadata || !wire) {
        return;
    }

    memset(wire, 0, PG_METADATA_WIRE_SIZE);

    value32 = htonl(metadata->rank);
    memcpy(wire + 0, &value32, sizeof(value32));
    value32 = htonl(metadata->size);
    memcpy(wire + 4, &value32, sizeof(value32));
    value32 = htonl(metadata->qpn);
    memcpy(wire + 8, &value32, sizeof(value32));
    value32 = htonl(metadata->psn);
    memcpy(wire + 12, &value32, sizeof(value32));
    value32 = htonl((uint32_t)metadata->lid);
    memcpy(wire + 16, &value32, sizeof(value32));
    value64 = host_to_network_u64(metadata->buffer_addr);
    memcpy(wire + 20, &value64, sizeof(value64));
    value32 = htonl(metadata->rkey);
    memcpy(wire + 28, &value32, sizeof(value32));
    memcpy(wire + 32, metadata->gid.raw, sizeof(metadata->gid.raw));
}

int deserialize_metadata(const unsigned char wire[PG_METADATA_WIRE_SIZE], pg_metadata_t *metadata)
{
    uint32_t value32;
    uint64_t value64;

    if (!wire || !metadata) {
        return -1;
    }

    memcpy(&value32, wire + 0, sizeof(value32));
    metadata->rank = ntohl(value32);
    memcpy(&value32, wire + 4, sizeof(value32));
    metadata->size = ntohl(value32);
    memcpy(&value32, wire + 8, sizeof(value32));
    metadata->qpn = ntohl(value32);
    memcpy(&value32, wire + 12, sizeof(value32));
    metadata->psn = ntohl(value32);
    memcpy(&value32, wire + 16, sizeof(value32));
    metadata->lid = (uint16_t)ntohl(value32);
    memcpy(&value64, wire + 20, sizeof(value64));
    metadata->buffer_addr = network_to_host_u64(value64);
    memcpy(&value32, wire + 28, sizeof(value32));
    metadata->rkey = ntohl(value32);
    memcpy(metadata->gid.raw, wire + 32, sizeof(metadata->gid.raw));
    return 0;
}

int write_full(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    size_t transferred = 0;

    if (fd < 0 || (!buffer && length > 0)) {
        return -1;
    }

    while (transferred < length) {
        ssize_t result = write(fd, cursor + transferred, length - transferred);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return -1;
        }
        transferred += (size_t)result;
    }

    return 0;
}

int read_full(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    size_t transferred = 0;

    if (fd < 0 || (!buffer && length > 0)) {
        return -1;
    }

    while (transferred < length) {
        ssize_t result = read(fd, cursor + transferred, length - transferred);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return -1;
        }
        transferred += (size_t)result;
    }

    return 0;
}

int metadata_from_qp(const pg_handle_t *pg, struct ibv_qp *qp,
                     uint32_t psn, pg_metadata_t *metadata)
{
    struct ibv_port_attr port_attr;

    if (!pg || !metadata || !pg->context || !qp || !pg->mr) {
        return -1;
    }
    memset(metadata, 0, sizeof(*metadata));
    if (ibv_query_port(pg->context, (uint8_t)pg->ib_port, &port_attr) != 0) {
        return -1;
    }
    /* A zeroed GID keeps LID routing; PG_GID_IDX opts into GRH (RoCE). */
    if (pg->gid_index >= 0 &&
        ibv_query_gid(pg->context, (uint8_t)pg->ib_port,
                      pg->gid_index, &metadata->gid) != 0) {
        return -1;
    }

    metadata->rank = (uint32_t)pg->rank;
    metadata->size = (uint32_t)pg->size;
    metadata->qpn = qp->qp_num;
    metadata->psn = psn & 0x00ffffffu;
    metadata->lid = port_attr.lid;
    metadata->buffer_addr = (uint64_t)(uintptr_t)pg->buf;
    metadata->rkey = pg->mr->rkey;
    return 0;
}

int validate_peer_metadata(const pg_metadata_t *metadata,
                          uint32_t expected_rank, uint32_t group_size)
{
    if (!metadata || metadata->rank != expected_rank ||
        metadata->size != group_size || metadata->qpn == 0 ||
        metadata->buffer_addr == 0 || metadata->rkey == 0) {
        return -1;
    }
    return 0;
}

static int create_bootstrap_listener(int port)
{
    struct sockaddr_in address = {0};
    int reuse = 1;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "Could not create bootstrap socket on port %d: %s\n",
            port, strerror(errno));
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        fprintf(stderr, "Could not set bootstrap socket options on port %d: %s\n",
            port, strerror(errno));
        close(fd);
        return -1;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 1) != 0) {
        fprintf(stderr, "Could not listen on bootstrap port %d: %s\n",
            port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_bootstrap_peer(const char *hostname, int port)
{
    struct addrinfo hints = {0};
    struct addrinfo *results = NULL;
    struct addrinfo *candidate;
    char service[16];
    int attempt;
    int fd = -1;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(hostname, service, &hints, &results) != 0) {
        fprintf(stderr, "Could not resolve bootstrap peer %s:%d\n",
                hostname, port);
        return -1;
    }

    fprintf(stderr,
            "Waiting for next rank at %s:%d (timeout: %d seconds)\n",
            hostname, port, PG_BOOTSTRAP_RETRIES / 10);
    fflush(stderr);

    for (attempt = 0; attempt < PG_BOOTSTRAP_RETRIES && fd < 0; ++attempt) {
        if (attempt == 0 || attempt % 50 == 0) {
            fprintf(stderr, "Connecting to bootstrap peer %s:%d (attempt %d/%d)\n",
                    hostname, port, attempt + 1, PG_BOOTSTRAP_RETRIES);
            fflush(stderr);
        }
        for (candidate = results; candidate; candidate = candidate->ai_next) {
            fd = socket(candidate->ai_family, candidate->ai_socktype,
                        candidate->ai_protocol);
            if (fd >= 0 && connect(fd, candidate->ai_addr,
                                   candidate->ai_addrlen) == 0) {
                fprintf(stderr, "Connected to bootstrap peer %s:%d\n",
                        hostname, port);
                fflush(stderr);
                break;
            }
            if (fd >= 0) {
                if (attempt == 0 || attempt % 50 == 0) {
                    fprintf(stderr, "Bootstrap connect to %s:%d failed: %s\n",
                            hostname, port, strerror(errno));
                    fflush(stderr);
                }
                close(fd);
                fd = -1;
            }
        }
        if (fd < 0) {
            struct timespec delay = {0, 100000000};
            nanosleep(&delay, NULL);
        }
    }

    freeaddrinfo(results);
    return fd;
}

static int send_peer_metadata(int fd, const pg_metadata_t *local)
{
    unsigned char local_wire[PG_METADATA_WIRE_SIZE];

    serialize_metadata(local, local_wire);
    fprintf(stderr, "[bootstrap] sending %zu metadata bytes on socket %d\n",
        sizeof(local_wire), fd);
    fflush(stderr);
    if (write_full(fd, local_wire, sizeof(local_wire)) != 0) {
        fprintf(stderr, "Could not send bootstrap metadata: %s\n",
            strerror(errno));
        return -1;
    }
    fprintf(stderr, "[bootstrap] metadata sent on socket %d\n", fd);
    fflush(stderr);
    return 0;
}

static int receive_peer_metadata(int fd, pg_metadata_t *remote)
{
    unsigned char remote_wire[PG_METADATA_WIRE_SIZE];

    fprintf(stderr, "[bootstrap] waiting for %zu metadata bytes on socket %d\n",
            sizeof(remote_wire), fd);
    fflush(stderr);
    if (read_full(fd, remote_wire, sizeof(remote_wire)) != 0) {
        fprintf(stderr, "Could not receive bootstrap metadata: %s\n",
                strerror(errno));
        return -1;
    }
    fprintf(stderr, "[bootstrap] received metadata on socket %d\n", fd);
    fflush(stderr);
    if (deserialize_metadata(remote_wire, remote) != 0) {
        fprintf(stderr, "Could not decode bootstrap metadata\n");
        return -1;
    }
    return 0;
}

int bootstrap_ring_barrier(pg_handle_t *pg)
{
    unsigned char token = 0x42;
    int lap;

    if (!pg || pg->sock_previous < 0 || pg->sock_next < 0) {
        return -1;
    }

    for (lap = 0; lap < 2; ++lap) {
        if (pg->rank == 0) {
            if (write_full(pg->sock_next, &token, 1) != 0 ||
                read_full(pg->sock_previous, &token, 1) != 0) {
                return -1;
            }
        } else if (read_full(pg->sock_previous, &token, 1) != 0 ||
                   write_full(pg->sock_next, &token, 1) != 0) {
            return -1;
        }
    }
    return 0;
}

int bootstrap_ring(pg_handle_t *pg, char **host_list, int host_count)
{
    pg_metadata_t local_metadata;
    pg_metadata_t remote_metadata;
    int listener_fd = -1;
    int outgoing_fd = -1;
    int incoming_fd = -1;
    int listen_port;
    int next_port;
    int rc = -1;
    uint32_t psn_next;
    uint32_t psn_previous;

    if (!pg || !host_list || host_count != pg->size || pg->size <= 0 ||
        pg->bootstrap_base_port + pg->size >= 65536) {
        return -1;
    }
    psn_next = (uint32_t)(rand() & 0x00ffffffu);
    psn_previous = (uint32_t)(rand() & 0x00ffffffu);
    if (metadata_from_qp(pg, pg->qp_send, psn_next, &local_metadata) != 0) {
        return -1;
    }

    listen_port = pg->bootstrap_base_port + pg->rank;
    next_port = pg->bootstrap_base_port + pg->next_rank;
    PG_LOG_INFO("pg_bootstrap",
                "Rank %d preparing listener on port %d; connecting to rank %d at %s:%d",
                pg->rank, listen_port, pg->next_rank,
                host_list[pg->next_rank], next_port);
    listener_fd = create_bootstrap_listener(listen_port);
    if (listener_fd < 0) {
        return -1;
    }
    PG_LOG_DEBUG("pg_bootstrap", "Rank %d listening on port %d",
                 pg->rank, listen_port);

    outgoing_fd = connect_bootstrap_peer(host_list[pg->next_rank], next_port);
    if (outgoing_fd < 0) {
        goto cleanup;
    }
    PG_LOG_DEBUG("pg_bootstrap",
                 "Rank %d connected to next rank; waiting for previous rank on port %d",
                 pg->rank, listen_port);
    incoming_fd = accept(listener_fd, NULL, NULL);
    if (incoming_fd < 0) {
        PG_LOG_ERROR("pg_bootstrap", "Rank %d accept failed: %s",
                 pg->rank, strerror(errno));
        goto cleanup;
    }
    PG_LOG_DEBUG("pg_bootstrap", "Rank %d accepted previous-rank connection",
                 pg->rank);

    PG_LOG_DEBUG("pg_bootstrap", "Rank %d sending metadata to rank %d",
                 pg->rank, pg->next_rank);
    if (send_peer_metadata(outgoing_fd, &local_metadata) != 0 ||
        receive_peer_metadata(incoming_fd, &remote_metadata) != 0 ||
        metadata_from_qp(pg, pg->qp_recv, psn_previous, &local_metadata) != 0 ||
        send_peer_metadata(incoming_fd, &local_metadata) != 0 ||
        receive_peer_metadata(outgoing_fd, &pg->next_peer) != 0 ||
        validate_peer_metadata(&pg->next_peer, (uint32_t)pg->next_rank,
                               (uint32_t)pg->size) != 0 ||
        validate_peer_metadata(&remote_metadata, (uint32_t)pg->previous_rank,
                               (uint32_t)pg->size) != 0) {
        PG_LOG_ERROR("pg_bootstrap",
                 "Rank %d peer metadata exchange or validation failed",
                 pg->rank);
        goto cleanup;
    }
    PG_LOG_DEBUG("pg_bootstrap",
                 "Rank %d validated peer metadata for ranks %d and %d",
                 pg->rank, pg->previous_rank, pg->next_rank);
    pg->previous_peer = remote_metadata;
    if (connect_rdma_qp(pg, pg->qp_send, psn_next, &pg->next_peer) != 0 ||
        connect_rdma_qp(pg, pg->qp_recv, psn_previous, &pg->previous_peer) != 0) {
        PG_LOG_ERROR("pg_bootstrap",
                 "Rank %d could not move directional QPs to RTS",
                 pg->rank);
        goto cleanup;
    }
    pg->sock_next = outgoing_fd;
    pg->sock_previous = incoming_fd;
    outgoing_fd = -1;
    incoming_fd = -1;
    if (bootstrap_ring_barrier(pg) != 0) {
        PG_LOG_ERROR("pg_bootstrap", "Rank %d ring setup barrier failed",
                 pg->rank);
        goto cleanup;
    }
    pg->is_connected = 1;
    PG_LOG_INFO("pg_bootstrap",
                "Rank %d ring setup completed; transport is ready",
                pg->rank);
    rc = 0;

cleanup:
    if (incoming_fd >= 0) {
        close(incoming_fd);
    }
    if (outgoing_fd >= 0) {
        close(outgoing_fd);
    }
    close(listener_fd);
    return rc;
}
