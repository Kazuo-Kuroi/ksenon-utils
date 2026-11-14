/*
 * sort - sort, merge, or sequence check text files.
 * SPDX-License-Identifier: MIT
 *
 * Input is held in memory. A stable merge sort over all input also gives
 * the correct result for -m, so no separate merge pass exists.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xn.h"

#define MOD_BLANK_START 0x01
#define MOD_BLANK_END 0x02
#define MOD_DICT 0x04
#define MOD_FOLD 0x08
#define MOD_PRINT 0x10
#define MOD_NUMERIC 0x20
#define MOD_REVERSE 0x40
#define EXIT_ERROR 2
#define READ_CHUNK 65536

struct key {
    size_t start_field;
    size_t start_char;
    size_t end_field;
    size_t end_char;
    unsigned int mods;
    int has_end;
    int has_mods;
};

struct span {
    const char *text;
    size_t len;
};

struct line {
    const char *text;
    size_t len;
    struct span *spans;
};

struct scratch {
    char *data;
    size_t cap;
};

static void load_file(const char *);
static void add_line(const char *, size_t);
static const char *skip_blanks(const char *, const char *);
static const char *field_start(const char *, const char *, size_t);
static const char *field_end(const char *, const char *);
static struct span locate(const struct line *, const struct key *);
static void parse_key(const char *);
static const char *parse_position(const char *, size_t *, size_t *, int);
static unsigned int parse_mods(const char **, int);
static size_t reduce(struct span, unsigned int, struct scratch *);
static int collate(const char *, size_t, const char *, size_t);
static int compare_numeric(struct span, struct span);
static int compare_span(struct span, struct span, unsigned int);
static int compare_keys(const struct line *, const struct line *);
static int compare_lines(const struct line *, const struct line *);
static void merge_sort(struct line **, size_t);
static void put_line(FILE *, const struct line *);
static void usage(void);

static struct key *keys;
static size_t nkeys;
static unsigned int global_mods;
static int field_sep = -1;
static int unique;
static int locale_is_c;
static int radix = '.';
static struct line *lines;
static size_t nlines, lines_cap;
static struct scratch scratch_a, scratch_b;

static void
add_line(const char *text, size_t len)
{
    if (nlines == lines_cap) {
        lines_cap = (lines_cap != 0) ? lines_cap * 2 : 1024;
        lines = xn_realloc(lines, lines_cap * sizeof(*lines));
    }
    lines[nlines].text = text;
    lines[nlines].len = len;
    lines[nlines].spans = NULL;
    nlines++;
}

/* Read a whole input and index its lines. The buffer is never freed. */
static void
load_file(const char *name)
{
    int is_stdin = (strcmp(name, "-") == 0);
    int fd = is_stdin ? STDIN_FILENO : open(name, O_RDONLY);
    size_t cap = READ_CHUNK, len = 0;
    char *buf, *ptr, *end;

    if (fd == -1)
        xn_err(EXIT_ERROR, "%s", name);
    buf = xn_malloc(cap);
    for (;;) {
        ssize_t got;

        if (cap - len < READ_CHUNK) {
            cap *= 2;
            buf = xn_realloc(buf, cap);
        }
        got = read(fd, buf + len, READ_CHUNK);
        if (got == 0)
            break;
        if (got == -1) {
            if (errno == EINTR)
                continue;
            xn_err(EXIT_ERROR, "%s", name);
        }
        len += (size_t)got;
    }
    if (!is_stdin)
        (void)close(fd);

    ptr = buf;
    end = buf + len;
    while (ptr < end) {
        char *newline = memchr(ptr, '\n', (size_t)(end - ptr));
        char *stop = (newline != NULL) ? newline : end;

        add_line(ptr, (size_t)(stop - ptr));
        ptr = stop + 1;
    }
}

static const char *
skip_blanks(const char *ptr, const char *end)
{
    while (ptr < end && isblank((unsigned char)*ptr))
        ptr++;
    return ptr;
}

/* Return the start of the given 1-based field. */
static const char *
field_start(const char *ptr, const char *end, size_t field)
{
    for (size_t i = 1; i < field && ptr < end; i++) {
        if (field_sep >= 0) {
            const char *next = memchr(ptr, field_sep, (size_t)(end - ptr));

            ptr = (next != NULL) ? next + 1 : end;
        } else {
            ptr = skip_blanks(ptr, end);
            while (ptr < end && !isblank((unsigned char)*ptr))
                ptr++;
        }
    }
    return ptr;
}

static const char *
field_end(const char *ptr, const char *end)
{
    if (field_sep >= 0) {
        const char *next = memchr(ptr, field_sep, (size_t)(end - ptr));

        return (next != NULL) ? next : end;
    }
    ptr = skip_blanks(ptr, end);
    while (ptr < end && !isblank((unsigned char)*ptr))
        ptr++;
    return ptr;
}

