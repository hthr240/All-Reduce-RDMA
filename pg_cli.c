#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pg_cli.h"

void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -myindex <rank> -list <host1> [host2 ...]\n"
            "       %s <hostname>\n",
            prog, prog);
}

int parse_rank_and_hosts(int argc, char **argv, int *myindex, char ***host_list, int *host_count)
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
