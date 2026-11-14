/*
 * File hierarchy traversal. SPDX-License-Identifier: MIT
 *
 * The walker moves through the tree with chdir and hands callbacks a short
 * relative name, so depth is not limited by PATH_MAX. Its state lives on
 * the heap rather than in recursion, so depth is not limited by the stack
 * either. Each directory's names are read and the stream closed before
 * descending, which also lets callbacks remove entries as they go.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "xn.h"

#define LEVELS_INITIAL 32

struct level {
    struct stat st;
    struct xn_dirstep step;
    char **names;
    size_t count;
    size_t next;
    size_t len;
    const char *name;
    int entered;
};

struct walker {
    xn_walk_cb *cb;
    void *arg;
    char *path;
    size_t cap;
    struct level *levels;
    size_t nlevels;
    size_t levels_cap;
    dev_t rootdev;
    int flags;
};

static int emit(struct walker *, const char *, const struct stat *,
                enum xn_walk_type, int, int);
static char **list_names(const char *, size_t *, int *);
static void reserve(struct walker *, size_t);
static int in_cycle(const struct walker *, const struct stat *);
static void visit(struct walker *, const char *, size_t);
static void finish_level(struct walker *);
static int save_origin(struct xn_dirstep *);
static int needs_saved_origin(const char *);

static int
needs_saved_origin(const char *name)
{
    struct stat st;

    if (strchr(name, '/') != NULL || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return 1;
    return lstat(name, &st) == 0 && S_ISLNK(st.st_mode);
}

/*
 * Remember the current directory when ".." will not lead back to it. An
 * unreadable directory cannot be opened, so fall back on its pathname.
 */
static int
save_origin(struct xn_dirstep *step)
{
    size_t cap = PATH_MAX;

    if ((step->fd = open(".", O_RDONLY)) != -1)
        return 0;
    step->path = xn_malloc(cap);
    while (getcwd(step->path, cap) == NULL) {
        if (errno != ERANGE) {
            int err = errno;

            free(step->path);
            step->path = NULL;
            errno = err;
            return -1;
        }
        cap *= 2;
        step->path = xn_realloc(step->path, cap);
    }
    return 0;
}

/*
 * Change into name, recording how to get back. When expected is given, the
 * directory reached must be that one: this stops a directory swapped for a
 * symbolic link mid-walk from redirecting a recursive removal.
 */
int
xn_dir_enter(const char *name, const struct stat *expected,
             struct xn_dirstep *step)
{
    struct stat st;

    step->path = NULL;
    step->fd = -1;
    if (stat(".", &st) == -1)
        return -1;
    step->dev = st.st_dev;
    step->ino = st.st_ino;
    if (needs_saved_origin(name) && save_origin(step) == -1)
        return -1;
    if (chdir(name) == -1) {
        int err = errno;

        if (step->fd != -1)
            (void)close(step->fd);
        free(step->path);
        step->path = NULL;
        step->fd = -1;
        errno = err;
        return -1;
    }
    if (expected != NULL && (stat(".", &st) == -1 ||
                             st.st_dev != expected->st_dev ||
                             st.st_ino != expected->st_ino)) {
        xn_dir_leave(step);
        errno = ESTALE;
        return -1;
    }
    return 0;
}

/*
 * Undo xn_dir_enter. Failure is fatal: if the tree moved underneath us the
 * working directory is unknown, and carrying on could touch the wrong files.
 */
void
xn_dir_leave(struct xn_dirstep *step)
{
    struct stat st;

    if (step->fd != -1) {
        if (fchdir(step->fd) == -1)
            xn_err(EXIT_FAILURE, "cannot return to previous directory");
        (void)close(step->fd);
        step->fd = -1;
    } else if (step->path != NULL) {
        if (chdir(step->path) == -1)
            xn_err(EXIT_FAILURE, "%s", step->path);
        free(step->path);
        step->path = NULL;
    } else {
        if (chdir("..") == -1 || stat(".", &st) == -1)
            xn_err(EXIT_FAILURE, "cannot return to parent directory");
        if (st.st_dev != step->dev || st.st_ino != step->ino)
            xn_errx(EXIT_FAILURE, "directory tree changed during traversal");
    }
}

static int
emit(struct walker *walker, const char *access, const struct stat *st,
     enum xn_walk_type type, int depth, int err)
{
    struct xn_walk_ent ent;

    ent.path = walker->path;
    ent.access = access;
    ent.st = st;
    ent.type = type;
    ent.depth = depth;
    ent.err = err;
    return walker->cb(&ent, walker->arg);
}

/* Return the entries of a directory, excluding dot and dot-dot. */
static char **
list_names(const char *path, size_t *countp, int *errp)
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    char **names = NULL;
    size_t count = 0;
    size_t cap = 0;

    *errp = 0;
    *countp = 0;
    if (dir == NULL) {
        *errp = errno;
        return NULL;
    }
    for (;;) {
        errno = 0;
        if ((entry = readdir(dir)) == NULL)
            break;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (count == cap) {
            cap = (cap != 0) ? cap * 2 : 32;
            names = xn_realloc(names, cap * sizeof(*names));
        }
        names[count++] = xn_strdup(entry->d_name);
    }
    if (errno != 0)
        *errp = errno;
    (void)closedir(dir);
    *countp = count;
    return names;
}

