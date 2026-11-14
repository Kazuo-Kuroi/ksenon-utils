/* rm - remove directory entries. SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xn.h"

struct rm_state {
    int force;
    int interactive;
    int recursive;
    int status;
    int declined;
    int unreadable;
};

static int forbidden(const char *);
static int prompt_needed(const struct rm_state *, const char *,
                         const struct stat *);
static int confirm(const char *, const char *);
static int remove_cb(const struct xn_walk_ent *, void *);
static void usage(void);

/* POSIX forbids dot and dot-dot; removing the root is refused as well. */
static int
forbidden(const char *path)
{
    size_t len = strlen(path);
    size_t start;

    while (len > 0 && path[len - 1] == '/')
        len--;
    if (len == 0 && path[0] == '/')
        return 1;
    start = len;
    while (start > 0 && path[start - 1] != '/')
        start--;
    return (len - start == 1 && path[start] == '.') ||
           (len - start == 2 && path[start] == '.' &&
            path[start + 1] == '.');
}

static int
prompt_needed(const struct rm_state *state, const char *path,
              const struct stat *st)
{
    if (state->force)
        return 0;
    if (state->interactive)
        return 1;
    return !S_ISLNK(st->st_mode) && access(path, W_OK) == -1 &&
           isatty(STDIN_FILENO);
}

static int
confirm(const char *question, const char *path)
{
    (void)fprintf(stderr, "%s: %s %s? ", xn_progname(), question, path);
    return xn_affirm();
}

static int
remove_cb(const struct xn_walk_ent *ent, void *arg)
{
    struct rm_state *state = arg;

    switch (ent->type) {
    case XN_WALK_STATERR:
        if (!(state->force && ent->err == ENOENT)) {
            errno = ent->err;
            xn_warn("%s", ent->path);
            state->status = EXIT_FAILURE;
        }
        break;
    case XN_WALK_FILE:
        if (prompt_needed(state, ent->access, ent->st) &&
            !confirm("remove", ent->path)) {
            state->declined = 1;
            break;
        }
        if (unlink(ent->access) == -1 &&
            !(state->force && errno == ENOENT)) {
            xn_warn("%s", ent->path);
            state->status = EXIT_FAILURE;
        }
        break;
    case XN_WALK_DIR:
        if (!state->recursive) {
            xn_warnx("%s: is a directory", ent->path);
            state->status = EXIT_FAILURE;
            return XN_WALK_SKIP;
        }
        if (prompt_needed(state, ent->access, ent->st) &&
            !confirm("examine files in directory", ent->path)) {
            state->declined = 1;
            return XN_WALK_SKIP;
        }
        break;
    case XN_WALK_DIRPOST:
        if (prompt_needed(state, ent->access, ent->st) &&
            !confirm("remove directory", ent->path)) {
            state->declined = 1;
            break;
        }
        if (rmdir(ent->access) == -1) {
            /* A declined child legitimately leaves its parent non-empty. */
            if (errno != ENOTEMPTY || !state->declined) {
                if (state->unreadable != 0)
                    errno = state->unreadable;
                xn_warn("%s", ent->path);
                state->status = EXIT_FAILURE;
            }
        }
        state->unreadable = 0;
        break;
    case XN_WALK_DIRERR:
        /* It may still be empty, so report this only if rmdir fails too. */
        state->unreadable = ent->err;
        break;
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
    (void)fprintf(stderr, "usage: %s [-fiRr] file...\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Remove each operand, descending into directories with -R. */
int
main(int argc, char **argv)
{
    struct rm_state state;
    int ch;

    xn_init(argv);
    memset(&state, 0, sizeof(state));
    state.status = EXIT_SUCCESS;
    while ((ch = xn_getopt(argc, argv, "fiRr")) != -1) {
        switch (ch) {
        case 'f':
            state.force = 1;
            state.interactive = 0;
            break;
        case 'i':
            state.interactive = 1;
            state.force = 0;
            break;
        case 'R':
        case 'r':
            state.recursive = 1;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc == 0 && !state.force)
        usage();

    for (int i = 0; i < argc; i++) {
        if (forbidden(argv[i])) {
            xn_warnx("%s: refusing to remove", argv[i]);
            state.status = EXIT_FAILURE;
            continue;
        }
        xn_walk(argv[i], XN_WALK_POSTORDER, remove_cb, &state);
    }
    return state.status;
}