static struct span
locate(const struct line *line, const struct key *key)
{
    const char *end = line->text + line->len;
    const char *start = field_start(line->text, end, key->start_field);
    const char *stop = end;
    struct span span;

    if ((key->mods & MOD_BLANK_START) != 0)
        start = skip_blanks(start, end);
    start = ((size_t)(end - start) < key->start_char - 1)
            ? end : start + key->start_char - 1;

    if (key->has_end) {
        const char *base = field_start(line->text, end, key->end_field);

        if (key->end_char == 0) {
            stop = field_end(base, end);
        } else {
            if ((key->mods & MOD_BLANK_END) != 0)
                base = skip_blanks(base, end);
            stop = ((size_t)(end - base) < key->end_char)
                   ? end : base + key->end_char;
        }
    }
    span.text = start;
    span.len = (stop > start) ? (size_t)(stop - start) : 0;
    return span;
}

static unsigned int
parse_mods(const char **textp, int is_end)
{
    unsigned int mods = 0;
    const char *text = *textp;

    for (;; text++) {
        if (*text == 'b')
            mods |= is_end ? MOD_BLANK_END : MOD_BLANK_START;
        else if (*text == 'd')
            mods |= MOD_DICT;
        else if (*text == 'f')
            mods |= MOD_FOLD;
        else if (*text == 'i')
            mods |= MOD_PRINT;
        else if (*text == 'n')
            mods |= MOD_NUMERIC;
        else if (*text == 'r')
            mods |= MOD_REVERSE;
        else
            break;
    }
    *textp = text;
    return mods;
}

static const char *
parse_position(const char *text, size_t *field, size_t *chr, int is_end)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (end == text || errno == ERANGE || value == 0)
        xn_errx(EXIT_ERROR, "invalid key field");
    *field = value;
    *chr = is_end ? 0 : 1;
    if (*end == '.') {
        text = end + 1;
        value = strtoul(text, &end, 10);
        if (end == text || (value == 0 && !is_end))
            xn_errx(EXIT_ERROR, "invalid key character position");
        *chr = value;
    }
    return end;
}

/* Parse field_start[type][,field_end[type]]. */
static void
parse_key(const char *text)
{
    struct key key;

    memset(&key, 0, sizeof(key));
    text = parse_position(text, &key.start_field, &key.start_char, 0);
    key.mods = parse_mods(&text, 0);
    if (*text == ',') {
        text++;
        key.has_end = 1;
        text = parse_position(text, &key.end_field, &key.end_char, 1);
        key.mods |= parse_mods(&text, 1);
    }
    if (*text != '\0')
        xn_errx(EXIT_ERROR, "invalid key definition");
    key.has_mods = (key.mods != 0);
    keys = xn_realloc(keys, (nkeys + 1) * sizeof(*keys));
    keys[nkeys++] = key;
}

/* Apply -d, -f and -i, writing the reduced key into scratch space. */
static size_t
reduce(struct span span, unsigned int mods, struct scratch *space)
{
    size_t len = 0;

    if (span.len + 1 > space->cap) {
        space->cap = (span.len + 1) * 2;
        space->data = xn_realloc(space->data, space->cap);
    }
    for (size_t i = 0; i < span.len; i++) {
        unsigned char byte = (unsigned char)span.text[i];

        if ((mods & MOD_DICT) != 0 && !isalnum(byte) && !isblank(byte))
            continue;
        if ((mods & MOD_PRINT) != 0 && !isprint(byte))
            continue;
        if ((mods & MOD_FOLD) != 0)
            byte = (unsigned char)toupper(byte);
        space->data[len++] = (char)byte;
    }
    return len;
}

static int
collate(const char *a, size_t alen, const char *b, size_t blen)
{
    int order;

    if (locale_is_c) {
        size_t common = (alen < blen) ? alen : blen;

        order = (common > 0) ? memcmp(a, b, common) : 0;
        if (order != 0)
            return order;
        return (alen > blen) - (alen < blen);
    }

    /* strcoll needs terminated strings; keep the caller's bytes intact. */
    if (alen + 1 > scratch_a.cap) {
        scratch_a.cap = (alen + 1) * 2;
        scratch_a.data = xn_realloc(scratch_a.data, scratch_a.cap);
    }
    if (blen + 1 > scratch_b.cap) {
        scratch_b.cap = (blen + 1) * 2;
        scratch_b.data = xn_realloc(scratch_b.data, scratch_b.cap);
    }
    memmove(scratch_a.data, a, alen);
    memmove(scratch_b.data, b, blen);
    scratch_a.data[alen] = '\0';
    scratch_b.data[blen] = '\0';
    return strcoll(scratch_a.data, scratch_b.data);
}

