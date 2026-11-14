/*
 * chown, chgrp - change the file ownership. SPDX-License-Identifier: MIT
 *
 * Both names share this binary; argv[0] selects which operand syntax is
 * in force.
 */
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "xn.h"

struct chown_state {
    uid_t uid;
    gid_t gid;
    int nofollow;
    int status;
};

static gid_t lookup_group(const char *);
static uid_t lookup_user(const char *);
static int apply(const struct chown_state *, const char *, int);
static int change_cb(const struct xn_walk_ent *, void *);
static void usage(void);

static int is_chgrp;

static uid_t
lookup_user(const char *name)
{
    const struct passwd *entry = getpwnam(name);
    uintmax_t value;

    if (entry != NULL)
        return entry->pw_uid;
    if (xn_parse_uint(name, &value) == -1)
        xn_errx(EXIT_FAILURE, "%s: unknown user", name);
    return (uid_t)value;
}

static gid_t
lookup_group(const char *name)
{
    const struct group *entry = getgrnam(name);
    uintmax_t value;

    if (entry != NULL)
        return entry->gr_gid;
    if (xn_parse_uint(name, &value) == -1)
        xn_errx(EXIT_FAILURE, "%s: unknown group", name);
    return (gid_t)value;
}

static int
apply(const struct chown_state *state, const char *path, int nofollow)
{
    if (nofollow)
        return lchown(path, state->uid, state->gid);
    return chown(path, state->uid, state->gid);
}

static int
change_cb(const struct xn_walk_ent *ent, void *arg)
{
    struct chown_state *state = arg;

    switch (ent->type) {
    case XN_WALK_FILE:
    case XN_WALK_DIR:
        if (apply(state, ent->access, S_ISLNK(ent->st->st_mode)) == -1) {
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
    const char *name = xn_progname();
    const char *spec = is_chgrp ? "group" : "owner[:group]";

    (void)fprintf(stderr,
                  "usage: %s [-h] %s file...\n"
                  "       %s -R [-H|-L|-P] %s file...\n",
                  name, spec, name, spec);
    exit(EXIT_FAILURE);
}

/* Change the owner and/or group of each operand. */
int
main(int argc, char **argv)
{
    struct chown_state state;
    int recursive = 0;
    int walk_flags = 0;
    char *spec;
    int ch;

    xn_init(argv);
    is_chgrp = (strcmp(xn_progname(), "chgrp") == 0);
    state.nofollow = 0;
    state.status = EXIT_SUCCESS;
    while ((ch = xn_getopt(argc, argv, "HLPRh")) != -1) {
        switch (ch) {
        case 'H':
            walk_flags = XN_WALK_COMFOLLOW;
            break;
        case 'L':
            walk_flags = XN_WALK_LOGICAL;
            break;
        case 'P':
            walk_flags = 0;
            break;
        case 'R':
            recursive = 1;
            break;
        case 'h':
            state.nofollow = 1;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc < 2)
        usage();

    state.uid = (uid_t)-1;
    state.gid = (gid_t)-1;
    spec = argv[0];
    if (is_chgrp) {
        state.gid = lookup_group(spec);
    } else {
        char *colon = strchr(spec, ':');

        if (colon != NULL) {
            *colon = '\0';
            if (colon[1] != '\0')
                state.gid = lookup_group(colon + 1);
        }
        if (*spec != '\0')
            state.uid = lookup_user(spec);
    }

    for (int i = 1; i < argc; i++) {
        if (recursive) {
            xn_walk(argv[i], walk_flags, change_cb, &state);
        } else if (apply(&state, argv[i], state.nofollow) == -1) {
            xn_warn("%s", argv[i]);
            state.status = EXIT_FAILURE;
        }
    }
    return state.status;
}
