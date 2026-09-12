#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pg.h"
#include "pg_log.h"

#define BENCHMARK_MAX_BYTES (4u << 20)

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s -myindex <one-based-rank> -list <host1> <host2> [host...] [-token | -check <count> | -bench] [-repeat <count>] [-int-only] [-mode auto|eager|rdvz] [-nopipe] [-dtype int32|double] [-op sum|prod] [-iters <count>] [-threshold <bytes>]\n",
            program);
}

static double monotonic_seconds(void)
{
    struct timespec timestamp;

    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return (double)timestamp.tv_sec + (double)timestamp.tv_nsec / 1e9;
}

static int benchmark_iterations(size_t bytes)
{
    if (bytes <= 4096) {
        return 2000;
    }
    if (bytes <= 262144) {
        return 500;
    }
    return 100;
}

static int build_group_spec(int rank, char **hosts, int host_count,
                            char **spec_out)
{
    size_t length = 32;
    char *spec;
    int index;

    for (index = 0; index < host_count; ++index) {
        length += strlen(hosts[index]) + 1;
    }
    spec = malloc(length);
    if (!spec) {
        return -1;
    }
    snprintf(spec, length, "%d:", rank + 1);
    for (index = 0; index < host_count; ++index) {
        strcat(spec, index == 0 ? "" : ",");
        strcat(spec, hosts[index]);
    }
    *spec_out = spec;
    return 0;
}

static int check_all_reduce(void *handle, int rank, int nranks, int count,
                            int repeat)
{
    int32_t *sendbuf;
    int32_t *recvbuf;
    int index;
    int source_rank;

    if (count < 0) {
        return -1;
    }
    sendbuf = malloc((size_t)(count > 0 ? count : 1) * sizeof(*sendbuf));
    recvbuf = malloc((size_t)(count > 0 ? count : 1) * sizeof(*recvbuf));
    if (!sendbuf || !recvbuf) {
        free(sendbuf);
        free(recvbuf);
        return -1;
    }
    for (int iteration = 0; iteration < repeat; ++iteration) {
        for (index = 0; index < count; ++index) {
            sendbuf[index] = (rank + 1) * 1000 + index + iteration;
        }
        if (pg_all_reduce(sendbuf, recvbuf, count, PG_INT32, PG_SUM, handle) != 0) {
            free(sendbuf);
            free(recvbuf);
            return -1;
        }
        for (index = 0; index < count; ++index) {
            int32_t expected = 0;

            for (source_rank = 0; source_rank < nranks; ++source_rank) {
                expected += (source_rank + 1) * 1000 + index + iteration;
            }
            if (recvbuf[index] != expected) {
                fprintf(stderr, "FAIL rank=%d iteration=%d index=%d got=%d expected=%d\n",
                        rank, iteration, index, recvbuf[index], expected);
                free(sendbuf);
                free(recvbuf);
                return -1;
            }
        }
    }
    fprintf(stderr, "PASS all_reduce rank=%d count=%d int32 sum repeat=%d\n",
            rank, count, repeat);
    free(sendbuf);
    free(recvbuf);
    return 0;
}

static int check_double_product_in_place(void *handle, int rank, int nranks,
                                         int count, int repeat)
{
    double *buffer;
    int index;
    int source_rank;

    buffer = malloc((size_t)(count > 0 ? count : 1) * sizeof(*buffer));
    if (!buffer) {
        return -1;
    }
    for (int iteration = 0; iteration < repeat; ++iteration) {
        for (index = 0; index < count; ++index) {
            buffer[index] = (double)(rank + 1) +
                            (double)(index + iteration) / 100.0;
        }
        if (pg_all_reduce(buffer, buffer, count, PG_DOUBLE, PG_PROD,
                          handle) != 0) {
            free(buffer);
            return -1;
        }
        for (index = 0; index < count; ++index) {
            double expected = 1.0;

            for (source_rank = 0; source_rank < nranks; ++source_rank) {
                expected *= (double)(source_rank + 1) +
                            (double)(index + iteration) / 100.0;
            }
            if (fabs(buffer[index] - expected) >
                1e-12 * (fabs(expected) + 1.0)) {
                fprintf(stderr,
                        "FAIL double product rank=%d iteration=%d index=%d got=%g expected=%g\n",
                        rank, iteration, index, buffer[index], expected);
                free(buffer);
                return -1;
            }
        }
    }
    fprintf(stderr,
            "PASS all_reduce rank=%d count=%d double product in-place repeat=%d\n",
            rank, count, repeat);
    free(buffer);
    return 0;
}

static int run_benchmark(void *handle, int rank, DATATYPE datatype,
                         OPERATION operation, int iterations_override)
{
    size_t element_size = datatype == PG_INT32 ? sizeof(int32_t) : sizeof(double);
    void *sendbuf = malloc(BENCHMARK_MAX_BYTES);
    void *recvbuf = malloc(BENCHMARK_MAX_BYTES);
    size_t bytes;
    size_t index;

    if (!sendbuf || !recvbuf) {
        free(sendbuf);
        free(recvbuf);
        return -1;
    }
    for (index = 0; index < BENCHMARK_MAX_BYTES / element_size; ++index) {
        if (datatype == PG_INT32) {
            ((int32_t *)sendbuf)[index] = operation == PG_SUM ? rank + 1 : 1;
        } else {
            ((double *)sendbuf)[index] = operation == PG_SUM ?
                (double)(rank + 1) : 1.0;
        }
    }

    for (bytes = 8; bytes <= BENCHMARK_MAX_BYTES; bytes <<= 1) {
        int count = (int)(bytes / element_size);
        int iterations = iterations_override > 0 ? iterations_override :
                         benchmark_iterations(bytes);
        int warmups = iterations / 10 > 10 ? iterations / 10 : 10;
        double started;
        double latency_us;
        int iteration;

        for (iteration = 0; iteration < warmups; ++iteration) {
            if (pg_all_reduce(sendbuf, recvbuf, count, datatype, operation,
                              handle) != 0) {
                goto fail;
            }
        }
        started = monotonic_seconds();
        for (iteration = 0; iteration < iterations; ++iteration) {
            if (pg_all_reduce(sendbuf, recvbuf, count, datatype, operation,
                              handle) != 0) {
                goto fail;
            }
        }
        latency_us = (monotonic_seconds() - started) * 1e6 / iterations;
        if (rank == 0) {
            printf("%zu\t%.3f\tusec\n", bytes, latency_us);
            fflush(stdout);
        }
    }

    free(sendbuf);
    free(recvbuf);
    return 0;

fail:
    fprintf(stderr, "Benchmark all-reduce failed at %zu bytes\n", bytes);
    free(sendbuf);
    free(recvbuf);
    return -1;
}