static void
reserve(struct walker *walker, size_t size)
{
    if (size > walker->cap) {
        walker->cap = size * 2;
        walker->path = xn_realloc(walker->path, walker->cap);
    }
}

/* A physical walk cannot loop, so only -L pays for this linear scan. */
static int
in_cycle(const struct walker *walker, const struct stat *st)
{
    if ((walker->flags & XN_WALK_LOGICAL) == 0)
        return 0;
    for (size_t i = 0; i < walker->nlevels; i++) {
        if (walker->levels[i].st.st_dev == st->st_dev &&
            walker->levels[i].st.st_ino == st->st_ino)
            return 1;
    }
    return 0;
}

/*
 * Examine one object. A directory that will be descended into becomes a new
 * level, which the loop in xn_walk then works through.
 */
static void
visit(struct walker *walker, const char *name, size_t len)
{
    struct stat st;
    struct level *level;
    int depth = (int)walker->nlevels;
    int err;
    int follow = (walker->flags & XN_WALK_LOGICAL) != 0 ||
                 (depth == 0 && (walker->flags & XN_WALK_COMFOLLOW) != 0);

    if (follow) {
        /* A dangling link is still an object that can be reported. */
        if (stat(name, &st) == -1 &&
            (errno != ENOENT || lstat(name, &st) == -1)) {
            (void)emit(walker, name, NULL, XN_WALK_STATERR, depth, errno);
            return;
        }
    } else if (lstat(name, &st) == -1) {
        (void)emit(walker, name, NULL, XN_WALK_STATERR, depth, errno);
        return;
    }

    if (depth == 0)
        walker->rootdev = st.st_dev;
    else if ((walker->flags & XN_WALK_XDEV) != 0 &&
             st.st_dev != walker->rootdev)
        return;

    if (!S_ISDIR(st.st_mode)) {
        (void)emit(walker, name, &st, XN_WALK_FILE, depth, 0);
        return;
    }
    if (in_cycle(walker, &st)) {
        (void)emit(walker, name, &st, XN_WALK_CYCLE, depth, ELOOP);
        return;
    }
    if (emit(walker, name, &st, XN_WALK_DIR, depth, 0) == XN_WALK_SKIP)
        return;

    if (walker->nlevels == walker->levels_cap) {
        walker->levels_cap *= 2;
        walker->levels = xn_realloc(walker->levels, walker->levels_cap *
                                    sizeof(*walker->levels));
    }
    level = &walker->levels[walker->nlevels];
    level->st = st;
    level->name = name;
    level->len = len;
    level->next = 0;
    level->entered = 0;
    level->names = list_names(name, &level->count, &err);
    if (err != 0)
        (void)emit(walker, name, &st, XN_WALK_DIRERR, depth, err);

    /*
     * An empty directory is not entered, so one that denies search can
     * still be processed by its parent.
     */
    if (level->count > 0) {
        if (xn_dir_enter(name, &st, &level->step) == 0) {
            level->entered = 1;
        } else {
            (void)emit(walker, name, &st, XN_WALK_DIRERR, depth, errno);
            for (size_t i = 0; i < level->count; i++)
                free(level->names[i]);
            level->count = 0;
        }
    }
    walker->nlevels++;
}

static void
finish_level(struct walker *walker)
{
    struct level *level = &walker->levels[walker->nlevels - 1];
    struct stat st = level->st;
    const char *name = level->name;
    char **names = level->names;
    size_t count = level->count;

    if (level->entered)
        xn_dir_leave(&level->step);
    walker->nlevels--;
    walker->path[level->len] = '\0';
    if ((walker->flags & XN_WALK_POSTORDER) != 0)
        (void)emit(walker, name, &st, XN_WALK_DIRPOST,
                   (int)walker->nlevels, 0);

    /* A level's name lives in its parent's array; free this one's last. */
    for (size_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
}

/* Traverse the hierarchy at path, invoking the callback for each object. */
void
xn_walk(const char *path, int flags, xn_walk_cb *cb, void *arg)
{
    struct walker walker;
    size_t len = strlen(path);

    walker.cb = cb;
    walker.arg = arg;
    walker.path = NULL;
    walker.cap = 0;
    walker.nlevels = 0;
    walker.levels_cap = LEVELS_INITIAL;
    walker.levels = xn_malloc(walker.levels_cap * sizeof(*walker.levels));
    walker.rootdev = 0;
    walker.flags = flags;
    reserve(&walker, len + 1);
    memcpy(walker.path, path, len + 1);
    visit(&walker, path, len);

    while (walker.nlevels > 0) {
        struct level *level = &walker.levels[walker.nlevels - 1];

        if (level->next < level->count) {
            const char *name = level->names[level->next++];
            size_t namelen = strlen(name);
            size_t dirlen = level->len;
            size_t pos = dirlen;

            reserve(&walker, pos + namelen + 2);
            if (pos > 0 && walker.path[pos - 1] != '/')
                walker.path[pos++] = '/';
            memcpy(walker.path + pos, name, namelen + 1);

            /* visit may grow the level array, so level is stale after it. */
            visit(&walker, name, pos + namelen);
            walker.path[dirlen] = '\0';
        } else {
            finish_level(&walker);
        }
    }
    free(walker.levels);
    free(walker.path);
}
