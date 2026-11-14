/* touch - change file access and modification times.
 * SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "xn.h"

#define CREATE_MODE 0666

static int all_digits(const char *, size_t);
static int two_digits(const char *);
static time_t build_time(int, int, int, int, int, int);
static time_t parse_stamp(const char *);
static time_t parse_legacy(const char *);
static void usage(void);

static int
all_digits(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9')
            return 0;
    }
    return 1;
}

static int
two_digits(const char *text)
{
    return (text[0] - '0') * 10 + (text[1] - '0');
}

/* Convert broken-down local time, rejecting values mktime would normalise. */
static time_t
build_time(int year, int month, int day, int hour, int minute, int second)
{
    struct tm tm;
    time_t value;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;
    value = mktime(&tm);
    if (value == (time_t)-1 || tm.tm_mon != month - 1 || tm.tm_mday != day)
        xn_errx(EXIT_FAILURE, "invalid date");
    return value;
}

/* Parse the -t form: [[CC]YY]MMDDhhmm[.SS]. */
static time_t
parse_stamp(const char *text)
{
    const char *dot = strchr(text, '.');
    size_t len = (dot != NULL) ? (size_t)(dot - text) : strlen(text);
    struct tm now;
    time_t clock_now = time(NULL);
    int year;
    int second = 0;

    if (!all_digits(text, len) ||
        (len != 8 && len != 10 && len != 12))
        xn_errx(EXIT_FAILURE, "%s: invalid time", text);
    if (dot != NULL) {
        if (strlen(dot + 1) != 2 || !all_digits(dot + 1, 2))
            xn_errx(EXIT_FAILURE, "%s: invalid time", text);
        second = two_digits(dot + 1);
    }

    now = *localtime(&clock_now);
    if (len == 12) {
        year = two_digits(text) * 100 + two_digits(text + 2);
        text += 4;
    } else if (len == 10) {
        year = two_digits(text);
        year += (year >= 69) ? 1900 : 2000;
        text += 2;
    } else {
        year = now.tm_year + 1900;
    }
    return build_time(year, two_digits(text), two_digits(text + 2),
                      two_digits(text + 4), two_digits(text + 6), second);
}

/* Parse the obsolescent operand form: MMDDhhmm[YY]. */
static time_t
parse_legacy(const char *text)
{
    struct tm now;
    time_t clock_now = time(NULL);
    size_t len = strlen(text);
    int year;

    now = *localtime(&clock_now);
    year = now.tm_year + 1900;
    if (len == 10) {
        year = two_digits(text + 8);
        year += (year >= 69) ? 1900 : 2000;
    }
    return build_time(year, two_digits(text), two_digits(text + 2),
                      two_digits(text + 4), two_digits(text + 6), 0);
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-acm] [-r ref_file|-t time] file...\n",
                  xn_progname());
    exit(EXIT_FAILURE);
}

/* Update the times on each operand, creating files unless -c is given. */
int
main(int argc, char **argv)
{
    const char *ref = NULL;
    const char *stamp = NULL;
    time_t atime = 0, mtime = 0;
    int use_time = 0;
    int only_a = 0, only_m = 0, nocreate = 0;
    int status = EXIT_SUCCESS;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "acmr:t:")) != -1) {
        switch (ch) {
        case 'a':
            only_a = 1;
            break;
        case 'c':
            nocreate = 1;
            break;
        case 'm':
            only_m = 1;
            break;
        case 'r':
            ref = xn_optarg;
            break;
        case 't':
            stamp = xn_optarg;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (ref != NULL && stamp != NULL)
        usage();

    if (ref != NULL) {
        struct stat st;

        if (stat(ref, &st) == -1)
            xn_err(EXIT_FAILURE, "%s", ref);
        atime = st.st_atime;
        mtime = st.st_mtime;
        use_time = 1;
    } else if (stamp != NULL) {
        atime = mtime = parse_stamp(stamp);
        use_time = 1;
    } else if (argc >= 2 && all_digits(argv[0], strlen(argv[0])) &&
               (strlen(argv[0]) == 8 || strlen(argv[0]) == 10)) {
        atime = mtime = parse_legacy(argv[0]);
        use_time = 1;
        argc--;
        argv++;
    }
    if (argc == 0)
        usage();
    if (!only_a && !only_m)
        only_a = only_m = 1;

    for (int i = 0; i < argc; i++) {
        struct timeval times[2];
        struct stat st;
        int plain_now = (!use_time && only_a && only_m);

        if (stat(argv[i], &st) == -1) {
            int fd;

            if (errno != ENOENT) {
                xn_warn("%s", argv[i]);
                status = EXIT_FAILURE;
                continue;
            }
            if (nocreate)
                continue;
            fd = open(argv[i], O_WRONLY | O_CREAT, CREATE_MODE);
            if (fd == -1 || fstat(fd, &st) == -1) {
                xn_warn("%s", argv[i]);
                status = EXIT_FAILURE;
                continue;
            }
            (void)close(fd);
            if (plain_now)
                continue;
        }

        /* NULL needs only write access; explicit times need ownership. */
        if (plain_now) {
            if (utimes(argv[i], NULL) == -1) {
                xn_warn("%s", argv[i]);
                status = EXIT_FAILURE;
            }
            continue;
        }
        times[0].tv_usec = 0;
        times[1].tv_usec = 0;
        {
            time_t now = time(NULL);

            times[0].tv_sec = only_a ? (use_time ? atime : now) : st.st_atime;
            times[1].tv_sec = only_m ? (use_time ? mtime : now) : st.st_mtime;
        }
        if (utimes(argv[i], times) == -1) {
            xn_warn("%s", argv[i]);
            status = EXIT_FAILURE;
        }
    }
    return status;
}
