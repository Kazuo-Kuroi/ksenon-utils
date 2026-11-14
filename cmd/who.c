/* who - display who is on the system. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>

#include "xn.h"

#define WANT_USER 0x01
#define WANT_BOOT 0x02
#define WANT_DEAD 0x04
#define WANT_LOGIN 0x08
#define WANT_INIT 0x10
#define WANT_RUNLEVEL 0x20
#define WANT_CLOCK 0x40
#define WANT_ALL 0x7f
#define ONE_DAY 86400

static int type_wanted(short, int);
static void put_record(const struct utmpx *);
static void usage(void);

static int show_state, show_idle;

static int
type_wanted(short type, int mask)
{
    switch (type) {
    case USER_PROCESS:
        return (mask & WANT_USER) != 0;
    case BOOT_TIME:
        return (mask & WANT_BOOT) != 0;
    case DEAD_PROCESS:
        return (mask & WANT_DEAD) != 0;
    case LOGIN_PROCESS:
        return (mask & WANT_LOGIN) != 0;
    case INIT_PROCESS:
        return (mask & WANT_INIT) != 0;
#ifdef RUN_LVL
    case RUN_LVL:
        return (mask & WANT_RUNLEVEL) != 0;
#endif
    case NEW_TIME:
    case OLD_TIME:
        return (mask & WANT_CLOCK) != 0;
    default:
        return 0;
    }
}

static void
put_record(const struct utmpx *record)
{
    char device[sizeof(record->ut_line) + 6];
    char line[sizeof(record->ut_line) + 1];
    char user[sizeof(record->ut_user) + 1];
    char date[64];
    struct stat st;
    time_t stamp = (time_t)record->ut_tv.tv_sec;
    time_t now = time(NULL);
    const struct tm *tm = localtime(&stamp);
    int state = '?';

    (void)snprintf(user, sizeof(user), "%.*s",
                   (int)sizeof(record->ut_user), record->ut_user);
    (void)snprintf(line, sizeof(line), "%.*s",
                   (int)sizeof(record->ut_line), record->ut_line);
    (void)snprintf(device, sizeof(device), "/dev/%s", line);

    date[0] = '\0';
    if (tm != NULL)
        (void)strftime(date, sizeof(date), "%b %e %H:%M", tm);

    (void)printf("%-8s ", user);
    if (show_state) {
        int have = (stat(device, &st) == 0);

        if (have)
            state = ((st.st_mode & S_IWGRP) != 0) ? '+' : '-';
        (void)printf("%c ", state);
    }
    (void)printf("%-12s %s", line, date);
    if (show_idle) {
        if (stat(device, &st) == -1) {
            (void)fputs("   ?", stdout);
        } else if (now - st.st_atime < 60) {
            (void)fputs("   .", stdout);
        } else if (now - st.st_atime >= ONE_DAY) {
            (void)fputs(" old", stdout);
        } else {
            long minutes = (long)((now - st.st_atime) / 60);

            (void)printf(" %02ld:%02ld", minutes / 60, minutes % 60);
        }
        (void)printf(" %8ld", (long)record->ut_pid);
    }
    (void)putchar('\n');
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-abdHlmpqrsTtu] [am i]\n",
                  xn_progname());
    exit(EXIT_FAILURE);
}

/* List the utmpx records selected by the options. */
int
main(int argc, char **argv)
{
    const struct utmpx *record;
    const char *own_line = NULL;
    int mask = 0;
    int headings = 0, quick = 0;
    uintmax_t users = 0;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "abdHlmpqrsTtu")) != -1) {
        switch (ch) {
        case 'a':
            mask = WANT_ALL;
            show_state = show_idle = 1;
            break;
        case 'b':
            mask |= WANT_BOOT;
            break;
        case 'd':
            mask |= WANT_DEAD;
            break;
        case 'H':
            headings = 1;
            break;
        case 'l':
            mask |= WANT_LOGIN;
            break;
        case 'm':
            own_line = "";
            break;
        case 'p':
            mask |= WANT_INIT;
            break;
        case 'q':
            quick = 1;
            break;
        case 'r':
            mask |= WANT_RUNLEVEL;
            break;
        case 's':
            show_idle = 0;
            mask |= WANT_USER;
            break;
        case 'T':
            show_state = 1;
            break;
        case 't':
            mask |= WANT_CLOCK;
            break;
        case 'u':
            show_idle = 1;
            mask |= WANT_USER;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;

    /* "who am i" is the historical spelling of -m. */
    if (argc == 2)
        own_line = "";
    else if (argc != 0)
        usage();
    if (mask == 0)
        mask = WANT_USER;

    if (own_line != NULL) {
        const char *name = ttyname(STDIN_FILENO);

        if (name == NULL)
            return xn_finish(EXIT_SUCCESS);
        if (strncmp(name, "/dev/", 5) == 0)
            name += 5;
        own_line = name;
    }

    if (headings && !quick) {
        (void)printf("%-8s ", "NAME");
        if (show_state)
            (void)fputs("S ", stdout);
        (void)printf("%-12s %s", "LINE", "TIME");
        if (show_idle)
            (void)fputs("         IDLE      PID", stdout);
        (void)putchar('\n');
    }

    setutxent();
    while ((record = getutxent()) != NULL) {
        if (!type_wanted(record->ut_type, quick ? WANT_USER : mask))
            continue;
        if (own_line != NULL &&
            strncmp(record->ut_line, own_line, sizeof(record->ut_line)) != 0)
            continue;
        if (quick) {
            (void)printf("%s%.*s", (users > 0) ? " " : "",
                         (int)sizeof(record->ut_user), record->ut_user);
            users++;
        } else {
            put_record(record);
        }
    }
    endutxent();
    if (quick)
        (void)printf("%s# users=%ju\n", (users > 0) ? "\n" : "", users);
    return xn_finish(EXIT_SUCCESS);
}
