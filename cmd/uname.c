/* uname - return system name. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <sys/utsname.h>

#include "xn.h"

#define SHOW_SYSNAME 0x01
#define SHOW_NODE 0x02
#define SHOW_RELEASE 0x04
#define SHOW_VERSION 0x08
#define SHOW_MACHINE 0x10
#define SHOW_ALL 0x1f

static void show(const char *);
static void usage(void);

static int first = 1;

static void
show(const char *field)
{
    (void)printf("%s%s", first ? "" : " ", field);
    first = 0;
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-amnrsv]\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Print the requested system identification fields in the standard order. */
int
main(int argc, char **argv)
{
    struct utsname name;
    int mask = 0;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "amnrsv")) != -1) {
        switch (ch) {
        case 'a':
            mask |= SHOW_ALL;
            break;
        case 'm':
            mask |= SHOW_MACHINE;
            break;
        case 'n':
            mask |= SHOW_NODE;
            break;
        case 'r':
            mask |= SHOW_RELEASE;
            break;
        case 's':
            mask |= SHOW_SYSNAME;
            break;
        case 'v':
            mask |= SHOW_VERSION;
            break;
        default:
            usage();
        }
    }
    if (xn_optind != argc)
        usage();
    if (mask == 0)
        mask = SHOW_SYSNAME;
    if (uname(&name) == -1)
        xn_err(EXIT_FAILURE, "uname");

    if ((mask & SHOW_SYSNAME) != 0)
        show(name.sysname);
    if ((mask & SHOW_NODE) != 0)
        show(name.nodename);
    if ((mask & SHOW_RELEASE) != 0)
        show(name.release);
    if ((mask & SHOW_VERSION) != 0)
        show(name.version);
    if ((mask & SHOW_MACHINE) != 0)
        show(name.machine);
    (void)putchar('\n');
    return xn_finish(EXIT_SUCCESS);
}
