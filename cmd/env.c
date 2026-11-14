/* env - set the environment for command invocation.
 * SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xn.h"

#define EXIT_UTILITY_ERROR 125
#define EXIT_CANNOT_EXEC 126
#define EXIT_NOT_FOUND 127

static void usage(void);

extern char **environ;

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-i] [name=value]... "
                  "[utility [argument...]]\n", xn_progname());
    exit(EXIT_UTILITY_ERROR);
}

/* Run a utility with a modified environment, or print the environment. */
int
main(int argc, char **argv)
{
    static char *empty[] = { NULL };
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "i")) != -1) {
        if (ch != 'i')
            usage();
        environ = empty;
    }
    argv += xn_optind;

    /* A lone "-" is the obsolescent spelling of -i. */
    if (*argv != NULL && strcmp(*argv, "-") == 0) {
        environ = empty;
        argv++;
    }

    for (; *argv != NULL && strchr(*argv, '=') != NULL; argv++) {
        char *equals = strchr(*argv, '=');

        *equals = '\0';
        if (setenv(*argv, equals + 1, 1) == -1)
            xn_err(EXIT_UTILITY_ERROR, "%s", *argv);
        *equals = '=';
    }

    if (*argv == NULL) {
        for (char **entry = environ; *entry != NULL; entry++)
            (void)puts(*entry);
        return xn_finish(EXIT_SUCCESS);
    }

    (void)execvp(argv[0], argv);
    xn_warn("%s", argv[0]);
    return (errno == ENOENT) ? EXIT_NOT_FOUND : EXIT_CANNOT_EXEC;
}
