#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/verbs.h>

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

/*
 * usage:
 *  Print the command-line usage for the process-group executable.
 *  The expected invocation is either:
 *      <prog> -myindex <rank> -list <host1> [host2 ...]
 *  or a single host name in the minimal standalone form.
 */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -myindex <rank> -list <host1> [host2 ...]\n"
            "       %s <hostname>\n",
            prog, prog);
}

/*
 * parse_rank_and_hosts:
 *  Parse the command line and extract the process-group information.
 *
 *  Parameters:
 *   - argc, argv: command-line arguments
 *   - myindex: selected local rank index
 *   - host_list: list of hostnames in rank order
 *   - host_count: number of hosts in the group
 *
 *  Returns:
 *   - 0 on success
 *   - -1 on invalid input or allocation failure
 */
static int parse_rank_and_hosts(int argc, char **argv, int *myindex, char ***host_list, int *host_count)
{
    int i;

    *myindex = -1;
    *host_count = 0;
    *host_list = NULL;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-myindex") == 0 && i + 1 < argc) {
            char *end = NULL;
            long value = strtol(argv[++i], &end, 10);
            if (end == argv[i] || value < 0 || value > 65535) {
                fprintf(stderr, "Invalid -myindex value\n");
                return -1;
            }
            *myindex = (int)value;
        } else if (strcmp(argv[i], "-list") == 0) {
            int count = 0;
            char **list = NULL;
            int j;

            for (j = i + 1; j < argc; ++j) {
                if (argv[j][0] == '-') {
                    break;
                }
                ++count;
            }

            list = calloc((size_t)count, sizeof(*list));
            if (!list) {
                fprintf(stderr, "Out of memory while parsing host list\n");
                return -1;
            }

            for (j = 0; j < count; ++j) {
                list[j] = strdup(argv[i + 1 + j]);
                if (!list[j]) {
                    fprintf(stderr, "Out of memory while copying host list\n");
                    free(list);
                    return -1;
                }
            }

            *host_list = list;
            *host_count = count;
            i += count;
        }
    }

    return 0;
}

/*
 * destroy_process_group:
 *  Release every resource that may have been created for a process group.
 *
 * Initialization can fail after any individual step. All fields in pg start
 * as NULL because the structure is allocated with calloc, so this same helper
 * can clean up both a complete and a partially initialized process group.
 * Verbs objects are destroyed before the objects they depend on.
 */
static void destroy_process_group(pg_handle_t *pg)
{
    if (!pg) {
        return;
    }

    if (pg->qp) {
        ibv_destroy_qp(pg->qp);
    }
    if (pg->cq) {
        ibv_destroy_cq(pg->cq);
    }
    if (pg->mr) {
        ibv_dereg_mr(pg->mr);
    }
    if (pg->pd) {
        ibv_dealloc_pd(pg->pd);
    }
    if (pg->context) {
        ibv_close_device(pg->context);
    }

    free(pg->buf);
    free(pg->hostname);
    free(pg);
}

/*
 * find_active_port:
 *  Find the first physical device port that is currently active.
 *
 * A device may expose multiple ports, and a valid device does not guarantee
 * that every port is usable. The selected port is needed when the QP enters
 * INIT and will also be used later when connecting the QP to a peer.
 */
static int find_active_port(struct ibv_context *context, int *port_num)
{
    struct ibv_device_attr device_attr;
    struct ibv_port_attr port_attr;
    int port;

    if (ibv_query_device(context, &device_attr) != 0) {
        fprintf(stderr, "Could not query device attributes\n");
        return -1;
    }

    for (port = 1; port <= device_attr.phys_port_cnt; ++port) {
        if (ibv_query_port(context, (uint8_t)port, &port_attr) == 0 &&
            port_attr.state == IBV_PORT_ACTIVE) {
            *port_num = port;
            return 0;
        }
    }

    fprintf(stderr, "No active Verbs port found\n");
    return -1;
}

/*
 * validate_host_list:
 *  Check the host list before it is used to define the process-group layout.
 *
 *  The list order is significant: list[i] identifies rank i. Every host must
 *  be non-empty and appear only once so that all ranks can agree on one ring.
 */
