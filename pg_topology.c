#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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
