#define _POSIX_C_SOURCE 200809L

/*
 * Phase 2 test:
 *  Verify the local logical-ring topology calculation without requiring
 *  network peers or an RDMA device.
 */
#define main ring_allreduce_program_main
#include "../ring_allreduce.c"
#undef main

#include <stdio.h>
#include <stdlib.h>

static int test_ring_neighbors(void)
{
    pg_handle_t pg = {0};
    const int expected_previous[] = {3, 0, 1, 2};
    const int expected_next[] = {1, 2, 3, 0};
    int rank;

    for (rank = 0; rank < 4; ++rank) {
        if (configure_process_group_topology(&pg, rank, 4) != 0 ||
            pg.rank != rank || pg.size != 4 ||
            pg.previous_rank != expected_previous[rank] ||
            pg.next_rank != expected_next[rank]) {
            fprintf(stderr, "incorrect ring neighbors for rank %d\n", rank);
            return -1;
        }
    }

    if (configure_process_group_topology(&pg, 0, 1) != 0 ||
        pg.previous_rank != 0 || pg.next_rank != 0) {
        fprintf(stderr, "incorrect single-rank ring topology\n");
        return -1;
    }

    return 0;
}

static int test_invalid_topology(void)
{
    pg_handle_t pg = {0};

    if (configure_process_group_topology(NULL, 0, 1) != -1 ||
        configure_process_group_topology(&pg, -1, 1) != -1 ||
        configure_process_group_topology(&pg, 1, 1) != -1 ||
        configure_process_group_topology(&pg, 0, 0) != -1) {
        fprintf(stderr, "invalid topology was accepted\n");
        return -1;
    }

    return 0;
}

static int test_host_list_validation(void)
{
    char *valid_hosts[] = {"mlxstud01", "mlxstud02", "mlxstud03", NULL};
    char *duplicate_hosts[] = {"mlxstud01", "mlxstud01", NULL};
    char *empty_host[] = {"", NULL};

    if (validate_host_list(valid_hosts, 3) != 0 ||
        validate_host_list(duplicate_hosts, 2) != -1 ||
        validate_host_list(empty_host, 1) != -1 ||
        validate_host_list(NULL, 0) != -1) {
        fprintf(stderr, "host-list validation returned an unexpected result\n");
        return -1;
    }

    return 0;
}

static int test_course_rank_numbering(void)
{
    char *arguments[] = {
        "test", "-myindex", "01", "-list", "mlx-stud-01", "mlx-stud-02"
    };
    char **hosts = NULL;
    int rank = -1;
    int host_count = 0;
    int index;

    if (parse_rank_and_hosts(6, arguments, &rank, &hosts, &host_count) != 0 ||
        rank != 0 || host_count != 2 || strcmp(hosts[rank], "mlx-stud-01") != 0) {
        fprintf(stderr, "course one-based rank numbering is incorrect\n");
        for (index = 0; hosts && index < host_count; ++index) {
            free(hosts[index]);
        }
        free(hosts);
        return -1;
    }
    for (index = 0; index < host_count; ++index) {
        free(hosts[index]);
    }
    free(hosts);
    return 0;
}

static int test_process_group_spec(void)
{
    char **hosts = NULL;
    int rank = -1;
    int host_count = 0;

    if (parse_process_group_spec("01:mlx-stud-01,mlx-stud-02", &rank,
                                 &hosts, &host_count) != 0 ||
        rank != 0 || host_count != 2 ||
        strcmp(hosts[0], "mlx-stud-01") != 0 ||
        strcmp(hosts[1], "mlx-stud-02") != 0) {
        fprintf(stderr, "process-group specification parsing is incorrect\n");
        free_process_group_hosts(hosts, host_count);
        return -1;
    }
    free_process_group_hosts(hosts, host_count);

    if (parse_process_group_spec("00:mlx-stud-01,mlx-stud-02", &rank,
                                 &hosts, &host_count) != -1 ||
        parse_process_group_spec("03:mlx-stud-01,mlx-stud-02", &rank,
                                 &hosts, &host_count) != -1 ||
        parse_process_group_spec("01:mlx-stud-01,,mlx-stud-02", &rank,
                                 &hosts, &host_count) != -1 ||
        parse_process_group_spec("01:mlx-stud-01,", &rank,
                                 &hosts, &host_count) != -1) {
        fprintf(stderr, "invalid process-group specification was accepted\n");
        return -1;
    }
    return 0;
}

int main(void)
{
    if (test_ring_neighbors() != 0 || test_invalid_topology() != 0 ||
        test_host_list_validation() != 0 || test_course_rank_numbering() != 0 ||
        test_process_group_spec() != 0) {
        return EXIT_FAILURE;
    }

    printf("Phase 2 topology tests passed\n");
    return EXIT_SUCCESS;
}