static int validate_host_list(char **host_list, int host_count)
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

/*
 * configure_process_group_topology:
 *  Store a rank's position in the logical ring and calculate its neighbors.
 *
 *  The modulo operation makes the ring wrap around: rank 0 receives from the
 *  last rank, and the last rank sends to rank 0. This function only configures
 *  local metadata; it does not open sockets or communicate with peers.
 */
static int configure_process_group_topology(pg_handle_t *pg, int rank, int size)
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

/*
 * serialize_metadata / deserialize_metadata:
 *  Convert bootstrap metadata to and from a 48-byte TCP message.
 *
 * Each integer is written in network byte order. Fixed offsets make the wire
 * format independent of compiler padding and structure alignment.
 */
static void serialize_metadata(
    const pg_metadata_t *metadata, unsigned char wire[PG_METADATA_WIRE_SIZE])
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

static int deserialize_metadata(
    const unsigned char wire[PG_METADATA_WIRE_SIZE], pg_metadata_t *metadata)
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

/*
 * write_full / read_full:
 *  Transfer exactly length bytes over a TCP socket.
 *
 * TCP is a byte stream: one write is not guaranteed to match one read. These
 * helpers continue until the complete message is transferred or an error or
 * orderly peer close occurs. EINTR is retried because signals may interrupt
 * system calls without indicating a connection failure.
 */
static int write_full(int fd, const void *buffer, size_t length)
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

static int read_full(int fd, void *buffer, size_t length)
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

static int metadata_from_process_group(const pg_handle_t *pg,
                                      pg_metadata_t *metadata)
{
    struct ibv_port_attr port_attr;

    if (!pg || !metadata || !pg->context || !pg->qp || !pg->mr) {
        return -1;
    }
    memset(metadata, 0, sizeof(*metadata));
    if (ibv_query_port(pg->context, (uint8_t)pg->ib_port, &port_attr) != 0 ||
        ibv_query_gid(pg->context, (uint8_t)pg->ib_port, 0, &metadata->gid) != 0) {
        return -1;
    }

    metadata->rank = (uint32_t)pg->rank;
    metadata->size = (uint32_t)pg->size;
    metadata->qpn = pg->qp->qp_num;
    metadata->psn = (uint32_t)(rand() & 0x00ffffff);
    metadata->lid = port_attr.lid;
    metadata->buffer_addr = (uint64_t)(uintptr_t)pg->buf;
    metadata->rkey = pg->mr->rkey;
    return 0;
}

