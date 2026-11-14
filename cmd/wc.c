/* wc - word, line, and byte or character count.
 * SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "xn.h"

#define BUFFER_SIZE 65536

struct counts {
    uintmax_t lines;
    uintmax_t words;
    uintmax_t bytes;
    uintmax_t chars;
};

static int count_fd(int, const char *, struct counts *);
static void report(const struct counts *, const char *);
static void usage(void);

static int show_lines, show_words, show_bytes, show_chars;

/* Count one input, decoding multibyte characters only when -m needs them. */
static int
count_fd(int fd, const char *name, struct counts *total)
{
    static char buf[BUFFER_SIZE + MB_LEN_MAX];
    mbstate_t state;
    size_t carry = 0;
    int in_word = 0;

    memset(&state, 0, sizeof(state));
    for (;;) {
        ssize_t got = read(fd, buf + carry, BUFFER_SIZE);
        size_t have, pos = 0;

        if (got == -1) {
            if (errno == EINTR)
                continue;
            xn_warn("%s", name);
            return -1;
        }
        have = carry + (size_t)got;
        carry = 0;
        total->bytes += (uintmax_t)got;
        while (pos < have) {
            wchar_t wc;
            size_t used;
            int space;

            if (show_chars) {
                used = mbrtowc(&wc, buf + pos, have - pos, &state);
                if (used == (size_t)-2 && got != 0) {
                    carry = have - pos;
                    memmove(buf, buf + pos, carry);
                    break;
                }
                if (used == (size_t)-1 || used == (size_t)-2) {
                    /* Invalid bytes count as one character each. */
                    memset(&state, 0, sizeof(state));
                    wc = L'?';
                    used = 1;
                } else if (used == 0) {
                    used = 1;
                }
                space = iswspace((wint_t)wc);
                if (wc == L'\n')
                    total->lines++;
                total->chars++;
            } else {
                unsigned char byte = (unsigned char)buf[pos];

                used = 1;
                space = isspace(byte);
                if (byte == '\n')
                    total->lines++;
            }
            if (space) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                total->words++;
            }
            pos += used;
        }
        if (got == 0)
            return 0;
    }
}

static void
report(const struct counts *counts, const char *name)
{
    const char *gap = "";

    if (show_lines) {
        (void)printf("%ju", counts->lines);
        gap = " ";
    }
    if (show_words) {
        (void)printf("%s%ju", gap, counts->words);
        gap = " ";
    }
    if (show_bytes) {
        (void)printf("%s%ju", gap, counts->bytes);
        gap = " ";
    }
    if (show_chars) {
        (void)printf("%s%ju", gap, counts->chars);
        gap = " ";
    }
    if (name != NULL)
        (void)printf("%s%s", gap, name);
    (void)putchar('\n');
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-c|-m] [-lw] [file...]\n",
                  xn_progname());
    exit(EXIT_FAILURE);
}

/* Report counts for each operand, then a total if there are several. */
int
main(int argc, char **argv)
{
    struct counts sum;
    int status = EXIT_SUCCESS;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "clmw")) != -1) {
        switch (ch) {
        case 'c':
            show_bytes = 1;
            show_chars = 0;
            break;
        case 'l':
            show_lines = 1;
            break;
        case 'm':
            show_chars = 1;
            show_bytes = 0;
            break;
        case 'w':
            show_words = 1;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (!show_lines && !show_words && !show_bytes && !show_chars)
        show_lines = show_words = show_bytes = 1;

    memset(&sum, 0, sizeof(sum));
    if (argc == 0) {
        if (count_fd(STDIN_FILENO, "standard input", &sum) == -1)
            status = EXIT_FAILURE;
        report(&sum, NULL);
        return xn_finish(status);
    }

    for (int i = 0; i < argc; i++) {
        struct counts one;
        int is_stdin = (strcmp(argv[i], "-") == 0);
        int fd = is_stdin ? STDIN_FILENO : open(argv[i], O_RDONLY);

        if (fd == -1) {
            xn_warn("%s", argv[i]);
            status = EXIT_FAILURE;
            continue;
        }
        memset(&one, 0, sizeof(one));
        if (count_fd(fd, argv[i], &one) == -1)
            status = EXIT_FAILURE;
        else
            report(&one, argv[i]);
        if (!is_stdin)
            (void)close(fd);
        sum.lines += one.lines;
        sum.words += one.words;
        sum.bytes += one.bytes;
        sum.chars += one.chars;
    }
    if (argc > 1)
        report(&sum, "total");
    return xn_finish(status);
}
