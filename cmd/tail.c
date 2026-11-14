/* tail - copy the last part of a file. SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xn.h"

#define BUFFER_SIZE 65536
#define FOLLOW_INTERVAL 1

struct chunk {
    struct chunk *next;
    size_t len;
    size_t newlines;
    char data[BUFFER_SIZE];
};

static ssize_t read_retry(int, void *, size_t);
static void put_bytes(const char *, size_t);
static void copy_rest(int, const char *);
static size_t count_newlines(const char *, size_t);
static void skip_forward(int, const char *, uintmax_t, int);
static int seek_back_lines(int, off_t, uintmax_t);
static void tail_seekable(int, const char *, off_t, uintmax_t, int);
static void tail_stream(int, const char *, uintmax_t, int);
static void follow(int, const char *);
static void usage(void);

static ssize_t
read_retry(int fd, void *buf, size_t size)
{
    ssize_t got;

    do {
        got = read(fd, buf, size);
    } while (got == -1 && errno == EINTR);
    return got;
}

static void
put_bytes(const char *data, size_t len)
{
    if (xn_writeall(STDOUT_FILENO, data, len) == -1)
        xn_err(EXIT_FAILURE, "standard output");
}

static void
copy_rest(int fd, const char *name)
{
    int result = xn_copyfd(fd, STDOUT_FILENO);

    if (result == XN_COPY_READ)
        xn_err(EXIT_FAILURE, "%s", name);
    if (result == XN_COPY_WRITE)
        xn_err(EXIT_FAILURE, "standard output");
}

static size_t
count_newlines(const char *data, size_t len)
{
    size_t count = 0;

    for (const char *ptr = data; (size_t)(ptr - data) < len; ptr++) {
        ptr = memchr(ptr, '\n', len - (size_t)(ptr - data));
        if (ptr == NULL)
            break;
        count++;
    }
    return count;
}

/* Handle "+number": discard units before the requested starting point. */
static void
skip_forward(int fd, const char *name, uintmax_t skip, int by_lines)
{
    static char buf[BUFFER_SIZE];

    while (skip > 0) {
        ssize_t got = read_retry(fd, buf, sizeof(buf));
        size_t used = 0;

        if (got == 0)
            return;
        if (got == -1)
            xn_err(EXIT_FAILURE, "%s", name);
        if (by_lines) {
            while (used < (size_t)got && skip > 0) {
                if (buf[used++] == '\n')
                    skip--;
            }
        } else {
            used = (skip < (uintmax_t)got) ? (size_t)skip : (size_t)got;
            skip -= used;
        }
        if (used < (size_t)got)
            put_bytes(buf + used, (size_t)got - used);
    }
}

/*
 * Scan a seekable file backwards and leave the offset at the start of the
 * last count lines. A final line with no newline still counts as a line.
 */
static int
seek_back_lines(int fd, off_t size, uintmax_t count)
{
    static char buf[BUFFER_SIZE];
    off_t pos = size;
    int first = 1;

    while (pos > 0) {
        size_t step = (pos > BUFFER_SIZE) ? BUFFER_SIZE : (size_t)pos;
        ssize_t got;
        size_t i;

        pos -= (off_t)step;
        if (lseek(fd, pos, SEEK_SET) == (off_t)-1)
            return -1;
        if ((got = read_retry(fd, buf, step)) != (ssize_t)step)
            return -1;
        i = step;
        if (first) {
            first = 0;
            if (buf[step - 1] == '\n')
                i--;
        }
        while (i > 0) {
            if (buf[--i] == '\n' && --count == 0)
                return (lseek(fd, pos + (off_t)i + 1, SEEK_SET) == (off_t)-1)
                       ? -1 : 0;
        }
    }
    return (lseek(fd, 0, SEEK_SET) == (off_t)-1) ? -1 : 0;
}

static void
tail_seekable(int fd, const char *name, off_t size, uintmax_t count,
              int by_lines)
{
    if (count == 0) {
        if (lseek(fd, size, SEEK_SET) == (off_t)-1)
            xn_err(EXIT_FAILURE, "%s", name);
        return;
    }
    if (by_lines) {
        if (seek_back_lines(fd, size, count) == -1)
            xn_err(EXIT_FAILURE, "%s", name);
    } else {
        off_t start = (count >= (uintmax_t)size) ? 0 : size - (off_t)count;

        if (lseek(fd, start, SEEK_SET) == (off_t)-1)
            xn_err(EXIT_FAILURE, "%s", name);
    }
    copy_rest(fd, name);
}

/*
 * A pipe cannot be read backwards, so hold just enough recent chunks to
 * cover the request. Memory use depends on the request, not the input size.
 */
