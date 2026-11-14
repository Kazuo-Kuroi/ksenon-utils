/* cat - concatenate and print files. SPDX-License-Identifier: MIT */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xn.h"

static int cat_fd(int, const char *);
static void usage(void);

static int
cat_fd(int fd, const char *name)
{
    int result = xn_copyfd(fd, STDOUT_FILENO);

    if (result == XN_COPY_READ) {
        xn_warn("%s", name);
        return EXIT_FAILURE;
    }
    if (result == XN_COPY_WRITE)
        xn_err(EXIT_FAILURE, "standard output");
    return EXIT_SUCCESS;
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-u] [file...]\n", xn_progname());
    exit(EXIT_FAILURE);
}

/*
 * Copy each operand to standard output. Output is never buffered, so -u
 * needs no action.
 */
int
main(int argc, char **argv)
{
    int status = EXIT_SUCCESS;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "u")) != -1) {
        if (ch != 'u')
            usage();
    }
    argc -= xn_optind;
    argv += xn_optind;

    if (argc == 0)
        return cat_fd(STDIN_FILENO, "standard input");

    for (int i = 0; i < argc; i++) {
        int fd;

        if (strcmp(argv[i], "-") == 0) {
            if (cat_fd(STDIN_FILENO, "standard input") != EXIT_SUCCESS)
                status = EXIT_FAILURE;
            continue;
        }
        if ((fd = open(argv[i], O_RDONLY)) == -1) {
            xn_warn("%s", argv[i]);
            status = EXIT_FAILURE;
            continue;
        }
        if (cat_fd(fd, argv[i]) != EXIT_SUCCESS)
            status = EXIT_FAILURE;
        (void)close(fd);
    }
    return status;
}
