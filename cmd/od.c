/* od - dump files in various formats. SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xn.h"

#define LINE_BYTES 16
#define MAX_SIZE 16

struct format {
    int type;
    size_t size;
    int width;
};

static void add_format(int, size_t);
static void parse_types(const char *);
static uintmax_t parse_count(const char *, int);
static int parse_legacy_offset(const char *, uintmax_t *);
static FILE *next_file(void);
static size_t read_input(unsigned char *, size_t);
static void skip_input(uintmax_t);
static void put_address(uintmax_t);
static void put_char_field(unsigned char);
static void put_named_field(unsigned char);
static void put_number(const struct format *, const unsigned char *);
static void dump_line(const unsigned char *, size_t, uintmax_t);
static void usage(void);

static struct format *formats;
static size_t nformats;
static int address_base = 'o';
static char **operands;
static int noperands;
static int next_operand;
static FILE *current;
static int status = EXIT_SUCCESS;

static void
add_format(int type, size_t size)
{
    static const int octal[] = { 0, 3, 6, 0, 11, 0, 0, 0, 22 };
    static const int unsig[] = { 0, 3, 5, 0, 10, 0, 0, 0, 20 };
    static const int sign[] = { 0, 4, 6, 0, 11, 0, 0, 0, 21 };
    struct format item;

    item.type = type;
    item.size = size;
    if (type == 'a' || type == 'c')
        item.width = 3;
    else if (type == 'f')
        item.width = (size <= sizeof(float)) ? 14
                     : (size <= sizeof(double)) ? 23 : 30;
    else if (size > 8)
        xn_errx(EXIT_FAILURE, "invalid type size");
    else if (type == 'o')
        item.width = octal[size];
    else if (type == 'u')
        item.width = unsig[size];
    else if (type == 'd')
        item.width = sign[size];
    else
        item.width = (int)size * 2;
    formats = xn_realloc(formats, (nformats + 1) * sizeof(*formats));
    formats[nformats++] = item;
}

/* Parse a -t type string such as "x1", "fD" or "d2c". */
static void
parse_types(const char *text)
{
    while (*text != '\0') {
        int type = (unsigned char)*text++;
        size_t size;

        if (type == 'a' || type == 'c') {
            add_format(type, 1);
            continue;
        }
        if (strchr("dfoux", type) == NULL)
            xn_errx(EXIT_FAILURE, "%c: invalid type", type);
        size = (type == 'f') ? sizeof(double) : sizeof(int);

        if (*text >= '0' && *text <= '9') {
            char *end;
            unsigned long value = strtoul(text, &end, 10);

            text = end;
            size = (size_t)value;
        } else if (type == 'f' && *text == 'F') {
            size = sizeof(float);
            text++;
        } else if (type == 'f' && *text == 'D') {
            size = sizeof(double);
            text++;
        } else if (type == 'f' && *text == 'L') {
            size = sizeof(long double);
            text++;
        } else if (type != 'f' && *text == 'C') {
            size = sizeof(char);
            text++;
        } else if (type != 'f' && *text == 'S') {
            size = sizeof(short);
            text++;
        } else if (type != 'f' && *text == 'I') {
            size = sizeof(int);
            text++;
        } else if (type != 'f' && *text == 'L') {
            size = sizeof(long);
            text++;
        }

        if (type == 'f') {
            if (size != sizeof(float) && size != sizeof(double) &&
                size != sizeof(long double))
                xn_errx(EXIT_FAILURE, "invalid floating type size");
        } else if (size != 1 && size != 2 && size != 4 && size != 8) {
            xn_errx(EXIT_FAILURE, "invalid integer type size");
        }
        add_format(type, size);
    }
}

/* Parse a -j or -N value: decimal, 0 octal or 0x hex, with b, k, m. */
static uintmax_t
parse_count(const char *text, int allow_suffix)
{
    char *end;
    uintmax_t value;

    errno = 0;
    value = strtoumax(text, &end, 0);
    if (end == text || errno == ERANGE || text[0] == '-')
        xn_errx(EXIT_FAILURE, "%s: invalid number", text);
    if (*end != '\0') {
        uintmax_t scale;

        if (!allow_suffix || end[1] != '\0')
            xn_errx(EXIT_FAILURE, "%s: invalid number", text);
        if (*end == 'b')
            scale = 512;
        else if (*end == 'k')
            scale = 1024;
        else if (*end == 'm')
            scale = 1048576;
        else
            xn_errx(EXIT_FAILURE, "%s: invalid number", text);
        if (value > UINTMAX_MAX / scale)
            xn_errx(EXIT_FAILURE, "%s: number too large", text);
        value *= scale;
    }
    return value;
}

