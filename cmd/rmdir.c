/* rmdir - remove directories. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xn.h"

static int remove_chain(char *, int);
static void usage(void);

/* Remove path and, with -p, each parent named in it. */
static int
remove_chain(char *path, int parents)
{
    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';
    for (;;) {
        char *slash;

        if (rmdir(path) == -1) {
            xn_warn("%s", path);
            return -1;
        }
        if (!parents || (slash = strrchr(path, '/')) == NULL)
            return 0;
        while (slash > path && slash[-1] == '/')
            slash--;
        if (slash == path)
            return 0;
        *slash = '\0';
    }
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-p] dir...\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Remove each empty directory operand. */
int
main(int argc, char **argv)
{
    int parents = 0;
    int status = EXIT_SUCCESS;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "p")) != -1) {
        if (ch != 'p')
            usage();
        parents = 1;
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc == 0)
        usage();

    for (int i = 0; i < argc; i++) {
        if (remove_chain(argv[i], parents) == -1)
            status = EXIT_FAILURE;
    }
    return status;
}
