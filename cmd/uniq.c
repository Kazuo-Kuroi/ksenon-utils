/* uniq - report or filter out repeated lines.
 * SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xn.h"

enum uniq_mode {
    MODE_ALL,
    MODE_REPEATED,
    MODE_UNIQUE
};

static const char *compare_start(const char *, size_t);
static void put_group(const char *, size_t, uintmax_t);
static void usage(void);

static enum uniq_mode mode = MODE_ALL;
static int show_count;
static uintmax_t skip_fields, skip_chars;
static FILE *output;

/* Locate where comparison begins after skipping fields and characters. */
static const char *
compare_start(const char *line, size_t len)
{
    const char *ptr = line;
    const char *end = line + len;

    for (uintmax_t i = 0; i < skip_fields && ptr < end; i++) {
        while (ptr < end && isblank((unsigned char)*ptr))
            ptr++;
        while (ptr < end && !isblank((unsigned char)*ptr))
            ptr++;
    }
    if (skip_chars > (uintmax_t)(end - ptr))
        return end;
    return ptr + skip_chars;
}

static void
put_group(const char *line, size_t len, uintmax_t count)
{
    if ((mode == MODE_REPEATED && count < 2) ||
        (mode == MODE_UNIQUE && count != 1))
        return;
    if (show_count)
        (void)fprintf(output, "%ju ", count);
    (void)fwrite(line, 1, len, output);
    (void)putc('\n', output);
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-c|-d|-u] [-f fields] [-s chars] "
                  "[input_file [output_file]]\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Collapse adjacent duplicate lines. */
int
main(int argc, char **argv)
{
    FILE *input = stdin;
    char *cur = NULL, *prev = NULL;
    size_t cur_cap = 0, prev_cap = 0, prev_len = 0;
    uintmax_t count = 0;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "cdf:s:u")) != -1) {
        switch (ch) {
        case 'c':
            show_count = 1;
            break;
        case 'd':
            mode = MODE_REPEATED;
            break;
        case 'f':
            if (xn_parse_uint(xn_optarg, &skip_fields) == -1)
                xn_errx(EXIT_FAILURE, "%s: invalid field count", xn_optarg);
            break;
        case 's':
            if (xn_parse_uint(xn_optarg, &skip_chars) == -1)
                xn_errx(EXIT_FAILURE, "%s: invalid character count",
                        xn_optarg);
            break;
        case 'u':
            mode = MODE_UNIQUE;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc > 2)
        usage();

    output = stdout;
    if (argc >= 1 && strcmp(argv[0], "-") != 0 &&
        (input = fopen(argv[0], "r")) == NULL)
        xn_err(EXIT_FAILURE, "%s", argv[0]);
    if (argc == 2 && strcmp(argv[1], "-") != 0 &&
        (output = fopen(argv[1], "w")) == NULL)
        xn_err(EXIT_FAILURE, "%s", argv[1]);

    for (;;) {
        ssize_t len = xn_getdelim(&cur, &cur_cap, '\n', input);
        const char *left, *right;
        size_t left_len, right_len;
        char *swap;
        size_t swap_cap;

        if (len == -1)
            break;
        if (len > 0 && cur[len - 1] == '\n')
            cur[--len] = '\0';

        if (count > 0) {
            left = compare_start(prev, prev_len);
            right = compare_start(cur, (size_t)len);
            left_len = prev_len - (size_t)(left - prev);
            right_len = (size_t)len - (size_t)(right - cur);
            if (left_len == right_len &&
                memcmp(left, right, left_len) == 0) {
                count++;
                continue;
            }
            put_group(prev, prev_len, count);
        }
        swap = prev;
        prev = cur;
        cur = swap;
        swap_cap = prev_cap;
        prev_cap = cur_cap;
        cur_cap = swap_cap;
        prev_len = (size_t)len;
        count = 1;
    }
    if (ferror(input))
        xn_err(EXIT_FAILURE, "%s", (argc >= 1) ? argv[0] : "standard input");
    if (count > 0)
        put_group(prev, prev_len, count);
    free(cur);
    free(prev);

    if (fflush(output) == EOF || ferror(output))
        xn_err(EXIT_FAILURE, "output");
    return EXIT_SUCCESS;
}
