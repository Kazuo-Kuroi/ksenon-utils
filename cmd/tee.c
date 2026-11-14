/* tee - duplicate standard input. SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "xn.h"

#define BUFFER_SIZE 65536

static void usage(void);

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-ai] [file...]\n", xn_progname());
    exit(EXIT_FAILURE);
}

/*
 * Copy standard input to standard output and to each operand. A failing
 * output is reported once and dropped; the others keep receiving data.
 */
int
main(int argc, char **argv)
{
    static char buf[BUFFER_SIZE];
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    int status = EXIT_SUCCESS;
    int *fds;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "ai")) != -1) {
        switch (ch) {
        case 'a':
            flags = O_WRONLY | O_CREAT | O_APPEND;
            break;
        case 'i':
            (void)signal(SIGINT, SIG_IGN);
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;

    fds = xn_malloc(((size_t)argc + 1) * sizeof(*fds));
    fds[0] = STDOUT_FILENO;
    for (int i = 0; i < argc; i++) {
        if ((fds[i + 1] = open(argv[i], flags, 0666)) == -1) {
            xn_warn("%s", argv[i]);
            status = EXIT_FAILURE;
        }
    }

    for (;;) {
        ssize_t got = read(STDIN_FILENO, buf, sizeof(buf));

        if (got == 0)
            break;
        if (got == -1) {
            if (errno == EINTR)
                continue;
            xn_warn("standard input");
            status = EXIT_FAILURE;
            break;
        }
        for (int i = 0; i <= argc; i++) {
            if (fds[i] != -1 &&
                xn_writeall(fds[i], buf, (size_t)got) == -1) {
                xn_warn("%s", (i == 0) ? "standard output" : argv[i - 1]);
                status = EXIT_FAILURE;
                fds[i] = -1;
            }
        }
    }
    for (int i = 1; i <= argc; i++) {
        if (fds[i] != -1 && close(fds[i]) == -1) {
            xn_warn("%s", argv[i - 1]);
            status = EXIT_FAILURE;
        }
    }
    free(fds);
    return status;
}
