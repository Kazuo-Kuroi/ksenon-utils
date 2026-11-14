/* du - estimate file space usage. SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "xn.h"

#define BUCKETS 4099

struct seen {
    dev_t dev;
    ino_t ino;
    struct seen *next;
};

struct du_state {
    uintmax_t *totals;
    size_t depth_cap;
    int show_all;
    int summary;
    int kilobytes;
    int status;
};

static int already_seen(const struct stat *);
static void show(const struct du_state *, uintmax_t, const char *);
static uintmax_t *slot(struct du_state *, int);
static int visit_cb(const struct xn_walk_ent *, void *);
static void usage(void);

static struct seen *table[BUCKETS];

/* Hard links must be counted once, so remember multiply linked inodes. */
static int
already_seen(const struct stat *st)
{
    size_t bucket = (size_t)((uintmax_t)st->st_ino % BUCKETS);
    struct seen *entry;

    for (entry = table[bucket]; entry != NULL; entry = entry->next) {
        if (entry->dev == st->st_dev && entry->ino == st->st_ino)
            return 1;
    }
    entry = xn_malloc(sizeof(*entry));
    entry->dev = st->st_dev;
    entry->ino = st->st_ino;
    entry->next = table[bucket];
    table[bucket] = entry;
    return 0;
}

static void
show(const struct du_state *state, uintmax_t blocks, const char *path)
{
    uintmax_t value = state->kilobytes ? (blocks + 1) / 2 : blocks;

    (void)printf("%ju\t%s\n", value, path);
}

static uintmax_t *
slot(struct du_state *state, int depth)
{
    size_t need = (size_t)depth + 2;

    if (need > state->depth_cap) {
        size_t cap = need * 2;

        state->totals = xn_realloc(state->totals,
                                   cap * sizeof(*state->totals));
        for (size_t i = state->depth_cap; i < cap; i++)
            state->totals[i] = 0;
        state->depth_cap = cap;
    }
    return &state->totals[depth];
}

static int
visit_cb(const struct xn_walk_ent *ent, void *arg)
{
    struct du_state *state = arg;
    uintmax_t blocks;

    switch (ent->type) {
    case XN_WALK_FILE:
        blocks = (uintmax_t)ent->st->st_blocks;
        if (ent->st->st_nlink > 1 && already_seen(ent->st))
            break;
        if (ent->depth > 0)
            *slot(state, ent->depth - 1) += blocks;
        if (ent->depth == 0 || (state->show_all && !state->summary))
            show(state, blocks, ent->path);
        break;
    case XN_WALK_DIR:
        *slot(state, ent->depth) = 0;
        break;
    case XN_WALK_DIRPOST:
        blocks = *slot(state, ent->depth) + (uintmax_t)ent->st->st_blocks;
        if (ent->depth > 0)
            state->totals[ent->depth - 1] += blocks;
        if (ent->depth == 0 || !state->summary)
            show(state, blocks, ent->path);
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
    (void)fprintf(stderr, "usage: %s [-a|-s] [-kx] [-H|-L] [file...]\n",
                  xn_progname());
    exit(EXIT_FAILURE);
}

/* Write the space used by each operand, in 512-byte units by default. */
int
main(int argc, char **argv)
{
    struct du_state state = { NULL, 0, 0, 0, 0, EXIT_SUCCESS };
    int flags = XN_WALK_POSTORDER;
    int follow = 0;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "HLakPsx")) != -1) {
        switch (ch) {
        case 'H':
            follow = XN_WALK_COMFOLLOW;
            break;
        case 'L':
            follow = XN_WALK_LOGICAL;
            break;
        case 'P':
            follow = 0;
            break;
        case 'a':
            state.show_all = 1;
            state.summary = 0;
            break;
        case 'k':
            state.kilobytes = 1;
            break;
        case 's':
            state.summary = 1;
            state.show_all = 0;
            break;
        case 'x':
            flags |= XN_WALK_XDEV;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;

    if (argc == 0) {
        xn_walk(".", flags | follow, visit_cb, &state);
    } else {
        for (int i = 0; i < argc; i++)
            xn_walk(argv[i], flags | follow, visit_cb, &state);
    }
    free(state.totals);
    return xn_finish(state.status);
}
