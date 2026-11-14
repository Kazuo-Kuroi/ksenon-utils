/* paste - merge corresponding or subsequent lines of files.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xn.h"

#define NO_DELIM (-1)

static size_t parse_list(const char *, int **);
static void put_delim(int);
static int paste_parallel(FILE **, int, const int *, size_t);
static int paste_serial(FILE *, const int *, size_t);
static void usage(void);

static char *line;
static size_t line_cap;

/* Decode a -d list. "\0" means no delimiter, not a NUL byte. */
static size_t
parse_list(const char *text, int **listp)
{
    int *list = xn_malloc((strlen(text) + 1) * sizeof(*list));
    size_t count = 0;

    for (; *text != '\0'; text++) {
        int value = (unsigned char)*text;

        if (*text == '\\' && text[1] != '\0') {
            text++;
            if (*text == 'n')
                value = '\n';
            else if (*text == 't')
                value = '\t';
            else if (*text == '0')
                value = NO_DELIM;
            else
                value = (unsigned char)*text;
        }
        list[count++] = value;
    }
    *listp = list;
    return count;
}

static void
put_delim(int delim)
{
    if (delim != NO_DELIM)
        (void)putchar(delim);
}

static int
paste_parallel(FILE **files, int nfiles, const int *delims, size_t ndelims)
{
    for (;;) {
        size_t di = 0;
        int live = 0;

        /*
         * Output for a line is emitted as it is read, so hold the separators
         * until some file proves the line exists at all.
         */
        size_t pending = 0;

        for (int i = 0; i < nfiles; i++) {
            ssize_t len = -1;

            if (files[i] != NULL)
                len = xn_getdelim(&line, &line_cap, '\n', files[i]);
            if (len == -1) {
                files[i] = NULL;
            } else {
                while (pending > 0) {
                    put_delim(delims[di]);
                    di = (di + 1) % ndelims;
                    pending--;
                }
                if (line[len - 1] == '\n')
                    len--;
                (void)fwrite(line, 1, (size_t)len, stdout);
                live = 1;
            }
            if (i + 1 < nfiles)
                pending++;
        }
        if (!live)
            return 0;
        while (pending > 0) {
            put_delim(delims[di]);
            di = (di + 1) % ndelims;
            pending--;
        }
        (void)putchar('\n');
    }
}

static int
paste_serial(FILE *fp, const int *delims, size_t ndelims)
{
    size_t di = 0;
    int first = 1;
    ssize_t len;

    while ((len = xn_getdelim(&line, &line_cap, '\n', fp)) != -1) {
        if (!first) {
            put_delim(delims[di]);
            di = (di + 1) % ndelims;
        }
        first = 0;
        if (line[len - 1] == '\n')
            len--;
        (void)fwrite(line, 1, (size_t)len, stdout);
    }
    (void)putchar('\n');
    return ferror(fp) ? -1 : 0;
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-s] [-d list] file...\n",
                  xn_progname());
    exit(EXIT_FAILURE);
}

/* Join lines across files, or within each file with -s. */
int
main(int argc, char **argv)
{
    static int tab[] = { '\t' };
    const int *delims = tab;
    size_t ndelims = 1;
    int *parsed = NULL;
    FILE **files;
    int serial = 0;
    int status = EXIT_SUCCESS;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "d:s")) != -1) {
        switch (ch) {
        case 'd':
            free(parsed);
            ndelims = parse_list(xn_optarg, &parsed);
            if (ndelims == 0)
                xn_errx(EXIT_FAILURE, "delimiter list is empty");
            delims = parsed;
            break;
        case 's':
            serial = 1;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc == 0)
        usage();

    files = xn_malloc((size_t)argc * sizeof(*files));
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0)
            files[i] = stdin;
        else if ((files[i] = fopen(argv[i], "r")) == NULL)
            xn_err(EXIT_FAILURE, "%s", argv[i]);
    }

    if (serial) {
        for (int i = 0; i < argc; i++) {
            if (paste_serial(files[i], delims, ndelims) == -1) {
                xn_warn("%s", argv[i]);
                status = EXIT_FAILURE;
            }
        }
    } else {
        (void)paste_parallel(files, argc, delims, ndelims);
    }
    free(files);
    free(parsed);
    return xn_finish(status);
}
