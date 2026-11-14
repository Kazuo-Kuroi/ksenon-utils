/* dirname - return the directory portion of a pathname.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xn.h"

static void usage(void);

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s string\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Print a pathname with its final component removed. */
int
main(int argc, char **argv)
{
    char *text;
    size_t len;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "")) != -1)
        usage();
    if (argc - xn_optind != 1)
        usage();

    text = argv[xn_optind];
    len = strlen(text);
    while (len > 1 && text[len - 1] == '/')
        len--;
    while (len > 0 && text[len - 1] != '/')
        len--;
    if (len == 0) {
        (void)puts(strcmp(text, "") != 0 && text[0] == '/' ? "/" : ".");
        return xn_finish(EXIT_SUCCESS);
    }
    while (len > 1 && text[len - 1] == '/')
        len--;
    text[len] = '\0';
    (void)puts(text);
    return xn_finish(EXIT_SUCCESS);
}
