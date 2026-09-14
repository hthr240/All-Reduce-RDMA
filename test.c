#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pg.h"

#define BENCHMARK_MAX_BYTES (4u << 20)

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s -myindex <one-based-rank> -list <host1> <host2> [host...] [-token | -suite | -check <count> | -bench] [-repeat <count>] [-int-only] [-mode auto|eager|rdvz] [-nopipe] [-dtype int32|double] [-op sum|prod] [-iters <count>] [-threshold <bytes>]\n",
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

static const char *datatype_name(DATATYPE datatype)
{
    return datatype == PG_INT32 ? "int32" : "double";
}

static const char *operation_name(OPERATION operation)
{
    return operation == PG_SUM ? "sum" : "prod";
}

static int32_t suite_int_value(OPERATION operation, int rank, int index)
{
    if (operation == PG_PROD) {
        return 1 + ((rank + index) & 1);
    }
    return (int32_t)((rank + 1) * 1000 + index % 997);
}

static double suite_double_value(OPERATION operation, int rank, int index)
{
    if (operation == PG_PROD) {
        return 1.0 + 0.25 * (double)((rank + index) % 3);
    }
    return (double)(rank + 1) + (double)(index % 1000) * 0.001;
}

static int verify_all_reduce_case(void *handle, int rank, int nranks,
                                  int count, DATATYPE datatype,
                                  OPERATION operation, int in_place,
                                  int quiet)
{
    size_t element_size = datatype == PG_INT32 ? sizeof(int32_t) : sizeof(double);
    size_t bytes = (size_t)count * element_size;
    void *sendbuf = malloc(bytes ? bytes : 1);
    void *recvbuf = malloc(bytes ? bytes : 1);
    int failures = 0;
    int index;
    int source_rank;

    if (!sendbuf || !recvbuf) {
        free(sendbuf);
        free(recvbuf);
        return -1;
    }
    for (index = 0; index < count; ++index) {
        if (datatype == PG_INT32) {
            int32_t value = suite_int_value(operation, rank, index);

            ((int32_t *)sendbuf)[index] = value;
            ((int32_t *)recvbuf)[index] = value;
        } else {
            double value = suite_double_value(operation, rank, index);

            ((double *)sendbuf)[index] = value;
            ((double *)recvbuf)[index] = value;
        }
    }
    if (pg_all_reduce(in_place ? recvbuf : sendbuf, recvbuf, count,
                      datatype, operation, handle) != 0) {
        fprintf(stderr, "FAIL all_reduce count=%d %s %s%s: call failed\n",
                count, datatype_name(datatype), operation_name(operation),
                in_place ? " in-place" : "");
        free(sendbuf);
        free(recvbuf);
        return -1;
    }

    for (index = 0; index < count && failures < 5; ++index) {
        if (datatype == PG_INT32) {
            int32_t expected = suite_int_value(operation, 0, index);
            int32_t actual = ((int32_t *)recvbuf)[index];

            for (source_rank = 1; source_rank < nranks; ++source_rank) {
                int32_t value = suite_int_value(operation, source_rank, index);

                expected = operation == PG_SUM ? expected + value : expected * value;
            }
            if (actual != expected) {
                fprintf(stderr,
                        "FAIL all_reduce rank=%d index=%d got=%d expected=%d\n",
                        rank, index, actual, expected);
                ++failures;
            }
        } else {
            double expected = suite_double_value(operation, 0, index);
            double actual = ((double *)recvbuf)[index];

            for (source_rank = 1; source_rank < nranks; ++source_rank) {
                double value = suite_double_value(operation, source_rank, index);

                expected = operation == PG_SUM ? expected + value : expected * value;
            }
            if (fabs(actual - expected) > 1e-9 * fabs(expected) + 1e-12) {
                fprintf(stderr,
                        "FAIL all_reduce rank=%d index=%d got=%.17g expected=%.17g\n",
                        rank, index, actual, expected);
                ++failures;
            }
        }
    }
    if (!quiet) {
        fprintf(stderr, "%s all_reduce count=%-8d %s %s%s\n",
                failures ? "FAIL" : "PASS", count,
                datatype_name(datatype), operation_name(operation),
                in_place ? " in-place" : "");
    }
    free(sendbuf);
    free(recvbuf);
    return failures ? -1 : 0;
}

