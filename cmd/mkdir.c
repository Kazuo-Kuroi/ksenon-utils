/* mkdir - make directories. SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "xn.h"

#define DIR_DEFAULT 0777
#define INTERMEDIATE_BITS 0300

static int make_final(const char *, mode_t, int, int);
static int make_parents(char *, mode_t);
static void usage(void);

/*
 * Create the leading components of path. POSIX gives intermediates the
 * default mode plus u+wx, so the rest of the path can be created in them.
 */
static int
make_parents(char *path, mode_t cmask)
{
    mode_t mode = (DIR_DEFAULT & ~cmask) | INTERMEDIATE_BITS;
    char *slash = path;

    while (*slash == '/')
        slash++;
    while ((slash = strchr(slash, '/')) != NULL) {
        struct stat st;

        *slash = '\0';
        if (mkdir(path, mode) == -1 && errno != EEXIST &&
            (stat(path, &st) == -1 || !S_ISDIR(st.st_mode))) {
            xn_warn("%s", path);
            *slash = '/';
            return -1;
        }
        *slash = '/';
        while (*slash == '/')
            slash++;
    }
    return 0;
}

static int
make_final(const char *path, mode_t mode, int explicit_mode, int parents)
{
    struct stat st;

    if (mkdir(path, mode) == -1) {
        if (parents && errno == EEXIST && stat(path, &st) == 0 &&
            S_ISDIR(st.st_mode))
            return 0;
        xn_warn("%s", path);
        return -1;
    }
    /* mkdir(2) applies the umask; an explicit -m mode must not be masked. */
    if (explicit_mode && chmod(path, mode) == -1) {
        xn_warn("%s", path);
        return -1;
    }
    return 0;
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-p] [-m mode] dir...\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Create each directory operand. */
int
main(int argc, char **argv)
{
    const char *mode_text = NULL;
    mode_t cmask = xn_umask();
    mode_t mode = DIR_DEFAULT;
    int parents = 0;
    int status = EXIT_SUCCESS;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "m:p")) != -1) {
        switch (ch) {
        case 'm':
            mode_text = xn_optarg;
            break;
        case 'p':
            parents = 1;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc == 0)
        usage();

    if (mode_text != NULL) {
        struct xn_mode *compiled = xn_mode_compile(mode_text);

        if (compiled == NULL)
            xn_errx(EXIT_FAILURE, "%s: invalid mode", mode_text);
        mode = xn_mode_apply(compiled, S_IFDIR | DIR_DEFAULT, cmask);
        free(compiled);
    }

    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]);

        while (len > 1 && argv[i][len - 1] == '/')
            argv[i][--len] = '\0';
        if (parents && make_parents(argv[i], cmask) == -1) {
            status = EXIT_FAILURE;
            continue;
        }
        if (make_final(argv[i], mode, mode_text != NULL, parents) == -1)
            status = EXIT_FAILURE;
    }
    return status;
}
