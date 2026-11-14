/*
 * mv, cp, ln - move, copy and link files. SPDX-License-Identifier: MIT
 *
 * These are one program, in the System V tradition: the three names are
 * hard links to a single binary and argv[0] selects the behaviour. The
 * sharing is natural, because a cross-device mv is a copy followed by a
 * removal and all three resolve their target operand identically.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "xn.h"

#define PERM_BITS 0777
#define MODE_BITS 07777
#define LINK_BUFSIZE 4096

struct cp_options {
    int force;
    int interactive;
    int preserve;
    int recursive;
    int walk_flags;
};

/*
 * One copy operation. src and dst are the names for system calls and the
 * _show fields the names for messages. In a deep tree the two ends sit in
 * different working directories, so src_fd and dst_fd hold those directories
 * and the copy switches between them; both are -1 when no switching is
 * needed because the names are valid from here.
 */
struct copy_item {
    const char *src;
    const char *dst;
    const char *src_show;
    const char *dst_show;
    int src_fd;
    int dst_fd;
};

struct dest_level {
    struct xn_dirstep step;
    int is_new;
};

struct tree_copy {
    const struct cp_options *options;
    const char *dest_root;
    struct dest_level *levels;
    size_t levels_cap;
    size_t src_len;
    int dest_fd;
    int status;
    int have_root;
    dev_t root_dev;
    ino_t root_ino;
};

static char *strip_slashes(const char *);
static char *target_in_dir(const char *, const char *);
static int is_directory(const char *);
static int confirm(const char *);
static void go_dest(const struct copy_item *);
static void go_source(const struct copy_item *);
static void preserve_times(const struct copy_item *, const struct stat *);
static void preserve_owner_mode(const struct copy_item *,
                                const struct stat *);
static int copy_regular(const struct copy_item *, const struct stat *,
                        const struct cp_options *);
static int copy_special(const struct copy_item *, const struct stat *,
                        const struct cp_options *);
static char *tree_dest(const struct tree_copy *, const char *);
static struct dest_level *dest_level_at(struct tree_copy *, int);
static int enter_dir(struct tree_copy *, const struct xn_walk_ent *,
                     const struct copy_item *);
static void leave_dir(struct tree_copy *, const struct xn_walk_ent *,
                      const struct copy_item *);
static int copy_cb(const struct xn_walk_ent *, void *);
static int copy_operand(const char *, const char *,
                        const struct cp_options *);
static int cp_main(int, char **);
static int remove_cb(const struct xn_walk_ent *, void *);
static int move_across(const char *, const struct stat *, const char *);
static int move_one(const char *, const char *, int, int);
static int mv_main(int, char **);
static int link_one(const char *, const char *, int, int);
static int ln_main(int, char **);
static void cp_usage(void);
static void mv_usage(void);
static void ln_usage(void);

/* Return a copy of path without trailing slashes, keeping a bare root. */
static char *
strip_slashes(const char *path)
{
    char *copy = xn_strdup(path);
    size_t len = strlen(copy);

    while (len > 1 && copy[len - 1] == '/')
        copy[--len] = '\0';
    return copy;
}

static char *
target_in_dir(const char *source, const char *dir)
{
    char *stripped = strip_slashes(source);
    char *target = xn_pathjoin(dir, xn_base(stripped));

    free(stripped);
    return target;
}

