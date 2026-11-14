/* chmod - change the file modes. SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "xn.h"

struct chmod_state {
    const struct xn_mode *mode;
    mode_t cmask;
    int status;
};

static int change_cb(const struct xn_walk_ent *, void *);
static void usage(void);

static int
change_cb(const struct xn_walk_ent *ent, void *arg)
{
    struct chmod_state *state = arg;

    switch (ent->type) {
    case XN_WALK_FILE:
    case XN_WALK_DIR:
        /* Links met during traversal have no mode of their own to change. */
        if (S_ISLNK(ent->st->st_mode))
            break;
        if (chmod(ent->access, xn_mode_apply(state->mode, ent->st->st_mode,
                                             state->cmask)) == -1) {
            xn_warn("%s", ent->path);
            state->status = EXIT_FAILURE;
        }
        break;
    case XN_WALK_DIRPOST:
        break;
    case XN_WALK_DIRERR:
    case XN_WALK_STATERR:
    case XN_WALK_CYCLE:
        errno = ent->err;
        xn_warn("%s", ent->path);
        state->status = EXIT_FAILURE;
        break;
    }
    return 0;
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-R] mode file...\n", xn_progname());
    exit(EXIT_FAILURE);
}

/*
 * Change file modes. Options are scanned by hand because a symbolic mode
 * such as "-w" is itself a valid operand and must not reach getopt.
 */
int
main(int argc, char **argv)
{
    struct chmod_state state;
    struct xn_mode *mode;
    int recursive = 0;
    int i = 1;

    xn_init(argv);
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-R") != 0)
            break;
        recursive = 1;
    }
    if (argc - i < 2)
        usage();

    if ((mode = xn_mode_compile(argv[i])) == NULL)
        xn_errx(EXIT_FAILURE, "%s: invalid mode", argv[i]);
    state.mode = mode;
    state.cmask = xn_umask();
    state.status = EXIT_SUCCESS;

    for (i++; i < argc; i++) {
        if (recursive) {
            xn_walk(argv[i], XN_WALK_COMFOLLOW, change_cb, &state);
        } else {
            struct stat st;

            if (stat(argv[i], &st) == -1 ||
                chmod(argv[i], xn_mode_apply(mode, st.st_mode,
                                             state.cmask)) == -1) {
                xn_warn("%s", argv[i]);
                state.status = EXIT_FAILURE;
            }
        }
    }
    free(mode);
    return state.status;
}
