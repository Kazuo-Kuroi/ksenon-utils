/*
 * tr - translate characters. SPDX-License-Identifier: MIT
 *
 * This operates on bytes, which is exact for single-byte locales; see the
 * README for the multibyte limitation.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xn.h"

#define BYTE_VALUES 256
#define BUFFER_SIZE 65536
#define REPEAT_LIMIT (1U << 24)

struct byte_set {
    unsigned char *bytes;
    size_t len;
    size_t cap;
    size_t fill_at;
    int fill_byte;
    int has_fill;
};

static void push(struct byte_set *, int);
static int read_escape(const char **);
static int read_char(const char **);
static int class_member(const char *, size_t, int);
static int parse_bracket(struct byte_set *, const char **, int);
static void parse_set(struct byte_set *, const char *, int);
static void apply_fill(struct byte_set *, size_t);
static void build_complement(struct byte_set *);
static void usage(void);

static void
push(struct byte_set *set, int byte)
{
    if (set->len == set->cap) {
        set->cap = (set->cap != 0) ? set->cap * 2 : 256;
        set->bytes = xn_realloc(set->bytes, set->cap);
    }
    set->bytes[set->len++] = (unsigned char)byte;
}

/* Decode the escape at *textp, which points just past the backslash. */
static int
read_escape(const char **textp)
{
    static const char names[] = "\\abfnrtv";
    static const char codes[] = "\\\a\b\f\n\r\t\v";
    const char *text = *textp;
    const char *match;
    unsigned char byte;
    size_t used;

    if (*text == '\0')
        return '\\';
    if ((used = xn_unescape_octal(text, 3, &byte)) > 0) {
        *textp = text + used;
        return byte;
    }
    *textp = text + 1;
    if ((match = strchr(names, *text)) != NULL)
        return (unsigned char)codes[match - names];
    return (unsigned char)*text;
}

static int
read_char(const char **textp)
{
    const char *text = *textp;

    *textp = text + 1;
    if (*text == '\\')
        return read_escape(textp);
    return (unsigned char)*text;
}

static int
class_member(const char *name, size_t len, int byte)
{
    static const struct {
        const char *name;
        int (*test)(int);
    } classes[] = {
        { "alnum", isalnum }, { "alpha", isalpha }, { "blank", isblank },
        { "cntrl", iscntrl }, { "digit", isdigit }, { "graph", isgraph },
        { "lower", islower }, { "print", isprint }, { "punct", ispunct },
        { "space", isspace }, { "upper", isupper }, { "xdigit", isxdigit }
    };

    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
        if (strlen(classes[i].name) == len &&
            strncmp(classes[i].name, name, len) == 0)
            return classes[i].test(byte) != 0;
    }
    xn_errx(EXIT_FAILURE, "%.*s: unknown character class", (int)len, name);
}

/*
 * Handle the bracketed forms [:class:], [=equiv=] and [x*n] at *textp.
 * Returns 0, leaving *textp alone, if this is just a literal '['.
 */
static int
parse_bracket(struct byte_set *set, const char **textp, int is_second)
{
    const char *text = *textp;

    if (text[1] == ':' || text[1] == '=') {
        char close_mark[3];
        const char *end;

        close_mark[0] = text[1];
        close_mark[1] = ']';
        close_mark[2] = '\0';
        if ((end = strstr(text + 2, close_mark)) == NULL)
            return 0;
        if (text[1] == ':') {
            size_t len = (size_t)(end - (text + 2));

            for (int byte = 0; byte < BYTE_VALUES; byte++) {
                if (class_member(text + 2, len, byte))
                    push(set, byte);
            }
        } else {
            const char *ptr = text + 2;

            if (ptr == end)
                return 0;
            push(set, read_char(&ptr));
            if (ptr != end)
                xn_errx(EXIT_FAILURE, "invalid equivalence class");
        }
        *textp = end + 2;
        return 1;
    }

    {
        const char *ptr = text + 1;
        int byte;
        const char *digits;
        char *end;
        unsigned long count;

        if (*ptr == '\0')
            return 0;
        byte = read_char(&ptr);
        if (*ptr != '*')
            return 0;
        digits = ptr + 1;
        if (*digits == ']') {
            if (!is_second)
                xn_errx(EXIT_FAILURE, "[x*] is only valid in string2");
            if (set->has_fill)
                xn_errx(EXIT_FAILURE, "only one [x*] is allowed");
            set->has_fill = 1;
            set->fill_at = set->len;
            set->fill_byte = byte;
            *textp = digits + 1;
            return 1;
        }
        if (*digits < '0' || *digits > '9')
            return 0;
        errno = 0;
        count = strtoul(digits, &end, (*digits == '0') ? 8 : 10);
        if (*end != ']' || errno == ERANGE || count > REPEAT_LIMIT)
            xn_errx(EXIT_FAILURE, "invalid repeat count");
        if (count == 0) {
            if (!is_second)
                xn_errx(EXIT_FAILURE, "[x*n] is only valid in string2");
            if (set->has_fill)
                xn_errx(EXIT_FAILURE, "only one [x*] is allowed");
            set->has_fill = 1;
            set->fill_at = set->len;
            set->fill_byte = byte;
        }
        for (unsigned long i = 0; i < count; i++)
            push(set, byte);
        *textp = end + 1;
        return 1;
    }
}