static int validate_peer_metadata(const pg_metadata_t *metadata,
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

    for (attempt = 0; attempt < PG_BOOTSTRAP_RETRIES && fd < 0; ++attempt) {
        if (attempt == 0 || attempt % 10 == 0) {
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
                if (attempt == 0 || attempt % 10 == 0) {
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

/*
 * bootstrap_ring:
 *  Create one TCP control connection for each logical ring edge.
 *
 * Every rank listens on its own rank-derived port and connects to its next
 * rank. The accepted connection comes from the previous rank. This avoids
 * opening two TCP connections for the same ring edge while allowing metadata
 * to flow in both directions on one full-duplex socket.
 */
static int bootstrap_ring(pg_handle_t *pg, char **host_list, int host_count)
{
    pg_metadata_t local_metadata;
    pg_metadata_t remote_metadata;
    int listener_fd = -1;
    int outgoing_fd = -1;
    int incoming_fd = -1;
    int listen_port;
    int next_port;
    int rc = -1;

    if (!pg || !host_list || host_count != pg->size || pg->size <= 0 ||
        PG_BOOTSTRAP_BASE_PORT + pg->size >= 65536) {
        return -1;
    }
    if (metadata_from_process_group(pg, &local_metadata) != 0) {
        return -1;
    }

    listen_port = PG_BOOTSTRAP_BASE_PORT + pg->rank;
    next_port = PG_BOOTSTRAP_BASE_PORT + pg->next_rank;
    PG_TRACE(pg->rank, "Preparing listener on port %d; connecting to rank %d at %s:%d",
             listen_port, pg->next_rank, host_list[pg->next_rank], next_port);
    listener_fd = create_bootstrap_listener(listen_port);
    if (listener_fd < 0) {
        return -1;
    }
    PG_TRACE(pg->rank, "Listening on port %d", listen_port);

    outgoing_fd = connect_bootstrap_peer(host_list[pg->next_rank], next_port);
    if (outgoing_fd < 0) {
        goto cleanup;
    }
    PG_TRACE(pg->rank, "Outgoing connection established; waiting for previous rank on port %d",
             listen_port);
    incoming_fd = accept(listener_fd, NULL, NULL);
    if (incoming_fd < 0) {
        PG_TRACE(pg->rank, "accept failed: %s", strerror(errno));
        goto cleanup;
    }
    PG_TRACE(pg->rank, "Incoming connection accepted");

    PG_TRACE(pg->rank, "Sending metadata to next rank %d", pg->next_rank);
    if (send_peer_metadata(outgoing_fd, &local_metadata) != 0 ||
        receive_peer_metadata(incoming_fd, &remote_metadata) != 0 ||
        send_peer_metadata(incoming_fd, &local_metadata) != 0 ||
        receive_peer_metadata(outgoing_fd, &pg->next_peer) != 0 ||
        validate_peer_metadata(&pg->next_peer, (uint32_t)pg->next_rank,
                               (uint32_t)pg->size) != 0 ||
        validate_peer_metadata(&remote_metadata, (uint32_t)pg->previous_rank,
                               (uint32_t)pg->size) != 0) {
        PG_TRACE(pg->rank, "Peer metadata exchange or validation failed");
        goto cleanup;
    }
    PG_TRACE(pg->rank, "Peer metadata validated for ranks %d and %d",
             pg->previous_rank, pg->next_rank);
    pg->previous_peer = remote_metadata;
    if (pg->size == 2) {
        pg->previous_peer = pg->next_peer;
    }
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

/*
 * connect_process_group:
 *  Initialize the local process-group handle and local RDMA resources.
 *
 *  Current phase flow:
 *   1. Allocate the opaque handle and copy the hostname.
 *   2. Discover and open the first available Verbs device.
 *   3. Find an active physical port.
 *   4. Allocate a protection domain and registered communication buffer.
 *   5. Create a completion queue and a reliable-connected queue pair.
 *   6. Move the new QP from RESET to INIT.
 *
 *  This function does not contact another process yet. The QP remains in INIT
 *  until a later phase exchanges remote metadata and moves it through RTR and
 *  RTS. Therefore is_connected currently means that local setup succeeded,
 *  not that a remote rank is connected.
 *
 *  Parameters:
 *   - servername: the host associated with this rank in the group
 *   - pg_handle: output pointer where the initialized handle is stored
 *
 *  Returns:
 *   - 0 on success
 *   - -1 on failure
 */
int connect_process_group(char *servername, void **pg_handle)
{
    /* Temporary list returned by the Verbs device-discovery API. */
    struct ibv_device **device_list = NULL;
    /* Number of entries written into device_list. */
    int device_count = 0;
    /* Internal state returned to the caller through pg_handle. */
    pg_handle_t *pg = NULL;

    /* Without this output address there is nowhere to return the new handle. */
    if (!pg_handle) {
        fprintf(stderr, "Invalid process-group handle pointer\n");
        return -1;
    }

    /* calloc gives every resource pointer a known NULL value for cleanup. */
    pg = calloc(1, sizeof(*pg));
    if (!pg) {
        fprintf(stderr, "Could not allocate process-group handle\n");
        return -1;
    }

    /* Rank and group size are placeholders until the topology phase. */
    pg->rank = 0;
    pg->size = 1;
    pg->is_connected = 0;

    /* Keep an owned hostname copy; the caller retains ownership of its input. */
    pg->hostname = servername ? strdup(servername) : NULL;
    if (servername && !pg->hostname) {
        fprintf(stderr, "Could not copy process-group hostname\n");
        destroy_process_group(pg);
        return -1;
    }

    /* Ask libibverbs which RDMA devices are visible on this host. */
    device_list = ibv_get_device_list(&device_count);
    if (!device_list || device_count == 0) {
        fprintf(stderr, "No InfiniBand Verbs device found\n");
        if (device_list) {
            ibv_free_device_list(device_list);
        }
        destroy_process_group(pg);
        return -1;
    }

    /* Device selection is intentionally simple for this first milestone. */
    pg->device = device_list[0];

    /* The context is the process's active handle for using the device. */
    pg->context = ibv_open_device(pg->device);
    if (!pg->context) {
        fprintf(stderr, "Could not open Verbs device %s\n",
                ibv_get_device_name(pg->device));
        ibv_free_device_list(device_list);
        destroy_process_group(pg);
        return -1;
    }
    /* The opened context remains valid after this temporary list is freed. */
    ibv_free_device_list(device_list);
    device_list = NULL;

    if (find_active_port(pg->context, &pg->ib_port) != 0) {
        destroy_process_group(pg);
        return -1;
    }

    /* The PD groups the QP and memory region under one access boundary. */
    pg->pd = ibv_alloc_pd(pg->context);
    if (!pg->pd) {
        fprintf(stderr, "Could not allocate Verbs protection domain\n");
        destroy_process_group(pg);
        return -1;
    }

    /* This buffer is a placeholder for future send/receive/chunk buffers. */
    pg->buf = calloc(1, PG_BUFFER_SIZE);
    pg->buf_size = PG_BUFFER_SIZE;
    if (!pg->buf) {
        fprintf(stderr, "Could not allocate process-group buffer\n");
        destroy_process_group(pg);
        return -1;
    }

    /* Registration makes the buffer accessible to the RDMA hardware. */
    pg->mr = ibv_reg_mr(pg->pd, pg->buf, pg->buf_size,
                        IBV_ACCESS_LOCAL_WRITE |
                        IBV_ACCESS_REMOTE_WRITE |
                        IBV_ACCESS_REMOTE_READ);
    if (!pg->mr) {
        fprintf(stderr, "Could not register process-group buffer\n");
        destroy_process_group(pg);
        return -1;
    }

    /* The CQ will report completion of future SEND/RECV/RDMA operations. */
    pg->cq = ibv_create_cq(pg->context, PG_CQ_CAPACITY, NULL, NULL, 0);
    if (!pg->cq) {
        fprintf(stderr, "Could not create process-group completion queue\n");
        destroy_process_group(pg);
        return -1;
    }

    /* Create one RC QP; later phases will connect it to a ring neighbor. */
    {
        struct ibv_qp_init_attr qp_attr = {
            .send_cq = pg->cq,
            .recv_cq = pg->cq,
            .cap = {
                .max_send_wr = PG_QP_DEPTH,
                .max_recv_wr = PG_QP_DEPTH,
                .max_send_sge = 1,
                .max_recv_sge = 1,
                .max_inline_data = 0
            },
            .qp_type = IBV_QPT_RC,
            .sq_sig_all = 1
        };

        pg->qp = ibv_create_qp(pg->pd, &qp_attr);
        if (!pg->qp) {
            fprintf(stderr, "Could not create process-group queue pair\n");
            destroy_process_group(pg);
            return -1;
        }
    }

    /* A QP must be in INIT before it can be connected to a remote QP. */
    {
        struct ibv_qp_attr qp_attr = {
            .qp_state = IBV_QPS_INIT,
            .pkey_index = 0,
            .port_num = (uint8_t)pg->ib_port,
            .qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
                               IBV_ACCESS_REMOTE_READ
        };

        if (ibv_modify_qp(pg->qp, &qp_attr,
                          IBV_QP_STATE |
                          IBV_QP_PKEY_INDEX |
                          IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS) != 0) {
            fprintf(stderr, "Could not move process-group queue pair to INIT\n");
            destroy_process_group(pg);
            return -1;
        }
    }

    /* Only local initialization is complete; remote connection comes later. */
    pg->is_connected = 1;

    *pg_handle = pg;
    return 0;
}

/*
 * pg_all_reduce:
 *  Public collective API for the all-reduce operation.
 *
 *  This function is the main collective entry point. In the final design it will:
 *  - validate the datatype and operation
 *  - divide the data into ring chunks
 *  - run Reduce Scatter
 *  - run All Gather
 *  - write the final reduced result into recvbuf
 *
 *  Parameters:
 *   - sendbuf: input data for the local rank
 *   - recvbuf: output buffer for the reduced result
 *   - count: number of elements to reduce
 *   - datatype: element type
 *   - op: reduction operation
 *   - pg_handle: process-group handle for the local rank
 *
 *  Returns:
 *   - 0 on success
 *   - -1 on failure
 */
int pg_all_reduce(void *sendbuf, void *recvbuf, int count, DATATYPE datatype, OPERATION op, void *pg_handle)
{
    pg_handle_t *pg = (pg_handle_t *)pg_handle;

    (void)sendbuf;
    (void)recvbuf;
    (void)count;
    (void)datatype;
    (void)op;

    if (!pg) {
        fprintf(stderr, "Invalid process-group handle\n");
        return -1;
    }

    fprintf(stderr, "pg_all_reduce not implemented in the skeleton build\n");
    return -1;
}

/*
 * pg_close:
 *  Release the process-group handle and any associated local resources.
 *
 *  In the final implementation this function must free all registered memory,
 *  destroy queue pairs, and clean up the verbs objects in reverse dependency order.
 */
int pg_close(void *pg_handle)
{
    pg_handle_t *pg = (pg_handle_t *)pg_handle;

    if (!pg) {
        return 0;
    }

    destroy_process_group(pg);
    return 0;
}

/*
 * main:
 *  Program entry point.
 *
 *  Responsibilities:
 *   - parse the command line
 *   - determine the rank and process-group membership
 *   - initialize the process-group handle
 *   - exercise the collective interface in the current skeleton build
 *
 *  This is the orchestration layer that connects user input to the process-group state.
 */
int main(int argc, char **argv)
{
    char **host_list = NULL;
    int host_count = 0;
    int myindex = -1;
    char *hostname = NULL;
    void *pg_handle = NULL;
    int rc;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    rc = parse_rank_and_hosts(argc, argv, &myindex, &host_list, &host_count);
    if (rc != 0) {
        free(host_list);
        return 1;
    }

    /* Distributed mode: validate the complete rank-to-host mapping first. */
    if (myindex >= 0 && host_count > 0) {
        if (validate_host_list(host_list, host_count) != 0) {
            free(host_list);
            return 1;
        }
        if (myindex >= host_count) {
            fprintf(stderr, "-myindex is out of range for the supplied -list\n");
            free(host_list);
            return 1;
        }
        hostname = host_list[myindex];
    /* Standalone mode represents a one-rank group for local development. */
    } else if (argc == 2) {
        hostname = argv[1];
    } else {
        usage(argv[0]);
        free(host_list);
        return 1;
    }

    /* Create local RDMA state before adding the ring metadata to the handle. */
    rc = connect_process_group(hostname, &pg_handle);
    if (rc != 0) {
        fprintf(stderr, "Failed to initialize process group\n");
        free(host_list);
        return 1;
    }

    /* The host-list rank and size now become part of the opaque handle. */
    if (configure_process_group_topology((pg_handle_t *)pg_handle,
                                         myindex >= 0 ? myindex : 0,
                                         myindex >= 0 ? host_count : 1) != 0) {
        pg_close(pg_handle);
        free(host_list);
        return 1;
    }

    if (myindex >= 0 && host_count > 1 &&
        bootstrap_ring((pg_handle_t *)pg_handle, host_list, host_count) != 0) {
        fprintf(stderr, "Failed to bootstrap ring peers\n");
        pg_close(pg_handle);
        free(host_list);
        return 1;
    }

    fprintf(stderr, "Exercise 3 local Verbs state initialized for rank %d\n",
            ((pg_handle_t *)pg_handle)->rank);

    free(host_list);
    pg_close(pg_handle);
    return 0;
}
