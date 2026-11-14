/*
 * Diagnostics, memory, I/O and parsing helpers.
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <inttypes.h>
#include <langinfo.h>
#include <locale.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xn.h"

#define COPY_BUFSIZE 65536

#if defined(_POSIX_THREAD_SAFE_FUNCTIONS) && _POSIX_THREAD_SAFE_FUNCTIONS > 0
#define XN_GETC(fp) getc_unlocked(fp)
#else
#define XN_GETC(fp) getc(fp)
#endif

static void message(int, const char *, va_list);

static const char *progname = "xenon";

/* Record the invocation name and adopt the environment's locale. */
void
xn_init(char **argv)
{
    if (argv[0] != NULL && argv[0][0] != '\0')
        progname = xn_base(argv[0]);
    (void)setlocale(LC_ALL, "");
}

const char *
xn_progname(void)
{
    return progname;
}

/* Return the final path component without modifying the string. */
const char *
xn_base(const char *path)
{
    const char *slash = strrchr(path, '/');

    return (slash != NULL) ? slash + 1 : path;
}

static void
message(int err, const char *fmt, va_list args)
{
    (void)fflush(stdout);
    (void)fprintf(stderr, "%s: ", progname);
    (void)vfprintf(stderr, fmt, args);
    if (err != 0)
        (void)fprintf(stderr, ": %s", strerror(err));
    (void)fputc('\n', stderr);
}

void
xn_warn(const char *fmt, ...)
{
    int err = errno;
    va_list args;

    va_start(args, fmt);
    message(err, fmt, args);
    va_end(args);
}

void
xn_warnx(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    message(0, fmt, args);
    va_end(args);
}

void
xn_err(int status, const char *fmt, ...)
{
    int err = errno;
    va_list args;

    va_start(args, fmt);
    message(err, fmt, args);
    va_end(args);
    exit(status);
}

void
xn_errx(int status, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    message(0, fmt, args);
    va_end(args);
    exit(status);
}

/*
 * Flush standard output and fold any failure into the exit status, so a
 * full disk or closed pipe is never reported as success.
 */
int
xn_finish(int status)
{
    if (fflush(stdout) == EOF || ferror(stdout)) {
        xn_warn("standard output");
        return (status != EXIT_SUCCESS) ? status : EXIT_FAILURE;
    }
    return status;
}

void *
xn_malloc(size_t size)
{
    void *ptr = malloc((size != 0) ? size : 1);

    if (ptr == NULL)
        xn_err(EXIT_FAILURE, "out of memory");
    return ptr;
}

void *
xn_realloc(void *old, size_t size)
{
    void *ptr = realloc(old, (size != 0) ? size : 1);

    if (ptr == NULL)
        xn_err(EXIT_FAILURE, "out of memory");
    return ptr;
}

char *
xn_strdup(const char *text)
{
    size_t size = strlen(text) + 1;

    return memcpy(xn_malloc(size), text, size);
}

/* Join a directory and a name without doubling the separator. */
char *
xn_pathjoin(const char *dir, const char *name)
{
    size_t dirlen = strlen(dir);
    size_t namelen = strlen(name);
    int slash = (dirlen > 0 && dir[dirlen - 1] != '/');
    char *path = xn_malloc(dirlen + (size_t)slash + namelen + 1);

    memcpy(path, dir, dirlen);
    if (slash)
        path[dirlen] = '/';
    memcpy(path + dirlen + (size_t)slash, name, namelen + 1);
    return path;
}

/*
 * Read one record, keeping the delimiter. getdelim(3) only arrived in
 * POSIX-2008, so it cannot be assumed here. Returns -1 at end of input
 * or on error; the stream's error indicator tells them apart.
 */
ssize_t
xn_getdelim(char **bufp, size_t *capp, int delim, FILE *fp)
{
    size_t len = 0;
    int ch;

    while ((ch = XN_GETC(fp)) != EOF) {
        if (len + 2 > *capp) {
            *capp = (*capp != 0) ? *capp * 2 : 256;
            *bufp = xn_realloc(*bufp, *capp);
        }
        (*bufp)[len++] = (char)ch;
        if (ch == delim)
            break;
    }
    if (len == 0)
        return -1;
    (*bufp)[len] = '\0';
    return (ssize_t)len;
}

/* Write the whole buffer, resuming after partial writes and signals. */
int
xn_writeall(int fd, const void *data, size_t len)
{
    const char *ptr = data;

    while (len > 0) {
        ssize_t done = write(fd, ptr, len);

        if (done == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        ptr += done;
        len -= (size_t)done;
    }
    return 0;
}

/* Copy until end of input. Reports which side failed via the result. */
int
xn_copyfd(int in, int out)
{
    static char buf[COPY_BUFSIZE];

    for (;;) {
        ssize_t got = read(in, buf, sizeof(buf));

        if (got == 0)
            return XN_COPY_OK;
        if (got == -1) {
            if (errno == EINTR)
                continue;
            return XN_COPY_READ;
        }
        if (xn_writeall(out, buf, (size_t)got) == -1)
            return XN_COPY_WRITE;
    }
}

/* Read a reply from standard input and match the locale's "yes". */
int
xn_affirm(void)
{
    char *line = NULL;
    size_t cap = 0;
    regex_t yes;
    int result = 0;

    (void)fflush(stdout);
    (void)fflush(stderr);
    if (xn_getdelim(&line, &cap, '\n', stdin) == -1) {
        free(line);
        return 0;
    }
    if (regcomp(&yes, nl_langinfo(YESEXPR), REG_EXTENDED | REG_NOSUB) == 0) {
        result = (regexec(&yes, line, 0, NULL, 0) == 0);
        regfree(&yes);
    } else {
        result = (line[0] == 'y' || line[0] == 'Y');
    }
    free(line);
    return result;
}

int
xn_parse_uint(const char *text, uintmax_t *value)
{
    char *end;

    if (*text == '\0' || *text == '-')
        return -1;
    errno = 0;
    *value = strtoumax(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE)
        return -1;
    return 0;
}

int
xn_parse_int(const char *text, intmax_t *value)
{
    char *end;

    errno = 0;
    *value = strtoimax(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE)
        return -1;
    return 0;
}

/*
 * Convert up to maxdigits octal digits into one byte, returning the count
 * of digits consumed.
 */
size_t
xn_unescape_octal(const char *text, int maxdigits, unsigned char *out)
{
    unsigned int value = 0;
    size_t used = 0;

    while ((int)used < maxdigits && text[used] >= '0' && text[used] <= '7')
        value = value * 8 + (unsigned int)(text[used++] - '0');
    *out = (unsigned char)value;
    return used;
}