static int
is_directory(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int
confirm(const char *path)
{
    (void)fprintf(stderr, "%s: overwrite %s? ", xn_progname(), path);
    return xn_affirm();
}

/* Losing track of the working directory mid-copy cannot be recovered. */
static void
go_dest(const struct copy_item *item)
{
    if (item->dst_fd != -1 && fchdir(item->dst_fd) == -1)
        xn_err(EXIT_FAILURE, "%s", item->dst_show);
}

static void
go_source(const struct copy_item *item)
{
    if (item->src_fd != -1 && fchdir(item->src_fd) == -1)
        xn_err(EXIT_FAILURE, "%s", item->src_show);
}

static void
preserve_times(const struct copy_item *item, const struct stat *st)
{
    struct timeval times[2];

    times[0].tv_sec = st->st_atime;
    times[0].tv_usec = 0;
    times[1].tv_sec = st->st_mtime;
    times[1].tv_usec = 0;
    if (utimes(item->dst, times) == -1)
        xn_warn("%s: cannot preserve times", item->dst_show);
}

/*
 * Owner comes first, since chown clears set-ID bits. If the owner cannot be
 * kept, the set-ID bits must not be kept either: they would now confer the
 * authority of a different user.
 */
static void
preserve_owner_mode(const struct copy_item *item, const struct stat *st)
{
    mode_t mode = st->st_mode & MODE_BITS;

    if (chown(item->dst, st->st_uid, st->st_gid) == -1) {
        mode &= PERM_BITS;
        if (errno != EPERM)
            xn_warn("%s: cannot preserve owner", item->dst_show);
    }
    if (chmod(item->dst, mode) == -1)
        xn_warn("%s: cannot preserve mode", item->dst_show);
}

/* Copy file contents, handling every state the destination can be in. */
static int
copy_regular(const struct copy_item *item, const struct stat *st,
             const struct cp_options *options)
{
    struct stat dst;
    int exists;
    int in, out, result;

    go_dest(item);
    exists = (stat(item->dst, &dst) == 0);
    if (exists && dst.st_dev == st->st_dev && dst.st_ino == st->st_ino) {
        xn_warnx("%s and %s are identical", item->src_show, item->dst_show);
        go_source(item);
        return -1;
    }
    if (exists && S_ISDIR(dst.st_mode)) {
        xn_warnx("%s: is a directory", item->dst_show);
        go_source(item);
        return -1;
    }
    if (exists && options->interactive && !confirm(item->dst_show)) {
        go_source(item);
        return 0;
    }

    go_source(item);
    if ((in = open(item->src, O_RDONLY)) == -1) {
        xn_warn("%s", item->src_show);
        return -1;
    }
    go_dest(item);
    if (exists) {
        out = open(item->dst, O_WRONLY | O_TRUNC);
        if (out == -1 && options->force && unlink(item->dst) == 0)
            out = open(item->dst, O_WRONLY | O_CREAT | O_EXCL,
                       st->st_mode & PERM_BITS);
    } else {
        out = open(item->dst, O_WRONLY | O_CREAT | O_TRUNC,
                   st->st_mode & PERM_BITS);
    }
    if (out == -1) {
        xn_warn("%s", item->dst_show);
        (void)close(in);
        go_source(item);
        return -1;
    }

    result = xn_copyfd(in, out);
    if (result != XN_COPY_OK)
        xn_warn("%s", (result == XN_COPY_READ) ? item->src_show
                                               : item->dst_show);
    (void)close(in);
    if (close(out) == -1 && result == XN_COPY_OK) {
        xn_warn("%s", item->dst_show);
        result = XN_COPY_WRITE;
    }
    if (result == XN_COPY_OK && options->preserve) {
        preserve_times(item, st);
        preserve_owner_mode(item, st);
    }
    go_source(item);
    return (result == XN_COPY_OK) ? 0 : -1;
}

/* Recreate a link, FIFO or device node instead of copying its contents. */
static int
copy_special(const struct copy_item *item, const struct stat *st,
             const struct cp_options *options)
{
    char target[LINK_BUFSIZE];
    struct stat dst;
    int result = -1;

    if (S_ISLNK(st->st_mode)) {
        ssize_t len = readlink(item->src, target, sizeof(target) - 1);

        if (len == -1) {
            xn_warn("%s", item->src_show);
            return -1;
        }
        target[len] = '\0';
    }

    go_dest(item);
    if (lstat(item->dst, &dst) == 0) {
        if (dst.st_dev == st->st_dev && dst.st_ino == st->st_ino) {
            xn_warnx("%s and %s are identical", item->src_show,
                     item->dst_show);
            goto done;
        }
        if (options->interactive && !confirm(item->dst_show)) {
            result = 0;
            goto done;
        }
        if (unlink(item->dst) == -1) {
            xn_warn("%s", item->dst_show);
            goto done;
        }
    }

    if (S_ISLNK(st->st_mode)) {
        if (symlink(target, item->dst) == -1) {
            xn_warn("%s", item->dst_show);
            goto done;
        }
        if (options->preserve &&
            lchown(item->dst, st->st_uid, st->st_gid) == -1 &&
            errno != EPERM)
            xn_warn("%s: cannot preserve owner", item->dst_show);
        result = 0;
        goto done;
    }

    if (S_ISFIFO(st->st_mode)) {
        if (mkfifo(item->dst, st->st_mode & PERM_BITS) == -1) {
            xn_warn("%s", item->dst_show);
            goto done;
        }
    } else if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode)) {
        if (mknod(item->dst, st->st_mode & (S_IFMT | PERM_BITS),
                  st->st_rdev) == -1) {
            xn_warn("%s", item->dst_show);
            goto done;
        }
    } else {
        xn_warnx("%s: cannot copy this type of file", item->src_show);
        goto done;
    }
    if (options->preserve) {
        preserve_times(item, st);
        preserve_owner_mode(item, st);
    }
    result = 0;

