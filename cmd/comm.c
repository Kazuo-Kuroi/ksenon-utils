/* comm - select or reject lines common to two files.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xn.h"

struct input {
    FILE *fp;
    const char *name;
    char *line;
    size_t cap;
    size_t len;
    int eof;
};

static void advance(struct input *);
static void put_line(const struct input *, int);
static void usage(void);

static int suppress[3];

static void
advance(struct input *in)
{
    ssize_t len = xn_getdelim(&in->line, &in->cap, '\n', in->fp);

    if (len == -1) {
        if (ferror(in->fp))
            xn_err(EXIT_FAILURE, "%s", in->name);
        in->eof = 1;
        return;
    }
    if (in->line[len - 1] == '\n')
        in->line[--len] = '\0';
    in->len = (size_t)len;
}

/* Write a line in a column, indented past the columns still shown. */
static void
put_line(const struct input *in, int column)
{
    if (suppress[column])
        return;
    for (int i = 0; i < column; i++) {
        if (!suppress[i])
            (void)putchar('\t');
    }
    (void)fwrite(in->line, 1, in->len, stdout);
    (void)putchar('\n');
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-123] file1 file2\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Compare two sorted files line by line, using the locale's collation. */
int
main(int argc, char **argv)
{
    struct input in[2];
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "123")) != -1) {
        if (ch != '1' && ch != '2' && ch != '3')
            usage();
        suppress[ch - '1'] = 1;
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc != 2)
        usage();

    memset(in, 0, sizeof(in));
    for (int i = 0; i < 2; i++) {
        in[i].name = argv[i];
        if (strcmp(argv[i], "-") == 0)
            in[i].fp = stdin;
        else if ((in[i].fp = fopen(argv[i], "r")) == NULL)
            xn_err(EXIT_FAILURE, "%s", argv[i]);
        advance(&in[i]);
    }

    while (!in[0].eof || !in[1].eof) {
        int order;

        if (in[0].eof)
            order = 1;
        else if (in[1].eof)
            order = -1;
        else
            order = strcoll(in[0].line, in[1].line);

        if (order < 0) {
            put_line(&in[0], 0);
            advance(&in[0]);
        } else if (order > 0) {
            put_line(&in[1], 1);
            advance(&in[1]);
        } else {
            put_line(&in[0], 2);
            advance(&in[0]);
            advance(&in[1]);
        }
    }
    return xn_finish(EXIT_SUCCESS);
}