int main(int argc, char **argv)
{
    char **hosts = NULL;
    char *spec = NULL;
    void *handle = NULL;
    int host_count = 0;
    int rank = -1;
    int token = 0;
    int benchmark = 0;
    int int_only = 0;
    int count = 8;
    int repeat = 1;
    int iterations = -1;
    int no_pipeline = 0;
    const char *mode = "auto";
    const char *threshold = NULL;
    DATATYPE benchmark_datatype = PG_DOUBLE;
    OPERATION benchmark_operation = PG_SUM;
    int index;
    int rc;

    if (argc == 2 && strcmp(argv[1], "-help") == 0) {
        usage(argv[0]);
        return EXIT_SUCCESS;
    }

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "-myindex") == 0 && index + 1 < argc) {
            rank = atoi(argv[++index]) - 1;
        } else if (strcmp(argv[index], "-list") == 0) {
            int first = index + 1;

            while (index + 1 < argc && argv[index + 1][0] != '-') {
                ++index;
                ++host_count;
            }
            hosts = &argv[first];
        } else if (strcmp(argv[index], "-token") == 0) {
            token = 1;
            benchmark = 0;
        } else if (strcmp(argv[index], "-check") == 0 && index + 1 < argc) {
            count = atoi(argv[++index]);
            token = 0;
            benchmark = 0;
        } else if (strcmp(argv[index], "-bench") == 0) {
            benchmark = 1;
            token = 0;
        } else if (strcmp(argv[index], "-repeat") == 0 && index + 1 < argc) {
            repeat = atoi(argv[++index]);
        } else if (strcmp(argv[index], "-int-only") == 0) {
            int_only = 1;
        } else if (strcmp(argv[index], "-mode") == 0 && index + 1 < argc) {
            mode = argv[++index];
        } else if (strcmp(argv[index], "-nopipe") == 0) {
            no_pipeline = 1;
        } else if (strcmp(argv[index], "-dtype") == 0 && index + 1 < argc) {
            const char *name = argv[++index];

            if (strcmp(name, "int32") == 0) {
                benchmark_datatype = PG_INT32;
            } else if (strcmp(name, "double") == 0) {
                benchmark_datatype = PG_DOUBLE;
            } else {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "-op") == 0 && index + 1 < argc) {
            const char *name = argv[++index];

            if (strcmp(name, "sum") == 0) {
                benchmark_operation = PG_SUM;
            } else if (strcmp(name, "prod") == 0) {
                benchmark_operation = PG_PROD;
            } else {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "-iters") == 0 && index + 1 < argc) {
            iterations = atoi(argv[++index]);
        } else if (strcmp(argv[index], "-threshold") == 0 && index + 1 < argc) {
            threshold = argv[++index];
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (rank < 0 || host_count < 2 || rank >= host_count || count < 0 ||
        repeat < 1 || iterations == 0 || iterations < -1 ||
        (strcmp(mode, "auto") != 0 && strcmp(mode, "eager") != 0 &&
         strcmp(mode, "rdvz") != 0)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (setenv("PG_MODE", mode, 1) != 0) {
        fprintf(stderr, "FAIL setting protocol mode\n");
        return EXIT_FAILURE;
    }
    if (setenv("PG_NOPIPE", no_pipeline ? "1" : "0", 1) != 0) {
        fprintf(stderr, "FAIL setting pipeline mode\n");
        return EXIT_FAILURE;
    }
    if (threshold && setenv("PG_EAGER_THRESHOLD", threshold, 1) != 0) {
        fprintf(stderr, "FAIL setting eager threshold\n");
        return EXIT_FAILURE;
    }
    if (benchmark) {
        pg_log_set_level(PG_LOG_ERROR);
    }
    if (build_group_spec(rank, hosts, host_count, &spec) != 0 ||
        connect_process_group(spec, &handle) != 0) {
        fprintf(stderr, "FAIL connect_process_group\n");
        free(spec);
        return EXIT_FAILURE;
    }

    rc = token ? pg_ring_token(handle, host_count) :
         benchmark ? run_benchmark(handle, rank, benchmark_datatype,
                                   benchmark_operation, iterations) :
         check_all_reduce(handle, rank, host_count, count, repeat);
    if (rc == 0 && !token && !benchmark && !int_only) {
        rc = check_double_product_in_place(handle, rank, host_count, count,
                                           repeat);
    }
    if (rc == 0 && token) {
        fprintf(stderr, "PASS token rank=%d laps=%d\n", rank, host_count);
    }
    if (pg_close(handle) != 0) {
        rc = -1;
    }
    free(spec);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}