/* Compare decimal strings exactly, with no conversion to binary. */
static int
compare_numeric(struct span a, struct span b)
{
    const char *pa = skip_blanks(a.text, a.text + a.len);
    const char *pb = skip_blanks(b.text, b.text + b.len);
    const char *ea = a.text + a.len;
    const char *eb = b.text + b.len;
    const char *ia, *ib;
    size_t la, lb;
    int nega = 0, negb = 0, sign, order = 0, nonzero_a = 0, nonzero_b = 0;

    if (pa < ea && *pa == '-') {
        nega = 1;
        pa++;
    }
    if (pb < eb && *pb == '-') {
        negb = 1;
        pb++;
    }
    while (pa < ea && *pa == '0')
        pa++;
    while (pb < eb && *pb == '0')
        pb++;
    ia = pa;
    ib = pb;
    while (pa < ea && isdigit((unsigned char)*pa))
        pa++;
    while (pb < eb && isdigit((unsigned char)*pb))
        pb++;
    la = (size_t)(pa - ia);
    lb = (size_t)(pb - ib);
    nonzero_a = (la > 0);
    nonzero_b = (lb > 0);

    if (la != lb) {
        order = (la > lb) ? 1 : -1;
    } else if (la > 0) {
        order = memcmp(ia, ib, la);
    }

    if (pa < ea && *pa == radix)
        pa++;
    else
        ea = pa;
    if (pb < eb && *pb == radix)
        pb++;
    else
        eb = pb;
    while (pa < ea || pb < eb) {
        int da = (pa < ea && isdigit((unsigned char)*pa)) ? *pa : '0';
        int db = (pb < eb && isdigit((unsigned char)*pb)) ? *pb : '0';

        if (pa < ea && !isdigit((unsigned char)*pa))
            ea = pa;
        if (pb < eb && !isdigit((unsigned char)*pb))
            eb = pb;
        if (pa >= ea && pb >= eb)
            break;
        if (da != '0' && pa < ea)
            nonzero_a = 1;
        if (db != '0' && pb < eb)
            nonzero_b = 1;
        if (order == 0 && da != db)
            order = (da > db) ? 1 : -1;
        if (pa < ea)
            pa++;
        if (pb < eb)
            pb++;
    }

    /* Negative zero sorts equal to zero. */
    if (!nonzero_a)
        nega = 0;
    if (!nonzero_b)
        negb = 0;
    if (nega != negb)
        return nega ? -1 : 1;
    sign = nega ? -1 : 1;
    return order * sign;
}

static int
compare_span(struct span a, struct span b, unsigned int mods)
{
    if ((mods & MOD_NUMERIC) != 0)
        return compare_numeric(a, b);
    if ((mods & (MOD_DICT | MOD_FOLD | MOD_PRINT)) != 0) {
        static struct scratch left, right;
        size_t la = reduce(a, mods, &left);
        size_t lb = reduce(b, mods, &right);

        return collate(left.data, la, right.data, lb);
    }
    return collate(a.text, a.len, b.text, b.len);
}

static int
compare_keys(const struct line *a, const struct line *b)
{
    if (nkeys == 0) {
        struct span sa, sb;
        int order;

        sa.text = a->text;
        sa.len = a->len;
        sb.text = b->text;
        sb.len = b->len;
        if ((global_mods & MOD_BLANK_START) != 0) {
            const char *end = a->text + a->len;

            sa.text = skip_blanks(a->text, end);
            sa.len = (size_t)(end - sa.text);
            end = b->text + b->len;
            sb.text = skip_blanks(b->text, end);
            sb.len = (size_t)(end - sb.text);
        }
        order = compare_span(sa, sb, global_mods);
        return ((global_mods & MOD_REVERSE) != 0) ? -order : order;
    }
    for (size_t i = 0; i < nkeys; i++) {
        int order = compare_span(a->spans[i], b->spans[i], keys[i].mods);

        if (order != 0)
            return ((keys[i].mods & MOD_REVERSE) != 0) ? -order : order;
    }
    return 0;
}

/* Key order, then whole-line bytes as the POSIX last-resort comparison. */
static int
compare_lines(const struct line *a, const struct line *b)
{
    int order = compare_keys(a, b);

    if (order == 0 && !unique) {
        size_t common = (a->len < b->len) ? a->len : b->len;

        order = (common > 0) ? memcmp(a->text, b->text, common) : 0;
        if (order == 0)
            order = (a->len > b->len) - (a->len < b->len);
        if ((global_mods & MOD_REVERSE) != 0)
            order = -order;
    }
    return order;
}

