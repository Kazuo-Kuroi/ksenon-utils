/* date - write the date and time. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xn.h"

#define FORMAT_DEFAULT "%a %b %e %H:%M:%S %Z %Y"
#define OUTPUT_LIMIT (1024U * 1024U)

static int two_digits(const char *);
static void set_clock(const char *);
static void show(const char *, const struct tm *);
static void usage(void);

static int
two_digits(const char *text)
{
    return (text[0] - '0') * 10 + (text[1] - '0');
}

/* Apply a mmddhhmm[[cc]yy] operand to the system clock. */
static void
set_clock(const char *text)
{
    size_t len = strlen(text);
    struct tm tm;
    struct timespec value;
    time_t now = time(NULL);
    int month, day;

    if (len != 8 && len != 10 && len != 12)
        usage();
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9')
            usage();
    }

    tm = *localtime(&now);
    month = two_digits(text);
    day = two_digits(text + 2);
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = two_digits(text + 4);
    tm.tm_min = two_digits(text + 6);
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    if (len == 12) {
        tm.tm_year = two_digits(text + 8) * 100 + two_digits(text + 10) - 1900;
    } else if (len == 10) {
        int year = two_digits(text + 8);

        tm.tm_year = (year >= 69) ? year : year + 100;
    }
    value.tv_sec = mktime(&tm);
    value.tv_nsec = 0;
    if (value.tv_sec == (time_t)-1 || tm.tm_mon != month - 1 ||
        tm.tm_mday != day)
        xn_errx(EXIT_FAILURE, "%s: invalid date", text);
    if (clock_settime(CLOCK_REALTIME, &value) == -1)
        xn_err(EXIT_FAILURE, "cannot set the clock");
}

/*
 * Format the time. A leading sentinel keeps the result non-empty, because
 * strftime's zero return cannot otherwise tell an empty result from a full
 * buffer.
 */
static void
show(const char *format, const struct tm *tm)
{
    size_t len = strlen(format);
    char *marked = xn_malloc(len + 2);
    size_t cap = len + 256;
    char *out = NULL;

    marked[0] = 'x';
    memcpy(marked + 1, format, len + 1);
    for (;;) {
        out = xn_realloc(out, cap);
        if (strftime(out, cap, marked, tm) != 0)
            break;
        if (cap > OUTPUT_LIMIT)
            xn_errx(EXIT_FAILURE, "output too long");
        cap *= 2;
    }
    (void)puts(out + 1);
    free(out);
    free(marked);
}

static void
usage(void)
{
    (void)fprintf(stderr,
                  "usage: %s [-u] [+format]\n"
                  "       %s [-u] mmddhhmm[[cc]yy]\n",
                  xn_progname(), xn_progname());
    exit(EXIT_FAILURE);
}

/* Write the current time, or set the clock from a date operand. */
int
main(int argc, char **argv)
{
    const char *format = FORMAT_DEFAULT;
    const struct tm *tm;
    time_t now;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "u")) != -1) {
        if (ch != 'u')
            usage();
        if (setenv("TZ", "UTC0", 1) == -1)
            xn_err(EXIT_FAILURE, "cannot select UTC");
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc > 1)
        usage();
    tzset();

    if (argc == 1) {
        if (argv[0][0] == '+')
            format = argv[0] + 1;
        else
            set_clock(argv[0]);
    }

    now = time(NULL);
    if ((tm = localtime(&now)) == NULL)
        xn_err(EXIT_FAILURE, "cannot convert the time");
    show(format, tm);
    return xn_finish(EXIT_SUCCESS);
}