static int verify_reduce_scatter(void *handle, int rank, int nranks, int count)
{
    int offset;
    int nelem;
    int32_t *sendbuf;
    int32_t *recvbuf;
    int failures = 0;
    int index;
    int source_rank;

    if (pg_chunk(handle, count, &offset, &nelem) != 0) {
        return -1;
    }
    sendbuf = malloc((size_t)(count > 0 ? count : 1) * sizeof(*sendbuf));
    recvbuf = malloc((size_t)(nelem > 0 ? nelem : 1) * sizeof(*recvbuf));
    if (!sendbuf || !recvbuf) {
        free(sendbuf);
        free(recvbuf);
        return -1;
    }
    for (index = 0; index < count; ++index) {
        sendbuf[index] = suite_int_value(PG_SUM, rank, index);
    }
    if (pg_reduce_scatter(sendbuf, recvbuf, count, PG_INT32, PG_SUM,
                          handle) != 0) {
        failures = 1;
    }
    for (index = 0; !failures && index < nelem; ++index) {
        int32_t expected = 0;

        for (source_rank = 0; source_rank < nranks; ++source_rank) {
            expected += suite_int_value(PG_SUM, source_rank, offset + index);
        }
        if (recvbuf[index] != expected) {
            failures = 1;
        }
    }
    fprintf(stderr, "%s reduce_scatter count=%d offset=%d nelem=%d\n",
            failures ? "FAIL" : "PASS", count, offset, nelem);
    free(sendbuf);
    free(recvbuf);
    return failures ? -1 : 0;
}

static int verify_all_gather(void *handle, int count)
{
    int offset;
    int nelem;
    int32_t *sendbuf;
    int32_t *recvbuf;
    int failures = 0;
    int index;

    if (pg_chunk(handle, count, &offset, &nelem) != 0) {
        return -1;
    }
    sendbuf = malloc((size_t)(nelem > 0 ? nelem : 1) * sizeof(*sendbuf));
    recvbuf = malloc((size_t)(count > 0 ? count : 1) * sizeof(*recvbuf));
    if (!sendbuf || !recvbuf) {
        free(sendbuf);
        free(recvbuf);
        return -1;
    }
    for (index = 0; index < nelem; ++index) {
        sendbuf[index] = 3 * (offset + index) + 1;
    }
    if (pg_all_gather(sendbuf, recvbuf, count, PG_INT32, handle) != 0) {
        failures = 1;
    }
    for (index = 0; !failures && index < count; ++index) {
        if (recvbuf[index] != 3 * index + 1) {
            failures = 1;
        }
    }
    fprintf(stderr, "%s all_gather count=%d\n",
            failures ? "FAIL" : "PASS", count);
    free(sendbuf);
    free(recvbuf);
    return failures ? -1 : 0;
}

