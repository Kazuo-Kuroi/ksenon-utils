/* cmp - compare two files. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xn.h"

#define EXIT_DIFFER 1
#define EXIT_ERROR 2

static FILE *open_input(const char *);
static void usage(void);

static FILE *
open_input(const char *name)
{
    FILE *fp = stdin;

    if (strcmp(name, "-") != 0 && (fp = fopen(name, "r")) == NULL)
        xn_err(EXIT_ERROR, "%s", name);
    return fp;
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-l|-s] file1 file2\n", xn_progname());
    exit(EXIT_ERROR);
}

/*
 * Compare byte by byte. Exit 0 if identical, 1 if different, 2 on error.
 * -l lists every difference; -s reports through the exit status alone.
 */
int
main(int argc, char **argv)
{
    FILE *first, *second;
    uintmax_t byte = 1, line = 1;
    int list = 0, silent = 0;
    int status = EXIT_SUCCESS;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "ls")) != -1) {
        if (ch == 'l')
            list = 1;
        else if (ch == 's')
            silent = 1;
        else
            usage();
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc != 2 || (list && silent))
        usage();

    first = open_input(argv[0]);
    second = open_input(argv[1]);

    for (;;) {
        int c1 = getc(first);
        int c2 = getc(second);

        if (c1 == EOF || c2 == EOF) {
            if (ferror(first) || ferror(second)) {
                xn_warn("%s", ferror(first) ? argv[0] : argv[1]);
                return xn_finish(EXIT_ERROR);
            }
            if (c1 != c2) {
                if (!silent)
                    (void)fprintf(stderr, "%s: EOF on %s\n", xn_progname(),
                                  (c1 == EOF) ? argv[0] : argv[1]);
                status = EXIT_DIFFER;
            }
            break;
        }
        if (c1 != c2) {
            status = EXIT_DIFFER;
            if (list) {
                (void)printf("%ju %o %o\n", byte, (unsigned int)c1,
                             (unsigned int)c2);
            } else {
                if (!silent)
                    (void)printf("%s %s differ: char %ju, line %ju\n",
                                 argv[0], argv[1], byte, line);
                break;
            }
        }
        if (c1 == '\n')
            line++;
        byte++;
    }
    status = xn_finish(status);
    return (status == EXIT_FAILURE && ferror(stdout)) ? EXIT_ERROR : status;
}
