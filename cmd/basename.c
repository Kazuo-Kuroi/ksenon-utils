/* basename - return the non-directory portion of a pathname.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xn.h"

static void usage(void);

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s string [suffix]\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Print the final component of a pathname, less an optional suffix. */
int
main(int argc, char **argv)
{
    char *text;
    size_t len;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "")) != -1)
        usage();
    argc -= xn_optind;
    argv += xn_optind;
    if (argc < 1 || argc > 2)
        usage();

    text = argv[0];
    len = strlen(text);
    if (len == 0) {
        (void)puts(".");
        return xn_finish(EXIT_SUCCESS);
    }
    while (len > 1 && text[len - 1] == '/')
        text[--len] = '\0';
    if (strcmp(text, "/") != 0) {
        const char *slash = strrchr(text, '/');

        if (slash != NULL)
            text = (char *)slash + 1;
        len = strlen(text);
        if (argc == 2) {
            size_t suffix = strlen(argv[1]);

            if (suffix < len && strcmp(text + len - suffix, argv[1]) == 0)
                text[len - suffix] = '\0';
        }
    }
    (void)puts(text);
    return xn_finish(EXIT_SUCCESS);
}
