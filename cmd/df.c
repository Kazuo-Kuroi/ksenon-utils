/* df - report free disk space. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include "xn.h"

struct locate {
    dev_t dev;
    char *fsname;
    char *dir;
};

static void locate_cb(const struct xn_mount *, void *);
static void list_cb(const struct xn_mount *, void *);
static int report(const char *, const char *, const char *, int);
static void header(void);
static void usage(void);

static unsigned long unit = 512;
static int totals_form;
static int status = EXIT_SUCCESS;

/* Later mounts shadow earlier ones, so the last match is the live one. */
static void
locate_cb(const struct xn_mount *mount, void *arg)
{
    struct locate *where = arg;
    struct stat st;

    if (stat(mount->dir, &st) == 0 && st.st_dev == where->dev) {
        free(where->fsname);
        free(where->dir);
        where->fsname = xn_strdup(mount->fsname);
        where->dir = xn_strdup(mount->dir);
    }
}

static void
list_cb(const struct xn_mount *mount, void *arg)
{
    (void)arg;
    (void)report(mount->fsname, mount->dir, mount->dir, 1);
}

/* Print one file system. quiet_empty hides pseudo file systems. */
static int
report(const char *fsname, const char *dir, const char *probe, int quiet_empty)
{
    struct statvfs vfs;
    uintmax_t frsize, total, used, avail, percent, usable;

    if (statvfs(probe, &vfs) == -1) {
        if (!quiet_empty) {
            xn_warn("%s", probe);
            status = EXIT_FAILURE;
        }
        return -1;
    }
    if (vfs.f_blocks == 0 && quiet_empty)
        return 0;

    frsize = (vfs.f_frsize != 0) ? vfs.f_frsize : vfs.f_bsize;
    total = (uintmax_t)vfs.f_blocks * frsize;
    used = (uintmax_t)(vfs.f_blocks - vfs.f_bfree) * frsize;
    avail = (uintmax_t)vfs.f_bavail * frsize;

    /* POSIX: the percentage is rounded up, of the space usable by users. */
    usable = used + avail;
    percent = (usable != 0) ? (used * 100 + usable - 1) / usable : 0;

    if (totals_form) {
        (void)printf("%-20s %12ju %12ju %12ju   %s\n", fsname,
                     total / unit, used / unit, avail / unit, dir);
    } else {
        (void)printf("%s %ju %ju %ju %ju%% %s\n", fsname, total / unit,
                     used / unit, avail / unit, percent, dir);
    }
    return 0;
}

static void
header(void)
{
    if (totals_form) {
        (void)printf("%-20s %12s %12s %12s   %s\n", "Filesystem", "Total",
                     "Used", "Available", "Mounted on");
    } else {
        (void)printf("Filesystem %lu-blocks Used Available Capacity "
                     "Mounted on\n", unit);
    }
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-k] [-P|-t] [file...]\n",
                  xn_progname());
    exit(EXIT_FAILURE);
}

/* Report space on the file systems holding the operands, or on them all. */
int
main(int argc, char **argv)
{
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "Pkt")) != -1) {
        switch (ch) {
        case 'P':
            totals_form = 0;
            break;
        case 'k':
            unit = 1024;
            break;
        case 't':
            totals_form = 1;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;

    header();
    if (argc == 0) {
        if (xn_mounts(list_cb, NULL) == -1)
            xn_err(EXIT_FAILURE, "cannot read the mount table");
        return xn_finish(status);
    }
    for (int i = 0; i < argc; i++) {
        struct locate where;
        struct stat st;

        if (stat(argv[i], &st) == -1) {
            xn_warn("%s", argv[i]);
            status = EXIT_FAILURE;
            continue;
        }
        where.dev = st.st_dev;
        where.fsname = NULL;
        where.dir = NULL;
        (void)xn_mounts(locate_cb, &where);
        (void)report((where.fsname != NULL) ? where.fsname : argv[i],
                     (where.dir != NULL) ? where.dir : argv[i], argv[i], 0);
        free(where.fsname);
        free(where.dir);
    }
    return xn_finish(status);
}