done:
    go_source(item);
    return result;
}

static char *
tree_dest(const struct tree_copy *copy, const char *path)
{
    const char *rest = path + copy->src_len;

    while (*rest == '/')
        rest++;
    if (*rest == '\0')
        return xn_strdup(copy->dest_root);
    return xn_pathjoin(copy->dest_root, rest);
}

static struct dest_level *
dest_level_at(struct tree_copy *copy, int depth)
{
    size_t need = (size_t)depth + 1;

    if (need > copy->levels_cap) {
        copy->levels_cap = need * 2;
        copy->levels = xn_realloc(copy->levels, copy->levels_cap *
                                  sizeof(*copy->levels));
    }
    return &copy->levels[depth];
}

/*
 * Prepare the destination for a source directory and move the destination
 * position into it. A directory is created with owner rwx so that its
 * contents can be written, and given its final mode after they are complete.
 */
static int
enter_dir(struct tree_copy *copy, const struct xn_walk_ent *ent,
          const struct copy_item *item)
{
    struct dest_level *level = dest_level_at(copy, ent->depth);
    struct stat dst;
    mode_t bits = ent->st->st_mode & PERM_BITS;
    int fd;

    if (copy->have_root && ent->st->st_dev == copy->root_dev &&
        ent->st->st_ino == copy->root_ino) {
        xn_warnx("%s: cannot copy a directory into itself", ent->path);
        goto failed;
    }

    go_dest(item);
    if (stat(item->dst, &dst) == 0) {
        if (!S_ISDIR(dst.st_mode)) {
            xn_warnx("%s: not a directory", item->dst_show);
            goto failed;
        }
        if (dst.st_dev == ent->st->st_dev &&
            dst.st_ino == ent->st->st_ino) {
            xn_warnx("%s and %s are identical", ent->path, item->dst_show);
            goto failed;
        }
        level->is_new = 0;
    } else {
        if (mkdir(item->dst, bits | S_IRWXU) == -1) {
            xn_warn("%s", item->dst_show);
            goto failed;
        }
        level->is_new = 1;
    }

    if (ent->depth == 0) {
        if ((fd = open(item->dst, O_RDONLY)) == -1) {
            xn_warn("%s", item->dst_show);
            goto failed;
        }
        if (fstat(fd, &dst) == 0) {
            copy->have_root = 1;
            copy->root_dev = dst.st_dev;
            copy->root_ino = dst.st_ino;
        }
    } else {
        if (xn_dir_enter(item->dst, NULL, &level->step) == -1) {
            xn_warn("%s", item->dst_show);
            goto failed;
        }
        if ((fd = open(".", O_RDONLY)) == -1)
            xn_err(EXIT_FAILURE, "%s", item->dst_show);
        (void)close(copy->dest_fd);
    }
    copy->dest_fd = fd;
    go_source(item);
    return 0;

failed:
    go_source(item);
    copy->status = EXIT_FAILURE;
    return XN_WALK_SKIP;
}

