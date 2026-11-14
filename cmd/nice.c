/* nice - invoke a utility with an altered nice value.
 * SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "xn.h"

#define EXIT_UTILITY_ERROR 125
#define EXIT_CANNOT_EXEC 126
#define EXIT_NOT_FOUND 127
#define DEFAULT_INCREMENT 10

static void usage(void);

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-n increment] utility [argument...]\n",
                  xn_progname());
    exit(EXIT_UTILITY_ERROR);
}

/*
 * Run a utility at an adjusted nice value. Lacking the privilege to raise
 * priority is reported, but the utility still runs, because the nice value
 * being "unchanged" is an allowed outcome.
 */
int
main(int argc, char **argv)
{
    intmax_t increment = DEFAULT_INCREMENT;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "n:")) != -1) {
        if (ch != 'n')
            usage();
        if (xn_parse_int(xn_optarg, &increment) == -1 ||
            increment > 1000 || increment < -1000)
            xn_errx(EXIT_UTILITY_ERROR, "%s: invalid increment", xn_optarg);
    }
    argv += xn_optind;
    if (*argv == NULL)
        usage();

    errno = 0;
    if (nice((int)increment) == -1 && errno != 0)
        xn_warn("cannot change nice value");

    (void)execvp(argv[0], argv);
    xn_warn("%s", argv[0]);
    return (errno == ENOENT) ? EXIT_NOT_FOUND : EXIT_CANNOT_EXEC;
}
