#ifndef PG_CLI_H
#define PG_CLI_H

/*
 * usage:
 *  Print the command-line usage for the process-group executable.
 *  The expected invocation is either:
 *      <prog> -myindex <rank> -list <host1> [host2 ...]
 *  or a single host name in the minimal standalone form.
 */
void usage(const char *prog);

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
int parse_rank_and_hosts(int argc, char **argv, int *myindex, char ***host_list, int *host_count);

#endif /* PG_CLI_H */
