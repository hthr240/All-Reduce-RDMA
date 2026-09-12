#define _POSIX_C_SOURCE 200809L

/* Phase 9: benchmark transport configuration. */
#define main ring_allreduce_program_main
#include "../ring_allreduce.c"
#undef main

#include <stdio.h>
#include <stdlib.h>

static int test_benchmark_transport_configuration(void)
{
    pg_handle_t pg = {0};

    unsetenv("PG_MODE");
    unsetenv("PG_NOPIPE");
    unsetenv("PG_EAGER_THRESHOLD");
    if (configure_transport_mode(&pg) != 0 ||
        pg.transport_mode != PG_TRANSPORT_AUTO ||
        pg.eager_threshold != PG_EAGER_THRESHOLD ||
        !pg.pipeline_enabled) {
        return -1;
    }

    setenv("PG_MODE", "rdvz", 1);
    setenv("PG_NOPIPE", "1", 1);
    setenv("PG_EAGER_THRESHOLD", "32768", 1);
    if (configure_transport_mode(&pg) != 0 ||
        pg.transport_mode != PG_TRANSPORT_RDVZ ||
        pg.eager_threshold != 32768 || pg.pipeline_enabled) {
        return -1;
    }

    setenv("PG_EAGER_THRESHOLD", "12KB", 1);
    if (configure_transport_mode(&pg) != -1) {
        return -1;
    }
    setenv("PG_EAGER_THRESHOLD", "-1", 1);
    if (configure_transport_mode(&pg) != -1) {
        return -1;
    }

    unsetenv("PG_MODE");
    unsetenv("PG_NOPIPE");
    unsetenv("PG_EAGER_THRESHOLD");
    return 0;
}

int main(void)
{
    if (test_benchmark_transport_configuration() != 0) {
        fprintf(stderr, "Phase 9 benchmark configuration tests failed\n");
        return EXIT_FAILURE;
    }
    printf("Phase 9 benchmark configuration tests passed\n");
    return EXIT_SUCCESS;
}