/* pwd - return working directory name. SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xn.h"

static int logical_valid(const char *);
static char *physical(void);
static void usage(void);

/* $PWD counts only if it is absolute, canonical and really names ".". */
static int
logical_valid(const char *path)
{
    struct stat dot, env;
    const char *ptr = path;

    if (path == NULL || path[0] != '/')
        return 0;
    while ((ptr = strstr(ptr, "/.")) != NULL) {
        ptr += 2;
        if (*ptr == '.')
            ptr++;
        if (*ptr == '/' || *ptr == '\0')
            return 0;
    }
    if (stat(".", &dot) == -1 || stat(path, &env) == -1)
        return 0;
    return dot.st_dev == env.st_dev && dot.st_ino == env.st_ino;
}

static char *
physical(void)
{
    size_t cap = 1024;
    char *buf = xn_malloc(cap);

    while (getcwd(buf, cap) == NULL) {
        if (errno != ERANGE)
            xn_err(EXIT_FAILURE, "cannot determine current directory");
        cap *= 2;
        buf = xn_realloc(buf, cap);
    }
    return buf;
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-L|-P]\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Print the working directory; -L, the default, honours a valid $PWD. */
int
main(int argc, char **argv)
{
    const char *env = getenv("PWD");
    int logical = 1;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "LP")) != -1) {
        if (ch == 'L')
            logical = 1;
        else if (ch == 'P')
            logical = 0;
        else
            usage();
    }
    if (xn_optind != argc)
        usage();

    (void)puts((logical && logical_valid(env)) ? env : physical());
    return xn_finish(EXIT_SUCCESS);
}
