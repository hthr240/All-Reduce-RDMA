#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "pg_common.h"
#include "pg_topology.h"

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