/* Step the destination back up, then give the directory its final mode. */
static void
leave_dir(struct tree_copy *copy, const struct xn_walk_ent *ent,
          const struct copy_item *item)
{
    struct dest_level *level = dest_level_at(copy, ent->depth);
    struct copy_item parent = *item;

    if (ent->depth == 0) {
        (void)close(copy->dest_fd);
        copy->dest_fd = -1;
    } else {
        int fd;

        go_dest(item);
        xn_dir_leave(&level->step);
        if ((fd = open(".", O_RDONLY)) == -1)
            xn_err(EXIT_FAILURE, "%s", item->dst_show);
        (void)close(copy->dest_fd);
        copy->dest_fd = fd;
        parent.dst_fd = fd;
    }

    go_dest(&parent);
    if (copy->options->preserve) {
        preserve_times(&parent, ent->st);
        preserve_owner_mode(&parent, ent->st);
    } else if (level->is_new &&
               chmod(parent.dst, ent->st->st_mode & PERM_BITS &
                     ~xn_umask()) == -1) {
        xn_warn("%s", parent.dst_show);
        copy->status = EXIT_FAILURE;
    }
    go_source(&parent);
}

static int
copy_cb(const struct xn_walk_ent *ent, void *arg)
{
    struct tree_copy *copy = arg;
    struct copy_item item;
    char *show = tree_dest(copy, ent->path);
    int result = 0;

    item.src = ent->access;
    item.dst = (ent->depth == 0) ? copy->dest_root : ent->access;
    item.src_show = ent->path;
    item.dst_show = show;
    item.src_fd = -1;
    item.dst_fd = -1;
    if (ent->depth > 0 && ent->type != XN_WALK_STATERR) {
        /* The walker is reading this directory, so it can be opened. */
        if ((item.src_fd = open(".", O_RDONLY)) == -1)
            xn_err(EXIT_FAILURE, "%s", ent->path);
        item.dst_fd = copy->dest_fd;
    }

    switch (ent->type) {
    case XN_WALK_FILE:
        if (S_ISREG(ent->st->st_mode)) {
            if (copy_regular(&item, ent->st, copy->options) == -1)
                copy->status = EXIT_FAILURE;
        } else if (copy_special(&item, ent->st, copy->options) == -1) {
            copy->status = EXIT_FAILURE;
        }
        break;
    case XN_WALK_DIR:
        result = enter_dir(copy, ent, &item);
        break;
    case XN_WALK_DIRPOST:
        leave_dir(copy, ent, &item);
        break;
    case XN_WALK_DIRERR:
    case XN_WALK_STATERR:
    case XN_WALK_CYCLE:
        errno = ent->err;
        xn_warn("%s", ent->path);
        copy->status = EXIT_FAILURE;
        break;
    }
    if (item.src_fd != -1)
        (void)close(item.src_fd);
    free(show);
    return result;
}

static int
copy_operand(const char *source, const char *dest,
             const struct cp_options *options)
{
    struct copy_item item;
    struct stat st;

    if (options->recursive) {
        struct tree_copy copy;

        memset(&copy, 0, sizeof(copy));
        copy.options = options;
        copy.dest_root = dest;
        copy.src_len = strlen(source);
        copy.status = EXIT_SUCCESS;
        copy.dest_fd = -1;
        xn_walk(source, options->walk_flags | XN_WALK_POSTORDER, copy_cb,
                &copy);
        free(copy.levels);
        return (copy.status == EXIT_SUCCESS) ? 0 : -1;
    }

    if (stat(source, &st) == -1) {
        xn_warn("%s", source);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        xn_warnx("%s: is a directory (not copied)", source);
        return -1;
    }
    item.src = source;
    item.dst = dest;
    item.src_show = source;
    item.dst_show = dest;
    item.src_fd = -1;
    item.dst_fd = -1;
    return copy_regular(&item, &st, options);
}

