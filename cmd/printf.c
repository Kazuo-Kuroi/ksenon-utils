/* printf - write formatted output. SPDX-License-Identifier: MIT */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xn.h"

#define SPEC_MAX 64

static const char *format_spec(const char *);
static const char *next_arg(void);
static int parse_star(void);
static uintmax_t parse_uint(const char *);
static intmax_t parse_int(const char *);
static long double parse_float(const char *);
static void check_number(const char *, const char *);
static const char *escape(const char *, int);
static char *expand_b(const char *, size_t *);
static void usage(void);

static char **args;
static int status = EXIT_SUCCESS;
static int consumed;
static int stop_output;

static const char *
next_arg(void)
{
    if (*args == NULL)
        return NULL;
    consumed = 1;
    return *args++;
}

static void
check_number(const char *text, const char *end)
{
    if (end == text) {
        xn_warnx("%s: expected numeric value", text);
        status = EXIT_FAILURE;
    } else if (*end != '\0') {
        xn_warnx("%s: not completely converted", text);
        status = EXIT_FAILURE;
    } else if (errno == ERANGE) {
        xn_warnx("%s: %s", text, strerror(ERANGE));
        status = EXIT_FAILURE;
    }
}

static intmax_t
parse_int(const char *text)
{
    char *end;
    intmax_t value;

    if (text == NULL)
        return 0;
    if (text[0] == '\'' || text[0] == '"')
        return (unsigned char)text[1];
    errno = 0;
    value = strtoimax(text, &end, 0);
    check_number(text, end);
    return value;
}

static uintmax_t
parse_uint(const char *text)
{
    char *end;
    uintmax_t value;

    if (text == NULL)
        return 0;
    if (text[0] == '\'' || text[0] == '"')
        return (unsigned char)text[1];
    errno = 0;
    value = strtoumax(text, &end, 0);
    check_number(text, end);
    return value;
}

static long double
parse_float(const char *text)
{
    char *end;
    long double value;

    if (text == NULL)
        return 0;
    if (text[0] == '\'' || text[0] == '"')
        return (unsigned char)text[1];
    errno = 0;
    value = strtold(text, &end);
    check_number(text, end);
    return value;
}

static int
parse_star(void)
{
    intmax_t value = parse_int(next_arg());

    return (int)value;
}

/*
 * Process one backslash escape at text, which points past the backslash.
 * in_b selects the %b argument rules, where octal is \0ddd and \c ends all
 * output. Returns the position following the escape.
 */
static const char *
escape(const char *text, int in_b)
{
    static const char names[] = "abfnrtv\\";
    static const char codes[] = "\a\b\f\n\r\t\v\\";
    const char *match;
    unsigned char byte;

    if (*text == '\0') {
        (void)putchar('\\');
        return text;
    }
    if (in_b && *text == 'c') {
        stop_output = 1;
        return text + 1;
    }
    if (in_b && *text == '0') {
        text += 1 + xn_unescape_octal(text + 1, 3, &byte);
        (void)putchar(byte);
        return text;
    }
    if (!in_b && *text >= '0' && *text <= '7') {
        text += xn_unescape_octal(text, 3, &byte);
        (void)putchar(byte);
        return text;
    }
    if ((match = strchr(names, *text)) != NULL) {
        (void)putchar(codes[match - names]);
        return text + 1;
    }
    (void)putchar('\\');
    (void)putchar((unsigned char)*text);
    return text + 1;
}

/*
 * Expand a %b argument into a buffer so width and precision can apply. The
 * expansion can never be longer than its source. The result may hold NUL
 * bytes, so its length is returned separately.
 */
static char *
expand_b(const char *text, size_t *lenp)
{
    char *out = xn_malloc(strlen(text) + 1);
    size_t len = 0;

    while (*text != '\0' && !stop_output) {
        unsigned char byte;

        if (*text != '\\' || text[1] == '\0') {
            out[len++] = *text++;
            continue;
        }
        text++;
        switch (*text) {
        case 'a':
            byte = '\a';
            break;
        case 'b':
            byte = '\b';
            break;
        case 'f':
            byte = '\f';
            break;
        case 'n':
            byte = '\n';
            break;
        case 'r':
            byte = '\r';
            break;
        case 't':
            byte = '\t';
            break;
        case 'v':
            byte = '\v';
            break;
        case '\\':
            byte = '\\';
            break;
        case 'c':
            stop_output = 1;
            continue;
        case '0':
            text += xn_unescape_octal(text + 1, 3, &byte);
            break;
        default:
            out[len++] = '\\';
            byte = (unsigned char)*text;
            break;
        }
        out[len++] = (char)byte;
        text++;
    }
    out[len] = '\0';
    *lenp = len;
    return out;
}

