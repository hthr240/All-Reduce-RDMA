#define _POSIX_C_SOURCE 200809L

/* Configuration test: environment-driven transport and network knobs. */
#define PG_INTERNAL
#include "../pg.h"

#include <stdio.h>
#include <stdlib.h>

static int test_transport_configuration(void)
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

static int test_network_configuration(void)
{
    pg_handle_t pg = {0};

    unsetenv("PG_GID_IDX");
    unsetenv("PG_TCP_PORT");
    if (configure_transport_mode(&pg) != 0 ||
        pg.gid_index != -1 ||
        pg.bootstrap_base_port != PG_BOOTSTRAP_BASE_PORT) {
        return -1;
    }

    setenv("PG_GID_IDX", "3", 1);
    setenv("PG_TCP_PORT", "23456", 1);
    if (configure_transport_mode(&pg) != 0 ||
        pg.gid_index != 3 || pg.bootstrap_base_port != 23456) {
        return -1;
    }

    setenv("PG_GID_IDX", "256", 1);
    if (configure_transport_mode(&pg) != -1) {
        return -1;
    }
    unsetenv("PG_GID_IDX");

    setenv("PG_TCP_PORT", "80", 1);
    if (configure_transport_mode(&pg) != -1) {
        return -1;
    }
    setenv("PG_TCP_PORT", "not-a-port", 1);
    if (configure_transport_mode(&pg) != -1) {
        return -1;
    }
    unsetenv("PG_TCP_PORT");
    return 0;
}

int main(void)
{
    if (test_transport_configuration() != 0 ||
        test_network_configuration() != 0) {
        fprintf(stderr, "Configuration tests failed\n");
        return EXIT_FAILURE;
    }
    printf("Configuration tests passed\n");
    return EXIT_SUCCESS;
}