static void
tail_stream(int fd, const char *name, uintmax_t count, int by_lines)
{
    struct chunk *head = NULL, *last = NULL;
    uintmax_t held_bytes = 0, held_lines = 0;
    uintmax_t skip;
    int ends_in_newline = 1;

    for (;;) {
        struct chunk *block = xn_malloc(sizeof(*block));
        ssize_t got = read_retry(fd, block->data, sizeof(block->data));

        if (got == -1)
            xn_err(EXIT_FAILURE, "%s", name);
        if (got == 0) {
            free(block);
            break;
        }
        block->len = (size_t)got;
        block->newlines = count_newlines(block->data, block->len);
        block->next = NULL;
        if (last != NULL)
            last->next = block;
        else
            head = block;
        last = block;
        held_bytes += block->len;
        held_lines += block->newlines;
        ends_in_newline = (block->data[block->len - 1] == '\n');

        while (head != last) {
            int spare = by_lines
                ? (held_lines - head->newlines > count)
                : (held_bytes - head->len >= count);
            struct chunk *old = head;

            if (!spare)
                break;
            held_bytes -= old->len;
            held_lines -= old->newlines;
            head = old->next;
            free(old);
        }
    }

    if (by_lines) {
        uintmax_t lines = held_lines + (ends_in_newline ? 0 : 1);

        skip = (lines > count) ? lines - count : 0;
    } else {
        skip = (held_bytes > count) ? held_bytes - count : 0;
    }

    while (head != NULL) {
        struct chunk *block = head;
        size_t used = 0;

        if (by_lines) {
            while (skip > 0 && used < block->len) {
                if (block->data[used++] == '\n')
                    skip--;
            }
        } else if (skip > 0) {
            used = (skip < block->len) ? (size_t)skip : block->len;
            skip -= used;
        }
        if (used < block->len)
            put_bytes(block->data + used, block->len - used);
        head = block->next;
        free(block);
    }
}

/* Poll for appended data; a shrinking file restarts from its new end. */
static void
follow(int fd, const char *name)
{
    struct stat st;
    off_t pos = lseek(fd, 0, SEEK_CUR);

    for (;;) {
        copy_rest(fd, name);
        (void)sleep(FOLLOW_INTERVAL);
        if (pos != (off_t)-1 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
            pos = lseek(fd, 0, SEEK_CUR);
            if (st.st_size < pos)
                (void)lseek(fd, st.st_size, SEEK_SET);
        }
    }
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-f] [-c number|-n number] [file]\n",
                  xn_progname());
    exit(EXIT_FAILURE);
}

/* Copy the end of a file, or everything from a given point with "+". */
int
main(int argc, char **argv)
{
    const char *name = "standard input";
    const char *spec = NULL;
    uintmax_t count = 10;
    struct stat st;
    int by_lines = 1, from_start = 0, want_follow = 0;
    int fd = STDIN_FILENO;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "c:fn:")) != -1) {
        switch (ch) {
        case 'c':
            by_lines = 0;
            spec = xn_optarg;
            break;
        case 'f':
            want_follow = 1;
            break;
        case 'n':
            by_lines = 1;
            spec = xn_optarg;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc > 1)
        usage();

    if (spec != NULL) {
        if (spec[0] == '+') {
            from_start = 1;
            spec++;
        } else if (spec[0] == '-') {
            spec++;
        }
        if (xn_parse_uint(spec, &count) == -1)
            xn_errx(EXIT_FAILURE, "%s: invalid number", spec);
    }

    if (argc == 1 && strcmp(argv[0], "-") != 0) {
        name = argv[0];
        if ((fd = open(name, O_RDONLY)) == -1)
            xn_err(EXIT_FAILURE, "%s", name);
    }
    if (fstat(fd, &st) == -1)
        xn_err(EXIT_FAILURE, "%s", name);

    /* POSIX: -f has no effect when reading a pipe on standard input. */
    if (argc == 0 && S_ISFIFO(st.st_mode))
        want_follow = 0;

    if (from_start) {
        skip_forward(fd, name, (count > 0) ? count - 1 : 0, by_lines);
        copy_rest(fd, name);
    } else if (S_ISREG(st.st_mode)) {
        off_t size = lseek(fd, 0, SEEK_END);

        if (size == (off_t)-1)
            xn_err(EXIT_FAILURE, "%s", name);
        tail_seekable(fd, name, size, count, by_lines);
    } else {
        tail_stream(fd, name, count, by_lines);
    }

    if (want_follow)
        follow(fd, name);
    return EXIT_SUCCESS;
}