/* Bottom-up merge sort; stability keeps the first of equal keys for -u. */
static void
merge_sort(struct line **items, size_t count)
{
    struct line **spare = xn_malloc(count * sizeof(*spare));
    struct line **from = items, **to = spare;

    for (size_t width = 1; width < count; width *= 2) {
        for (size_t low = 0; low < count; low += 2 * width) {
            size_t mid = (low + width < count) ? low + width : count;
            size_t high = (low + 2 * width < count) ? low + 2 * width : count;
            size_t left = low, right = mid, out = low;

            while (left < mid && right < high) {
                if (compare_lines(from[right], from[left]) < 0)
                    to[out++] = from[right++];
                else
                    to[out++] = from[left++];
            }
            while (left < mid)
                to[out++] = from[left++];
            while (right < high)
                to[out++] = from[right++];
        }
        {
            struct line **swap = from;

            from = to;
            to = swap;
        }
    }
    if (from != items)
        memcpy(items, from, count * sizeof(*items));
    free(spare);
}

static void
put_line(FILE *out, const struct line *line)
{
    (void)fwrite(line->text, 1, line->len, out);
    (void)putc('\n', out);
}

static void
usage(void)
{
    (void)fprintf(stderr,
                  "usage: %s [-m] [-o output] [-bdfinru] [-t char] "
                  "[-k keydef]... [file...]\n"
                  "       %s -c [-bdfinru] [-t char] [-k keydef]... [file]\n",
                  xn_progname(), xn_progname());
    exit(EXIT_ERROR);
}

/* Sort the operands, which are all read before any output is opened. */
int
main(int argc, char **argv)
{
    const char *output = NULL;
    const char *collation;
    struct line **order;
    FILE *out = stdout;
    int check = 0;
    int ch;

    xn_init(argv);
    collation = setlocale(LC_COLLATE, NULL);
    locale_is_c = (collation == NULL || strcmp(collation, "C") == 0 ||
                   strcmp(collation, "POSIX") == 0);
    {
        const struct lconv *conv = localeconv();

        if (conv->decimal_point != NULL && conv->decimal_point[0] != '\0')
            radix = (unsigned char)conv->decimal_point[0];
    }

    while ((ch = xn_getopt(argc, argv, "bcdfik:mno:rt:u")) != -1) {
        switch (ch) {
        case 'b':
            global_mods |= MOD_BLANK_START | MOD_BLANK_END;
            break;
        case 'c':
            check = 1;
            break;
        case 'd':
            global_mods |= MOD_DICT;
            break;
        case 'f':
            global_mods |= MOD_FOLD;
            break;
        case 'i':
            global_mods |= MOD_PRINT;
            break;
        case 'k':
            parse_key(xn_optarg);
            break;
        case 'm':
            break;
        case 'n':
            global_mods |= MOD_NUMERIC;
            break;
        case 'o':
            output = xn_optarg;
            break;
        case 'r':
            global_mods |= MOD_REVERSE;
            break;
        case 't':
            if (xn_optarg[0] == '\0' || xn_optarg[1] != '\0')
                xn_errx(EXIT_ERROR, "field separator must be one character");
            field_sep = (unsigned char)xn_optarg[0];
            break;
        case 'u':
            unique = 1;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;

    /* A key with no modifiers of its own inherits the global ones. */
    for (size_t i = 0; i < nkeys; i++) {
        if (!keys[i].has_mods)
            keys[i].mods = global_mods;
    }

    if (argc == 0) {
        load_file("-");
    } else {
        for (int i = 0; i < argc; i++)
            load_file(argv[i]);
    }

    if (nkeys > 0 && nlines > 0) {
        struct span *pool = xn_malloc(nlines * nkeys * sizeof(*pool));

        for (size_t i = 0; i < nlines; i++) {
            lines[i].spans = pool + i * nkeys;
            for (size_t k = 0; k < nkeys; k++)
                lines[i].spans[k] = locate(&lines[i], &keys[k]);
        }
    }

    if (check) {
        for (size_t i = 1; i < nlines; i++) {
            int cmp = compare_lines(&lines[i - 1], &lines[i]);

            if (cmp > 0 || (unique && cmp == 0)) {
                xn_warnx("disorder at line %zu", i + 1);
                return EXIT_FAILURE;
            }
        }
        return EXIT_SUCCESS;
    }

    order = xn_malloc(nlines * sizeof(*order));
    for (size_t i = 0; i < nlines; i++)
        order[i] = &lines[i];
    if (nlines > 1)
        merge_sort(order, nlines);

    if (output != NULL && (out = fopen(output, "w")) == NULL)
        xn_err(EXIT_ERROR, "%s", output);
    for (size_t i = 0; i < nlines; i++) {
        if (unique && i > 0 && compare_keys(order[i - 1], order[i]) == 0)
            continue;
        put_line(out, order[i]);
    }
    if (fflush(out) == EOF || ferror(out))
        xn_err(EXIT_ERROR, "%s", (output != NULL) ? output : "standard output");
    return EXIT_SUCCESS;
}