static int run_correctness_suite(void *handle, int rank, int nranks)
{
    int int_counts[] = {
        0, 1, nranks - 1, nranks, nranks + 1,
        nranks * 1024 - 1, nranks * 1024, nranks * 1024 + 1,
        nranks * 4096 - 1, nranks * 4096 + 1,
        nranks * 32768 - 1, nranks * 32768 + 1,
        1000003, (int)(BENCHMARK_MAX_BYTES / sizeof(int32_t))
    };
    int double_counts[] = {
        0, 1, nranks - 1, nranks, nranks + 1,
        nranks * 512 - 1, nranks * 512 + 1,
        nranks * 2048 - 1, nranks * 2048 + 1,
        nranks * 16384 - 1, nranks * 16384 + 1,
        500009, (int)(BENCHMARK_MAX_BYTES / sizeof(double))
    };
    OPERATION operations[] = {PG_SUM, PG_PROD};
    int failures = 0;
    size_t count_index;
    size_t operation_index;
    int repetition;

    for (count_index = 0;
         count_index < sizeof(int_counts) / sizeof(int_counts[0]);
         ++count_index) {
        for (operation_index = 0;
             operation_index < sizeof(operations) / sizeof(operations[0]);
             ++operation_index) {
            failures += verify_all_reduce_case(
                handle, rank, nranks, int_counts[count_index], PG_INT32,
                operations[operation_index],
                (int)((count_index + operation_index) & 1u), 0) != 0;
        }
    }
    for (count_index = 0;
         count_index < sizeof(double_counts) / sizeof(double_counts[0]);
         ++count_index) {
        for (operation_index = 0;
             operation_index < sizeof(operations) / sizeof(operations[0]);
             ++operation_index) {
            failures += verify_all_reduce_case(
                handle, rank, nranks, double_counts[count_index], PG_DOUBLE,
                operations[operation_index],
                (int)((count_index + operation_index) & 1u), 0) != 0;
        }
    }
    failures += verify_reduce_scatter(handle, rank, nranks,
                                      7 * nranks + 3) != 0;
    failures += verify_all_gather(handle, 7 * nranks + 3) != 0;
    failures += verify_reduce_scatter(handle, rank, nranks, 100000) != 0;
    failures += verify_all_gather(handle, 100000) != 0;

    for (repetition = 0; repetition < 50 && failures == 0; ++repetition) {
        failures += verify_all_reduce_case(handle, rank, nranks,
                                           nranks * 4096 + 1, PG_INT32,
                                           PG_SUM, repetition & 1, 1) != 0;
    }
    fprintf(stderr, "suite: %s (%d failure%s)\n",
            failures ? "FAILED" : "all passed", failures,
            failures == 1 ? "" : "s");
    return failures ? -1 : 0;
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

    if (rank == 0) {
        printf("# bytes\tlatency_usec\tbandwidth_MBps\n");
        fflush(stdout);
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
            /* bytes per microsecond is exactly decimal megabytes per second */
            printf("%zu\t%.3f\t%.3f\n", bytes, latency_us,
                   (double)bytes / latency_us);
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
    enum action {
        ACTION_CHECK,
        ACTION_TOKEN,
        ACTION_SUITE,
        ACTION_BENCHMARK
    } action = ACTION_CHECK;
    char **hosts = NULL;
    char *spec = NULL;
    void *handle = NULL;
    int host_count = 0;
    int rank = -1;
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
            action = ACTION_TOKEN;
        } else if (strcmp(argv[index], "-suite") == 0) {
            action = ACTION_SUITE;
        } else if (strcmp(argv[index], "-check") == 0 && index + 1 < argc) {
            count = atoi(argv[++index]);
            action = ACTION_CHECK;
        } else if (strcmp(argv[index], "-bench") == 0) {
            action = ACTION_BENCHMARK;
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
    if (action == ACTION_BENCHMARK) {
        if (setenv("PG_LOG_LEVEL", "error", 1) != 0) {
            fprintf(stderr, "FAIL setting benchmark log level\n");
            return EXIT_FAILURE;
        }
    }
    if (build_group_spec(rank, hosts, host_count, &spec) != 0 ||
        connect_process_group(spec, &handle) != 0) {
        fprintf(stderr, "FAIL connect_process_group\n");
        free(spec);
        return EXIT_FAILURE;
    }

    if (action == ACTION_TOKEN) {
        rc = pg_ring_token(handle, host_count);
    } else if (action == ACTION_SUITE) {
        rc = run_correctness_suite(handle, rank, host_count);
    } else if (action == ACTION_BENCHMARK) {
        rc = run_benchmark(handle, rank, benchmark_datatype,
                           benchmark_operation, iterations);
    } else {
        rc = check_all_reduce(handle, rank, host_count, count, repeat);
    }
    if (rc == 0 && action == ACTION_CHECK && !int_only) {
        rc = check_double_product_in_place(handle, rank, host_count, count,
                                           repeat);
    }
    if (rc == 0 && action == ACTION_TOKEN) {
        fprintf(stderr, "PASS token rank=%d laps=%d\n", rank, host_count);
    }
    if (pg_close(handle) != 0) {
        rc = -1;
    }
    free(spec);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}