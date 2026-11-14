/* cut - cut out selected fields of each line of a file.
 * SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xn.h"

struct range {
    uintmax_t low;
    uintmax_t high;
};

enum cut_mode {
    CUT_NONE,
    CUT_BYTES,
    CUT_CHARS,
    CUT_FIELDS
};

static int compare_ranges(const void *, const void *);
static void cut_bytes(const char *, size_t);
static void cut_chars(const char *, size_t);
static void cut_fields(const char *, size_t);
static int cut_stream(FILE *);
static void parse_list(char *);
static int selected(uintmax_t, size_t *);
static void usage(void);

static struct range *ranges;
static size_t nranges;
static enum cut_mode mode = CUT_NONE;
static int delim = '\t';
static int suppress;

static int
compare_ranges(const void *left, const void *right)
{
    const struct range *a = left;
    const struct range *b = right;

    if (a->low != b->low)
        return (a->low < b->low) ? -1 : 1;
    return 0;
}

/* Parse "a-b,c,d-" into a sorted, merged list of ranges. */
static void
parse_list(char *list)
{
    size_t merged = 0;
    char *save = list;

    for (char *part = strtok(save, ", "); part != NULL;
         part = strtok(NULL, ", ")) {
        struct range item;
        char *dash = strchr(part, '-');

        item.low = 1;
        item.high = UINTMAX_MAX;
        if (dash != NULL) {
            *dash = '\0';
            if (*part != '\0' && xn_parse_uint(part, &item.low) == -1)
                xn_errx(EXIT_FAILURE, "%s: invalid list value", part);
            if (dash[1] != '\0' &&
                xn_parse_uint(dash + 1, &item.high) == -1)
                xn_errx(EXIT_FAILURE, "%s: invalid list value", dash + 1);
            if (*part == '\0' && dash[1] == '\0')
                xn_errx(EXIT_FAILURE, "invalid range in list");
        } else {
            if (xn_parse_uint(part, &item.low) == -1)
                xn_errx(EXIT_FAILURE, "%s: invalid list value", part);
            item.high = item.low;
        }
        if (item.low == 0 || item.low > item.high)
            xn_errx(EXIT_FAILURE, "invalid range in list");
        ranges = xn_realloc(ranges, (nranges + 1) * sizeof(*ranges));
        ranges[nranges++] = item;
    }
    if (nranges == 0)
        xn_errx(EXIT_FAILURE, "empty list");

    qsort(ranges, nranges, sizeof(*ranges), compare_ranges);
    for (size_t i = 1; i < nranges; i++) {
        if (ranges[i].low <= ranges[merged].high ||
            ranges[merged].high + 1 == ranges[i].low) {
            if (ranges[i].high > ranges[merged].high)
                ranges[merged].high = ranges[i].high;
        } else {
            ranges[++merged] = ranges[i];
        }
    }
    nranges = merged + 1;
}

/* Test a position; positions must be presented in increasing order. */
static int
selected(uintmax_t pos, size_t *cursor)
{
    while (*cursor < nranges && ranges[*cursor].high < pos)
        (*cursor)++;
    return *cursor < nranges && ranges[*cursor].low <= pos;
}

static void
cut_bytes(const char *line, size_t len)
{
    size_t cursor = 0;

    for (size_t i = 0; i < len; i++) {
        if (selected((uintmax_t)i + 1, &cursor))
            (void)putchar((unsigned char)line[i]);
    }
    (void)putchar('\n');
}

static void
cut_chars(const char *line, size_t len)
{
    size_t cursor = 0;
    size_t pos = 0;
    uintmax_t index = 1;

    (void)mblen(NULL, 0);
    while (pos < len) {
        int width = mblen(line + pos, len - pos);

        if (width < 1)
            width = 1;
        if (selected(index, &cursor))
            (void)fwrite(line + pos, 1, (size_t)width, stdout);
        pos += (size_t)width;
        index++;
    }
    (void)putchar('\n');
}

static void
cut_fields(const char *line, size_t len)
{
    const char *ptr = line;
    const char *end = line + len;
    size_t cursor = 0;
    uintmax_t field = 1;
    int wrote = 0;

    if (memchr(line, delim, len) == NULL) {
        if (!suppress) {
            (void)fwrite(line, 1, len, stdout);
            (void)putchar('\n');
        }
        return;
    }
    for (;;) {
        const char *stop = memchr(ptr, delim, (size_t)(end - ptr));

        if (stop == NULL)
            stop = end;
        if (selected(field, &cursor)) {
            if (wrote)
                (void)putchar(delim);
            (void)fwrite(ptr, 1, (size_t)(stop - ptr), stdout);
            wrote = 1;
        }
        if (stop == end)
            break;
        ptr = stop + 1;
        field++;
    }
    (void)putchar('\n');
}

static int
cut_stream(FILE *fp)
{
    static char *line;
    static size_t cap;
    ssize_t len;

    while ((len = xn_getdelim(&line, &cap, '\n', fp)) != -1) {
        if (line[len - 1] == '\n')
            len--;
        if (mode == CUT_BYTES)
            cut_bytes(line, (size_t)len);
        else if (mode == CUT_CHARS)
            cut_chars(line, (size_t)len);
        else
            cut_fields(line, (size_t)len);
    }
    return ferror(fp) ? -1 : 0;
}

static void
usage(void)
{
    (void)fprintf(stderr,
                  "usage: %s -b list [-n] [file...]\n"
                  "       %s -c list [file...]\n"
                  "       %s -f list [-d delim] [-s] [file...]\n",
                  xn_progname(), xn_progname(), xn_progname());
    exit(EXIT_FAILURE);
}

/* Write the selected parts of each line. */
int
main(int argc, char **argv)
{
    int status = EXIT_SUCCESS;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "b:c:d:f:ns")) != -1) {
        switch (ch) {
        case 'b':
        case 'c':
        case 'f':
            if (mode != CUT_NONE)
                usage();
            mode = (ch == 'b') ? CUT_BYTES :
                   (ch == 'c') ? CUT_CHARS : CUT_FIELDS;
            parse_list(xn_optarg);
            break;
        case 'd':
            if (xn_optarg[0] == '\0' || xn_optarg[1] != '\0')
                xn_errx(EXIT_FAILURE, "delimiter must be one character");
            delim = (unsigned char)xn_optarg[0];
            break;
        case 'n':
            break;
        case 's':
            suppress = 1;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (mode == CUT_NONE)
        usage();

    if (argc == 0) {
        if (cut_stream(stdin) == -1) {
            xn_warn("standard input");
            status = EXIT_FAILURE;
        }
        return xn_finish(status);
    }
    for (int i = 0; i < argc; i++) {
        FILE *fp = stdin;

        if (strcmp(argv[i], "-") != 0 &&
            (fp = fopen(argv[i], "r")) == NULL) {
            xn_warn("%s", argv[i]);
            status = EXIT_FAILURE;
            continue;
        }
        if (cut_stream(fp) == -1) {
            xn_warn("%s", argv[i]);
            status = EXIT_FAILURE;
        }
        if (fp != stdin)
            (void)fclose(fp);
    }
    return xn_finish(status);
}
