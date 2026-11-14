/*
 * GNU/Linux platform layer. SPDX-License-Identifier: MIT
 *
 * These are facts about the kernel interface, not the C library, so the
 * file works unchanged with any libc on Linux.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "xn.h"

static void decode_octal(char *);

/* /proc/self/mounts encodes spaces and similar bytes as \ooo. */
static void
decode_octal(char *text)
{
    char *out = text;

    while (*text != '\0') {
        if (text[0] == '\\' && text[1] >= '0' && text[1] <= '3' &&
            text[2] >= '0' && text[2] <= '7' &&
            text[3] >= '0' && text[3] <= '7') {
            unsigned char byte;

            (void)xn_unescape_octal(text + 1, 3, &byte);
            *out++ = (char)byte;
            text += 4;
        } else {
            *out++ = *text++;
        }
    }
    *out = '\0';
}

/* Enumerate mounted file systems in mount order. Returns -1 on failure. */
int
xn_mounts(xn_mount_cb *cb, void *arg)
{
    FILE *fp = fopen("/proc/self/mounts", "r");
    char *line = NULL;
    size_t cap = 0;

    if (fp == NULL)
        return -1;
    while (xn_getdelim(&line, &cap, '\n', fp) != -1) {
        struct xn_mount mount;
        char *space = strchr(line, ' ');
        char *dir;

        if (space == NULL)
            continue;
        *space = '\0';
        dir = space + 1;
        if ((space = strchr(dir, ' ')) == NULL)
            continue;
        *space = '\0';
        decode_octal(line);
        decode_octal(dir);
        mount.fsname = line;
        mount.dir = dir;
        cb(&mount, arg);
    }
    free(line);
    (void)fclose(fp);
    return 0;
}

/* The bit layout below is the kernel's stable user-space dev_t encoding. */
unsigned long
xn_dev_major(dev_t dev)
{
    uintmax_t value = (uintmax_t)dev;

    return (unsigned long)(((value >> 8) & 0xfff) |
                           ((value >> 32) & ~(uintmax_t)0xfff));
}

unsigned long
xn_dev_minor(dev_t dev)
{
    uintmax_t value = (uintmax_t)dev;

    return (unsigned long)((value & 0xff) |
                           ((value >> 12) & ~(uintmax_t)0xff));
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
