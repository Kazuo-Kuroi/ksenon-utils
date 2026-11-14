/* sleep - suspend execution for an interval. SPDX-License-Identifier: MIT */
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xn.h"

static void on_alarm(int);
static void usage(void);

/* POSIX lets SIGALRM end sleep normally, with a zero exit status. */
static void
on_alarm(int sig)
{
    (void)sig;
    _exit(EXIT_SUCCESS);
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s time\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Wait for the given number of seconds. */
int
main(int argc, char **argv)
{
    struct sigaction action;
    uintmax_t left;

    xn_init(argv);
    while (xn_getopt(argc, argv, "") != -1)
        usage();
    if (argc - xn_optind != 1)
        usage();
    if (xn_parse_uint(argv[xn_optind], &left) == -1)
        xn_errx(EXIT_FAILURE, "%s: invalid time", argv[xn_optind]);

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_alarm;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGALRM, &action, NULL);

    while (left > 0) {
        unsigned int chunk = (left > UINT_MAX) ? UINT_MAX : (unsigned int)left;

        left -= chunk - sleep(chunk);
    }
    return EXIT_SUCCESS;
}
