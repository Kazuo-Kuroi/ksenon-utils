/* tty - return the user's terminal name. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "xn.h"

#define EXIT_NOT_TTY 1
#define EXIT_ERROR 2

static void usage(void);

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-s]\n", xn_progname());
    exit(EXIT_ERROR);
}

/* Print the name of the terminal on standard input. -s only sets status. */
int
main(int argc, char **argv)
{
    const char *name;
    int quiet = 0;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "s")) != -1) {
        if (ch != 's')
            usage();
        quiet = 1;
    }
    if (xn_optind != argc)
        usage();

    if (quiet)
        return isatty(STDIN_FILENO) ? EXIT_SUCCESS : EXIT_NOT_TTY;
    if ((name = ttyname(STDIN_FILENO)) == NULL) {
        (void)puts("not a tty");
        return xn_finish(EXIT_NOT_TTY);
    }
    (void)puts(name);
    return xn_finish(EXIT_SUCCESS);
}
