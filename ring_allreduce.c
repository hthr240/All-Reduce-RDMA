#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    int is_connected;
    char *hostname;
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    void *buf;
    size_t buf_size;
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
 * connect_process_group:
 *  Initialize the local process-group handle.
 *
 *  This is the bootstrap function that prepares the per-rank state used by the
 *  ring collective. In the final implementation it will also initialize the RDMA
 *  device state, create queue pairs, and exchange metadata with neighbors.
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
    pg_handle_t *pg = NULL;

    if (!pg_handle) {
        fprintf(stderr, "Invalid process-group handle pointer\n");
        return -1;
    }

    pg = calloc(1, sizeof(*pg));
    if (!pg) {
        fprintf(stderr, "Could not allocate process-group handle\n");
        return -1;
    }

    pg->rank = 0;
    pg->size = 1;
    pg->is_connected = 0;
    pg->hostname = servername ? strdup(servername) : NULL;

    /*
     * Exercise 3 skeleton:
     * - device discovery and Verbs setup will be filled in later
     * - this stub intentionally keeps the project buildable
     */
    pg->context = NULL;
    pg->pd = NULL;
    pg->cq = NULL;
    pg->qp = NULL;
    pg->mr = NULL;
    pg->buf = NULL;
    pg->buf_size = 0;

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

    free(pg->hostname);
    free(pg);
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

    if (myindex >= 0 && host_count > 0) {
        if (myindex >= host_count) {
            fprintf(stderr, "-myindex is out of range for the supplied -list\n");
            free(host_list);
            return 1;
        }
        hostname = host_list[myindex];
    } else if (argc == 2) {
        hostname = argv[1];
    } else {
        usage(argv[0]);
        free(host_list);
        return 1;
    }

    rc = connect_process_group(hostname, &pg_handle);
    if (rc != 0) {
        fprintf(stderr, "Failed to initialize process group\n");
        free(host_list);
        return 1;
    }

    fprintf(stderr, "Exercise 3 process-group skeleton initialized for rank %d\n", myindex >= 0 ? myindex : 0);

    free(host_list);
    pg_close(pg_handle);
    return 0;
}