static void
cp_usage(void)
{
    (void)fprintf(stderr,
                  "usage: cp [-fip] source_file target_file\n"
                  "       cp [-fip] source_file... target_dir\n"
                  "       cp -R|-r [-H|-L|-P] [-fip] source... target\n");
    exit(EXIT_FAILURE);
}

static int
cp_main(int argc, char **argv)
{
    struct cp_options options;
    int status = EXIT_SUCCESS;
    int ch;

    memset(&options, 0, sizeof(options));
    while ((ch = xn_getopt(argc, argv, "HLPRfipr")) != -1) {
        switch (ch) {
        case 'H':
            options.walk_flags = XN_WALK_COMFOLLOW;
            break;
        case 'L':
            options.walk_flags = XN_WALK_LOGICAL;
            break;
        case 'P':
            options.walk_flags = 0;
            break;
        case 'R':
        case 'r':
            options.recursive = 1;
            break;
        case 'f':
            options.force = 1;
            options.interactive = 0;
            break;
        case 'i':
            options.interactive = 1;
            options.force = 0;
            break;
        case 'p':
            options.preserve = 1;
            break;
        default:
            cp_usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc < 2)
        cp_usage();

    if (is_directory(argv[argc - 1])) {
        for (int i = 0; i < argc - 1; i++) {
            char *dest = target_in_dir(argv[i], argv[argc - 1]);

            if (copy_operand(argv[i], dest, &options) == -1)
                status = EXIT_FAILURE;
            free(dest);
        }
        return status;
    }
    if (argc != 2)
        xn_errx(EXIT_FAILURE, "%s: not a directory", argv[argc - 1]);
    return (copy_operand(argv[0], argv[1], &options) == -1) ? EXIT_FAILURE
                                                            : EXIT_SUCCESS;
}

static int
remove_cb(const struct xn_walk_ent *ent, void *arg)
{
    int *status = arg;

    switch (ent->type) {
    case XN_WALK_FILE:
        if (unlink(ent->access) == -1) {
            xn_warn("%s", ent->path);
            *status = EXIT_FAILURE;
        }
        break;
    case XN_WALK_DIRPOST:
        if (rmdir(ent->access) == -1) {
            xn_warn("%s", ent->path);
            *status = EXIT_FAILURE;
        }
        break;
    case XN_WALK_DIR:
        break;
    case XN_WALK_DIRERR:
    case XN_WALK_STATERR:
    case XN_WALK_CYCLE:
        errno = ent->err;
        xn_warn("%s", ent->path);
        *status = EXIT_FAILURE;
        break;
    }
    return 0;
}

/*
 * rename(2) cannot cross file systems, so copy with everything preserved
 * and remove the original only after the whole copy has succeeded.
 */
static int
move_across(const char *source, const struct stat *st, const char *dest)
{
    struct cp_options options;
    struct stat dst;
    int status = EXIT_SUCCESS;

    if (lstat(dest, &dst) == 0) {
        if (S_ISDIR(dst.st_mode)) {
            if (!S_ISDIR(st->st_mode)) {
                xn_warnx("%s: is a directory", dest);
                return -1;
            }
            if (rmdir(dest) == -1) {
                xn_warn("%s", dest);
                return -1;
            }
        } else if (S_ISDIR(st->st_mode)) {
            xn_warnx("%s: not a directory", dest);
            return -1;
        } else if (unlink(dest) == -1) {
            xn_warn("%s", dest);
            return -1;
        }
    }

    memset(&options, 0, sizeof(options));
    options.force = 1;
    options.preserve = 1;
    options.recursive = 1;
    if (copy_operand(source, dest, &options) == -1) {
        xn_warnx("%s: not removed", source);
        return -1;
    }
    xn_walk(source, XN_WALK_POSTORDER, remove_cb, &status);
    return (status == EXIT_SUCCESS) ? 0 : -1;
}

static int
move_one(const char *source, const char *dest, int force, int interactive)
{
    struct stat st, dst;

    if (lstat(source, &st) == -1) {
        xn_warn("%s", source);
        return -1;
    }
    if (lstat(dest, &dst) == 0 && !force) {
        int ask = interactive || (!S_ISLNK(dst.st_mode) &&
                                  access(dest, W_OK) == -1 &&
                                  isatty(STDIN_FILENO));

        if (ask && !confirm(dest))
            return 0;
    }
    if (rename(source, dest) == 0)
        return 0;
    if (errno == EXDEV)
        return move_across(source, &st, dest);
    xn_warn("%s to %s", source, dest);
    return -1;
}

static void
mv_usage(void)
{
    (void)fprintf(stderr,
                  "usage: mv [-fi] source_file target_file\n"
                  "       mv [-fi] source_file... target_dir\n");
    exit(EXIT_FAILURE);
}

static int
mv_main(int argc, char **argv)
{
    int force = 0, interactive = 0;
    int status = EXIT_SUCCESS;
    int ch;

    while ((ch = xn_getopt(argc, argv, "fi")) != -1) {
        if (ch == 'f') {
            force = 1;
            interactive = 0;
        } else if (ch == 'i') {
            interactive = 1;
            force = 0;
        } else {
            mv_usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc < 2)
        mv_usage();

    if (is_directory(argv[argc - 1])) {
        for (int i = 0; i < argc - 1; i++) {
            char *dest = target_in_dir(argv[i], argv[argc - 1]);

            if (move_one(argv[i], dest, force, interactive) == -1)
                status = EXIT_FAILURE;
            free(dest);
        }
        return status;
    }
    if (argc != 2)
        xn_errx(EXIT_FAILURE, "%s: not a directory", argv[argc - 1]);
    return (move_one(argv[0], argv[1], force, interactive) == -1)
           ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int
link_one(const char *source, const char *dest, int force, int symbolic)
{
    struct stat st;

    if (force && lstat(dest, &st) == 0) {
        struct stat src;

        /* Unlinking a file onto itself would destroy the only name. */
        if (!symbolic && stat(source, &src) == 0 &&
            stat(dest, &st) == 0 && src.st_dev == st.st_dev &&
            src.st_ino == st.st_ino) {
            xn_warnx("%s and %s are identical", source, dest);
            return -1;
        }
        if (unlink(dest) == -1) {
            xn_warn("%s", dest);
            return -1;
        }
    }
    if ((symbolic ? symlink(source, dest) : link(source, dest)) == -1) {
        xn_warn("%s to %s", source, dest);
        return -1;
    }
    return 0;
}

static void
ln_usage(void)
{
    (void)fprintf(stderr,
                  "usage: ln [-fs] source_file target_file\n"
                  "       ln [-fs] source_file... target_dir\n");
    exit(EXIT_FAILURE);
}

static int
ln_main(int argc, char **argv)
{
    int force = 0, symbolic = 0;
    int status = EXIT_SUCCESS;
    int ch;

    while ((ch = xn_getopt(argc, argv, "fs")) != -1) {
        if (ch == 'f')
            force = 1;
        else if (ch == 's')
            symbolic = 1;
        else
            ln_usage();
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc < 2)
        ln_usage();

    if (is_directory(argv[argc - 1])) {
        for (int i = 0; i < argc - 1; i++) {
            char *dest = target_in_dir(argv[i], argv[argc - 1]);

            if (link_one(argv[i], dest, force, symbolic) == -1)
                status = EXIT_FAILURE;
            free(dest);
        }
        return status;
    }
    if (argc != 2)
        xn_errx(EXIT_FAILURE, "%s: not a directory", argv[argc - 1]);
    return (link_one(argv[0], argv[1], force, symbolic) == -1)
           ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* Dispatch on the name this binary was invoked under. */
int
main(int argc, char **argv)
{
    const char *name;

    xn_init(argv);
    name = xn_progname();
    if (strcmp(name, "cp") == 0)
        return cp_main(argc, argv);
    if (strcmp(name, "ln") == 0)
        return ln_main(argc, argv);
    if (strcmp(name, "mv") == 0)
        return mv_main(argc, argv);
    xn_errx(EXIT_FAILURE, "must be invoked as cp, ln or mv");
}