/*
 * Handle the conversion starting at the '%' in fmt and return the position
 * following it. The specification is rebuilt with a width modifier wide
 * enough for the argument type.
 */
static const char *
format_spec(const char *fmt)
{
    char spec[SPEC_MAX];
    const char *ptr = fmt + 1;
    size_t len = 1;
    int width = 0, precision = 0;
    int has_width = 0, has_precision = 0;
    char conv;

    spec[0] = '%';
    while (*ptr != '\0' && strchr("-+ #0", *ptr) != NULL &&
           len < SPEC_MAX - 8)
        spec[len++] = *ptr++;
    if (*ptr == '*') {
        width = parse_star();
        has_width = 1;
        spec[len++] = '*';
        ptr++;
    } else {
        while (*ptr >= '0' && *ptr <= '9' && len < SPEC_MAX - 8)
            spec[len++] = *ptr++;
    }
    if (*ptr == '.') {
        spec[len++] = *ptr++;
        if (*ptr == '*') {
            precision = parse_star();
            has_precision = 1;
            spec[len++] = '*';
            ptr++;
        } else {
            while (*ptr >= '0' && *ptr <= '9' && len < SPEC_MAX - 8)
                spec[len++] = *ptr++;
        }
    }
    conv = *ptr;
    if (conv == '\0')
        xn_errx(EXIT_FAILURE, "%s: incomplete format specification", fmt);

#define EMIT(value)                                                     \
    do {                                                                \
        if (has_width && has_precision)                                 \
            (void)printf(spec, width, precision, value);                \
        else if (has_width)                                             \
            (void)printf(spec, width, value);                           \
        else if (has_precision)                                         \
            (void)printf(spec, precision, value);                       \
        else                                                            \
            (void)printf(spec, value);                                  \
    } while (0)

    switch (conv) {
    case 'd':
    case 'i': {
        intmax_t value = parse_int(next_arg());

        spec[len++] = 'j';
        spec[len++] = conv;
        spec[len] = '\0';
        EMIT(value);
        break;
    }
    case 'o':
    case 'u':
    case 'x':
    case 'X': {
        uintmax_t value = parse_uint(next_arg());

        spec[len++] = 'j';
        spec[len++] = conv;
        spec[len] = '\0';
        EMIT(value);
        break;
    }
    case 'a':
    case 'A':
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G': {
        long double value = parse_float(next_arg());

        spec[len++] = 'L';
        spec[len++] = conv;
        spec[len] = '\0';
        EMIT(value);
        break;
    }
    case 'c': {
        const char *arg = next_arg();

        if (arg != NULL && *arg != '\0') {
            spec[len++] = 'c';
            spec[len] = '\0';
            EMIT((int)(unsigned char)*arg);
        }
        break;
    }
    case 's': {
        const char *arg = next_arg();

        spec[len++] = 's';
        spec[len] = '\0';
        EMIT((arg != NULL) ? arg : "");
        break;
    }
    case 'b': {
        const char *arg = next_arg();
        size_t textlen;
        char *text = expand_b((arg != NULL) ? arg : "", &textlen);

        if (len == 1) {
            (void)fwrite(text, 1, textlen, stdout);
        } else {
            spec[len++] = 's';
            spec[len] = '\0';
            EMIT(text);
        }
        free(text);
        break;
    }
    default:
        xn_errx(EXIT_FAILURE, "%%%c: invalid conversion", conv);
    }
#undef EMIT
    return ptr + 1;
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s format [argument...]\n", xn_progname());
    exit(EXIT_FAILURE);
}

/*
 * Write the operands according to format, reusing the format as many times
 * as needed to consume every argument.
 */
int
main(int argc, char **argv)
{
    const char *format;

    xn_init(argv);
    while (xn_getopt(argc, argv, "") != -1)
        usage();
    if (xn_optind >= argc)
        usage();
    format = argv[xn_optind];
    args = &argv[xn_optind + 1];

    do {
        const char *ptr = format;

        consumed = 0;
        while (*ptr != '\0' && !stop_output) {
            if (*ptr == '\\') {
                ptr = escape(ptr + 1, 0);
            } else if (*ptr == '%' && ptr[1] == '%') {
                (void)putchar('%');
                ptr += 2;
            } else if (*ptr == '%') {
                ptr = format_spec(ptr);
            } else {
                (void)putchar((unsigned char)*ptr++);
            }
        }
    } while (consumed && *args != NULL && !stop_output);

    return xn_finish(status);
}
