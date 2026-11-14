/* head - copy the first part of files. SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xn.h"

#define BUFFER_SIZE 65536

static int head_fd(int, const char *, uintmax_t);
static void usage(void);

/*
 * Copy up to count lines. On a seekable file the offset is left just past
 * the last byte used, as POSIX requires when input is not fully consumed.
 */
static int
head_fd(int fd, const char *name, uintmax_t count)
{
    static char buf[BUFFER_SIZE];

    while (count > 0) {
        ssize_t got = read(fd, buf, sizeof(buf));
        size_t used = 0;

        if (got == 0)
            return EXIT_SUCCESS;
        if (got == -1) {
            if (errno == EINTR)
                continue;
            xn_warn("%s", name);
            return EXIT_FAILURE;
        }
        while (used < (size_t)got && count > 0) {
            if (buf[used++] == '\n')
                count--;
        }
        if (fwrite(buf, 1, used, stdout) != used)
            xn_err(EXIT_FAILURE, "standard output");
        if (used < (size_t)got)
            (void)lseek(fd, (off_t)used - got, SEEK_CUR);
    }
    return EXIT_SUCCESS;
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-n number] [file...]\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Write the first lines of each operand, ten unless -n says otherwise. */
int
main(int argc, char **argv)
{
    uintmax_t count = 10;
    int status = EXIT_SUCCESS;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "n:")) != -1) {
        if (ch != 'n')
            usage();
        if (xn_parse_uint(xn_optarg, &count) == -1)
            xn_errx(EXIT_FAILURE, "%s: invalid line count", xn_optarg);
    }
    argc -= xn_optind;
    argv += xn_optind;

    if (argc == 0)
        return xn_finish(head_fd(STDIN_FILENO, "standard input", count));

    for (int i = 0; i < argc; i++) {
        int is_stdin = (strcmp(argv[i], "-") == 0);
        int fd = is_stdin ? STDIN_FILENO : open(argv[i], O_RDONLY);

        if (fd == -1) {
            xn_warn("%s", argv[i]);
            status = EXIT_FAILURE;
            continue;
        }
        if (argc > 1)
            (void)printf("%s==> %s <==\n", (i > 0) ? "\n" : "", argv[i]);
        if (head_fd(fd, argv[i], count) != EXIT_SUCCESS)
            status = EXIT_FAILURE;
        if (!is_stdin)
            (void)close(fd);
    }
    return xn_finish(status);
}