static void
parse_set(struct byte_set *set, const char *text, int is_second)
{
    while (*text != '\0') {
        int low;

        if (*text == '[' && parse_bracket(set, &text, is_second))
            continue;
        low = read_char(&text);
        if (text[0] == '-' && text[1] != '\0') {
            int high;

            text++;
            high = read_char(&text);
            if (high < low)
                xn_errx(EXIT_FAILURE, "range-endpoints out of order");
            for (int byte = low; byte <= high; byte++)
                push(set, byte);
        } else {
            push(set, low);
        }
    }
}

/* Expand a [x*] in string2 so that it matches the length of string1. */
static void
apply_fill(struct byte_set *set, size_t target)
{
    size_t extra;
    size_t tail;

    if (!set->has_fill || set->len >= target)
        return;
    extra = target - set->len;
    tail = set->len - set->fill_at;
    for (size_t i = 0; i < extra; i++)
        push(set, set->fill_byte);
    memmove(set->bytes + set->fill_at + extra, set->bytes + set->fill_at,
            tail);
    memset(set->bytes + set->fill_at, set->fill_byte, extra);
}

/* Replace a set with its complement, in ascending byte order. */
static void
build_complement(struct byte_set *set)
{
    char member[BYTE_VALUES];
    struct byte_set result;

    memset(member, 0, sizeof(member));
    for (size_t i = 0; i < set->len; i++)
        member[set->bytes[i]] = 1;
    memset(&result, 0, sizeof(result));
    for (int byte = 0; byte < BYTE_VALUES; byte++) {
        if (!member[byte])
            push(&result, byte);
    }
    free(set->bytes);
    *set = result;
}

static void
usage(void)
{
    (void)fprintf(stderr,
                  "usage: %s [-c|-C] [-s] string1 string2\n"
                  "       %s -s [-c|-C] string1\n"
                  "       %s -d [-c|-C] string1\n"
                  "       %s -ds [-c|-C] string1 string2\n",
                  xn_progname(), xn_progname(), xn_progname(), xn_progname());
    exit(EXIT_FAILURE);
}

/* Filter standard input to standard output through the byte tables. */
int
main(int argc, char **argv)
{
    static unsigned char in[BUFFER_SIZE], out[BUFFER_SIZE];
    struct byte_set first, second;
    unsigned char map[BYTE_VALUES];
    char drop[BYTE_VALUES], squeeze[BYTE_VALUES];
    int complement = 0, delete_mode = 0, squeeze_mode = 0;
    int last = -1;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "Ccds")) != -1) {
        switch (ch) {
        case 'C':
        case 'c':
            complement = 1;
            break;
        case 'd':
            delete_mode = 1;
            break;
        case 's':
            squeeze_mode = 1;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc < 1 || argc > 2 ||
        (delete_mode && squeeze_mode && argc != 2) ||
        (delete_mode && !squeeze_mode && argc != 1) ||
        (!delete_mode && !squeeze_mode && argc != 2))
        usage();

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    parse_set(&first, argv[0], 0);
    if (argc == 2)
        parse_set(&second, argv[1], 1);
    if (complement)
        build_complement(&first);
    apply_fill(&second, first.len);

    for (int byte = 0; byte < BYTE_VALUES; byte++)
        map[byte] = (unsigned char)byte;
    memset(drop, 0, sizeof(drop));
    memset(squeeze, 0, sizeof(squeeze));

    if (delete_mode) {
        for (size_t i = 0; i < first.len; i++)
            drop[first.bytes[i]] = 1;
        for (size_t i = 0; i < second.len; i++)
            squeeze[second.bytes[i]] = 1;
    } else if (argc == 2) {
        if (second.len == 0 && first.len > 0)
            xn_errx(EXIT_FAILURE, "string2 must not be empty");
        /* When string2 is shorter, its last byte pads it out. */
        for (size_t i = 0; i < first.len; i++) {
            size_t pick = (i < second.len) ? i : second.len - 1;

            map[first.bytes[i]] = second.bytes[pick];
        }
        for (size_t i = 0; squeeze_mode && i < second.len; i++)
            squeeze[second.bytes[i]] = 1;
    } else {
        for (size_t i = 0; i < first.len; i++)
            squeeze[first.bytes[i]] = 1;
    }

    for (;;) {
        ssize_t got = read(STDIN_FILENO, in, sizeof(in));
        size_t len = 0;

        if (got == 0)
            break;
        if (got == -1) {
            if (errno == EINTR)
                continue;
            xn_err(EXIT_FAILURE, "standard input");
        }
        for (size_t i = 0; i < (size_t)got; i++) {
            int byte = in[i];

            if (drop[byte])
                continue;
            byte = map[byte];
            if (squeeze[byte] && byte == last)
                continue;
            out[len++] = (unsigned char)byte;
            last = byte;
        }
        if (xn_writeall(STDOUT_FILENO, out, len) == -1)
            xn_err(EXIT_FAILURE, "standard output");
    }
    free(first.bytes);
    free(second.bytes);
    return EXIT_SUCCESS;
}
