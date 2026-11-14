/*
 * Belka platform layer. SPDX-License-Identifier: MIT
 *
 * Belka is a System V-style kernel: the mount table is /etc/mnttab in
 * the System V format, and a device number holds the major number in
 * its upper 32 bits and the minor number in its lower 32.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>         /* struct winsize, which mlibc keeps here */

#include "xn.h"

#define MNTTAB "/etc/mnttab"

/*
 * Enumerate mounted file systems in mount order, from /etc/mnttab: one
 * line each, "special mount-point type options time", separated by tabs
 * or spaces. Returns -1 if there is no table.
 */
int
xn_mounts(xn_mount_cb *cb, void *arg)
{
    FILE *fp = fopen(MNTTAB, "r");
    char *line = NULL;
    size_t cap = 0;

    if (fp == NULL)
        return -1;
    while (xn_getdelim(&line, &cap, '\n', fp) != -1) {
        struct xn_mount mount;
        char *special, *dir;

        special = line + strspn(line, " \t");
        if (*special == '\0' || *special == '\n' || *special == '#')
            continue;
        dir = special + strcspn(special, " \t\n");
        if (*dir == '\0' || *dir == '\n')
            continue;
        *dir++ = '\0';
        dir += strspn(dir, " \t");
        dir[strcspn(dir, " \t\n")] = '\0';
        if (*dir == '\0')
            continue;
        mount.fsname = special;
        mount.dir = dir;
        cb(&mount, arg);
    }
    free(line);
    (void)fclose(fp);
    return 0;
}

unsigned long
xn_dev_major(dev_t dev)
{
    return (unsigned long)(((uintmax_t)dev >> 32) & 0xffffffffu);
}

unsigned long
xn_dev_minor(dev_t dev)
{
    return (unsigned long)((uintmax_t)dev & 0xffffffffu);
}

/* Return the terminal's width in columns, or 0 when it is unknown. */
int
xn_term_width(int fd)
{
#ifdef TIOCGWINSZ
    struct winsize size;

    if (ioctl(fd, TIOCGWINSZ, &size) == 0)
        return size.ws_col;
#else
    (void)fd;
#endif
    return 0;
}