/* Recognise [+]offset[.][b], which is octal unless marked decimal or hex. */
static int
parse_legacy_offset(const char *text, uintmax_t *value)
{
    const char *ptr = text;
    char *end;
    int base = 8;

    if (*ptr == '+')
        ptr++;
    if (*ptr == '\0')
        return 0;
    if (ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X')) {
        base = 16;
    } else {
        const char *scan = ptr;

        while (isdigit((unsigned char)*scan))
            scan++;
        if (*scan == '.')
            base = 10;
    }
    errno = 0;
    *value = strtoumax(ptr, &end, base);
    if (end == ptr || errno == ERANGE)
        return 0;
    if (*end == '.' && base == 10)
        end++;
    if (*end == 'b') {
        *value *= 512;
        end++;
    }
    return *end == '\0';
}

static FILE *
next_file(void)
{
    if (noperands == 0) {
        if (next_operand++ == 0)
            return stdin;
        return NULL;
    }
    while (next_operand < noperands) {
        const char *name = operands[next_operand++];
        FILE *fp;

        if (strcmp(name, "-") == 0)
            return stdin;
        if ((fp = fopen(name, "r")) != NULL)
            return fp;
        xn_warn("%s", name);
        status = EXIT_FAILURE;
    }
    return NULL;
}

/* Read across file boundaries; inputs form one continuous stream. */
static size_t
read_input(unsigned char *buf, size_t want)
{
    size_t have = 0;

    while (have < want) {
        size_t got;

        if (current == NULL && (current = next_file()) == NULL)
            break;
        got = fread(buf + have, 1, want - have, current);
        have += got;
        if (have < want) {
            if (ferror(current)) {
                xn_warn("read error");
                status = EXIT_FAILURE;
            }
            if (current != stdin)
                (void)fclose(current);
            current = NULL;
        }
    }
    return have;
}

static void
skip_input(uintmax_t count)
{
    unsigned char buf[4096];

    while (count > 0) {
        size_t want = (count < sizeof(buf)) ? (size_t)count : sizeof(buf);
        size_t got = read_input(buf, want);

        if (got == 0)
            xn_errx(EXIT_FAILURE, "cannot skip past end of input");
        count -= got;
    }
}

static void
put_address(uintmax_t offset)
{
    switch (address_base) {
    case 'd':
        (void)printf("%07ju", offset);
        break;
    case 'o':
        (void)printf("%07jo", offset);
        break;
    case 'x':
        (void)printf("%06jx", offset);
        break;
    default:
        break;
    }
}

static void
put_char_field(unsigned char byte)
{
    static const char names[] = "\a\b\f\n\r\t\v";
    static const char *const shown[] = {
        "\\a", "\\b", "\\f", "\\n", "\\r", "\\t", "\\v"
    };
    const char *match = (byte != 0) ? strchr(names, byte) : NULL;

    if (byte == 0)
        (void)printf(" %3s", "\\0");
    else if (match != NULL)
        (void)printf(" %3s", shown[match - names]);
    else if (isprint(byte) && byte < 0x80)
        (void)printf(" %3c", byte);
    else
        (void)printf(" %03o", (unsigned int)byte);
}

static void
put_named_field(unsigned char byte)
{
    static const char *const names[] = {
        "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel",
        "bs", "ht", "nl", "vt", "ff", "cr", "so", "si",
        "dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb",
        "can", "em", "sub", "esc", "fs", "gs", "rs", "us", "sp"
    };
    unsigned int low = byte & 0x7f;

    if (low <= 0x20)
        (void)printf(" %3s", names[low]);
    else if (low == 0x7f)
        (void)printf(" %3s", "del");
    else
        (void)printf(" %3c", (int)low);
}

static void
put_number(const struct format *format, const unsigned char *data)
{
    uintmax_t raw = 0;
    int width = format->width;

    if (format->type == 'f') {
        if (format->size == sizeof(float)) {
            float value;

            memcpy(&value, data, sizeof(value));
            (void)printf(" %*.7e", width, (double)value);
        } else if (format->size == sizeof(double)) {
            double value;

            memcpy(&value, data, sizeof(value));
            (void)printf(" %*.16e", width, value);
        } else {
            long double value;

            memcpy(&value, data, sizeof(value));
            (void)printf(" %*.21Le", width, value);
        }
        return;
    }

    switch (format->size) {
    case 1:
        raw = *data;
        break;
    case 2: {
        uint16_t value;

        memcpy(&value, data, sizeof(value));
        raw = value;
        break;
    }
    case 4: {
        uint32_t value;

        memcpy(&value, data, sizeof(value));
        raw = value;
        break;
    }
    default: {
        uint64_t value;

        memcpy(&value, data, sizeof(value));
        raw = value;
        break;
    }
    }

    switch (format->type) {
    case 'o':
        (void)printf(" %0*jo", width, raw);
        break;
    case 'u':
        (void)printf(" %*ju", width, raw);
        break;
    case 'x':
        (void)printf(" %0*jx", width, raw);
        break;
    default: {
        intmax_t value;

        if (format->size == 1)
            value = (int8_t)(uint8_t)raw;
        else if (format->size == 2)
            value = (int16_t)(uint16_t)raw;
        else if (format->size == 4)
            value = (int32_t)(uint32_t)raw;
        else
            value = (int64_t)(uint64_t)raw;
        (void)printf(" %*jd", width, value);
        break;
    }
    }
}

