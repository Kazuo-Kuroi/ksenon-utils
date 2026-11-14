/*
 * Xenon Utils shim library. SPDX-License-Identifier: MIT
 *
 * This is deliberately small: it holds only what POSIX-2001 does not
 * provide, or what several utilities would otherwise duplicate.
 */
#ifndef XN_H
#define XN_H

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(__GNUC__)
#define XN_NORETURN __attribute__((noreturn))
#define XN_PRINTF(fmt, first) __attribute__((format(printf, fmt, first)))
#else
#define XN_NORETURN
#define XN_PRINTF(fmt, first)
#endif

/* Strict POSIX option parser: stops at the first operand, never permutes. */
extern char *xn_optarg;
extern int xn_optind;
extern int xn_opterr;
extern int xn_optopt;
int xn_getopt(int, char *const *, const char *);

/* Program setup and diagnostics. */
void xn_init(char **);
const char *xn_progname(void);
void xn_warn(const char *, ...) XN_PRINTF(1, 2);
void xn_warnx(const char *, ...) XN_PRINTF(1, 2);
void xn_err(int, const char *, ...) XN_NORETURN XN_PRINTF(2, 3);
void xn_errx(int, const char *, ...) XN_NORETURN XN_PRINTF(2, 3);
int xn_finish(int);

/* Allocation helpers; these exit with status 1 when memory runs out. */
void *xn_malloc(size_t);
void *xn_realloc(void *, size_t);
char *xn_strdup(const char *);
char *xn_pathjoin(const char *, const char *);

/* Input and output. */
ssize_t xn_getdelim(char **, size_t *, int, FILE *);
int xn_writeall(int, const void *, size_t);
int xn_copyfd(int, int);
int xn_affirm(void);
const char *xn_base(const char *);

#define XN_COPY_OK 0
#define XN_COPY_READ 1
#define XN_COPY_WRITE 2

/* Numeric operands: the whole string must convert, or -1 is returned. */
int xn_parse_uint(const char *, uintmax_t *);
int xn_parse_int(const char *, intmax_t *);

/* Backslash escapes shared by echo and printf. */
size_t xn_unescape_octal(const char *, int, unsigned char *);

/* Compiled chmod(1) mode expressions. */
struct xn_mode;
struct xn_mode *xn_mode_compile(const char *);
mode_t xn_mode_apply(const struct xn_mode *, mode_t, mode_t);
mode_t xn_umask(void);

/*
 * Reversible change of working directory. Trees deeper than PATH_MAX can
 * only be reached by moving through them, because POSIX-2001 has no openat.
 */
struct xn_dirstep {
    char *path;
    int fd;
    dev_t dev;
    ino_t ino;
};

int xn_dir_enter(const char *, const struct stat *, struct xn_dirstep *);
void xn_dir_leave(struct xn_dirstep *);

/*
 * Depth-first file hierarchy traversal. A callback runs with the working
 * directory set to the one holding the entry: it must use access for system
 * calls, keep path for messages, and leave the working directory as found.
 */
#define XN_WALK_LOGICAL 0x01
#define XN_WALK_COMFOLLOW 0x02
#define XN_WALK_POSTORDER 0x04
#define XN_WALK_XDEV 0x08
#define XN_WALK_SKIP 1

enum xn_walk_type {
    XN_WALK_FILE,
    XN_WALK_DIR,
    XN_WALK_DIRPOST,
    XN_WALK_DIRERR,
    XN_WALK_STATERR,
    XN_WALK_CYCLE
};

struct xn_walk_ent {
    const char *path;
    const char *access;
    const struct stat *st;
    enum xn_walk_type type;
    int depth;
    int err;
};

typedef int xn_walk_cb(const struct xn_walk_ent *, void *);
void xn_walk(const char *, int, xn_walk_cb *, void *);

/* Platform layer, implemented once per target in lib/plat_*.c. */
struct xn_mount {
    const char *fsname;
    const char *dir;
};

typedef void xn_mount_cb(const struct xn_mount *, void *);
int xn_mounts(xn_mount_cb *, void *);
unsigned long xn_dev_major(dev_t);
unsigned long xn_dev_minor(dev_t);
int xn_term_width(int);

#endif /* XN_H */