/* A short final line is zero padded to a whole number of items. */
static void
dump_line(const unsigned char *data, size_t len, uintmax_t offset)
{
    static const char blank[] = "                ";
    unsigned char padded[LINE_BYTES + MAX_SIZE];
    int cell = 0;

    /* Every format gets the same columns per byte, so that lines align. */
    for (size_t f = 0; f < nformats; f++) {
        int size = (int)formats[f].size;
        int need = (formats[f].width + 1 + size - 1) / size;

        if (need > cell)
            cell = need;
    }

    memset(padded, 0, sizeof(padded));
    memcpy(padded, data, len);
    for (size_t f = 0; f < nformats; f++) {
        const struct format *format = &formats[f];
        int pad = (nformats > 1)
                  ? cell * (int)format->size - (format->width + 1) : 0;

        if (f == 0)
            put_address(offset);
        else if (address_base != 'n')
            (void)printf("%.*s", (address_base == 'x') ? 6 : 7, blank);
        for (size_t pos = 0; pos < len; pos += format->size) {
            (void)printf("%*s", pad, "");
            if (format->type == 'a')
                put_named_field(padded[pos]);
            else if (format->type == 'c')
                put_char_field(padded[pos]);
            else
                put_number(format, padded + pos);
        }
        (void)putchar('\n');
    }
}

static void
usage(void)
{
    (void)fprintf(stderr,
                  "usage: %s [-v] [-A base] [-j skip] [-N count] "
                  "[-t type]... [file...]\n"
                  "       %s [-bcdosx] [file] [[+]offset[.][b]]\n",
                  xn_progname(), xn_progname());
    exit(EXIT_FAILURE);
}

/* Dump input in the selected representations. */
int
main(int argc, char **argv)
{
    unsigned char line[LINE_BYTES], prev[LINE_BYTES];
    uintmax_t skip = 0, limit = 0, offset;
    int have_limit = 0, show_all = 0, modern = 0;
    int have_prev = 0, starred = 0;
    size_t prev_len = 0;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "A:N:bcdj:ost:vx")) != -1) {
        switch (ch) {
        case 'A':
            modern = 1;
            if (strlen(xn_optarg) != 1 ||
                strchr("dnox", xn_optarg[0]) == NULL)
                xn_errx(EXIT_FAILURE, "%s: invalid address base", xn_optarg);
            address_base = xn_optarg[0];
            break;
        case 'N':
            modern = 1;
            limit = parse_count(xn_optarg, 0);
            have_limit = 1;
            break;
        case 'b':
            add_format('o', 1);
            break;
        case 'c':
            add_format('c', 1);
            break;
        case 'd':
            add_format('u', 2);
            break;
        case 'j':
            modern = 1;
            skip = parse_count(xn_optarg, 1);
            break;
        case 'o':
            add_format('o', 2);
            break;
        case 's':
            add_format('d', 2);
            break;
        case 't':
            modern = 1;
            parse_types(xn_optarg);
            break;
        case 'v':
            modern = 1;
            show_all = 1;
            break;
        case 'x':
            add_format('x', 2);
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (nformats == 0)
        add_format('o', 2);

    /* The offset operand exists only in the legacy synopsis. */
    if (!modern && argc > 0) {
        uintmax_t value;
        const char *last = argv[argc - 1];

        if (argc <= 2 && (last[0] == '+' || argc == 2) &&
            parse_legacy_offset(last, &value)) {
            skip = value;
            argc--;
        }
    }
    operands = argv;
    noperands = argc;

    if (skip > 0)
        skip_input(skip);

    offset = skip;
    for (;;) {
        size_t want = LINE_BYTES;
        size_t got;

        if (have_limit && limit < want)
            want = (size_t)limit;
        got = (want > 0) ? read_input(line, want) : 0;
        if (got == 0)
            break;
        if (have_limit)
            limit -= got;

        if (!show_all && have_prev && got == prev_len &&
            memcmp(line, prev, got) == 0) {
            if (!starred) {
                (void)puts("*");
                starred = 1;
            }
        } else {
            dump_line(line, got, offset);
            starred = 0;
        }
        memcpy(prev, line, got);
        prev_len = got;
        have_prev = 1;
        offset += got;
    }
    if (address_base != 'n') {
        put_address(offset);
        (void)putchar('\n');
    }
    return xn_finish(status);
}
