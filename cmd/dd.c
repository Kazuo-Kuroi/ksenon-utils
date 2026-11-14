/* dd - convert and copy a file. SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xn.h"

#define DEFAULT_BLOCK 512
#define BYTE_VALUES 256
#define CREATE_MODE 0666

#define CONV_ASCII 0x0001
#define CONV_EBCDIC 0x0002
#define CONV_IBM 0x0004
#define CONV_BLOCK 0x0008
#define CONV_UNBLOCK 0x0010
#define CONV_LCASE 0x0020
#define CONV_UCASE 0x0040
#define CONV_SWAB 0x0080
#define CONV_NOERROR 0x0100
#define CONV_NOTRUNC 0x0200
#define CONV_SYNC 0x0400

struct stats {
    uintmax_t in_full;
    uintmax_t in_part;
    uintmax_t out_full;
    uintmax_t out_part;
    uintmax_t truncated;
};

static uintmax_t parse_size(const char *);
static unsigned int parse_conv(const char *);
static void report(void);
static void on_interrupt(int);
static void write_block(const unsigned char *, size_t);
static void put_final(const unsigned char *, size_t);
static void stage_block(const unsigned char *, size_t);
static void finish_block(void);
static void stage_unblock(const unsigned char *, size_t);
static void finish_unblock(void);
static void process(unsigned char *, size_t);
static void skip_input(uintmax_t);
static void seek_output(uintmax_t);
static void usage(void);

/* The table POSIX gives for conv=ebcdic; conv=ascii is its inverse. */
static const unsigned char to_ebcdic[BYTE_VALUES] = {
    0x00, 0x01, 0x02, 0x03, 0x37, 0x2d, 0x2e, 0x2f, 0x16, 0x05, 0x25, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x3c, 0x3d, 0x32, 0x26,
    0x18, 0x19, 0x3f, 0x27, 0x1c, 0x1d, 0x1e, 0x1f, 0x40, 0x5a, 0x7f, 0x7b,
    0x5b, 0x6c, 0x50, 0x7d, 0x4d, 0x5d, 0x5c, 0x4e, 0x6b, 0x60, 0x4b, 0x61,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0x7a, 0x5e,
    0x4c, 0x7e, 0x6e, 0x6f, 0x7c, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xad, 0xe0, 0xbd, 0x9a, 0x6d,
    0x79, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x91, 0x92,
    0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
    0xa7, 0xa8, 0xa9, 0xc0, 0x4f, 0xd0, 0x5f, 0x07, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x15, 0x06, 0x17, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x09, 0x0a, 0x1b,
    0x30, 0x31, 0x1a, 0x33, 0x34, 0x35, 0x36, 0x08, 0x38, 0x39, 0x3a, 0x3b,
    0x04, 0x14, 0x3e, 0xe1, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x62, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x80, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x6a,
    0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xaa, 0xab, 0xac, 0x4a, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb,
    0xbc, 0xa1, 0xbe, 0xbf, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xda, 0xdb,
    0xdc, 0xdd, 0xde, 0xdf, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xfa, 0xfb,
    0xfc, 0xfd, 0xfe, 0xff
};

/* The conv=ibm table differs from conv=ebcdic in only a few positions. */
static const struct {
    unsigned char from;
    unsigned char to;
} ibm_changes[] = {
    { 0x5e, 0x5f }, { 0x7e, 0xa1 }, { 0xcb, 0x9a }, { 0xd5, 0xad },
    { 0xe5, 0xbd }
};

static struct stats stats;
static volatile sig_atomic_t interrupted;
static unsigned char final_table[BYTE_VALUES];
static unsigned char input_table[BYTE_VALUES];
static unsigned int conv;
static size_t ibs = DEFAULT_BLOCK, obs = DEFAULT_BLOCK, cbs;
static unsigned char *obuf;
static size_t ofill;
static unsigned char *cbuf;
static size_t cfill;
static int in_fd = STDIN_FILENO, out_fd = STDOUT_FILENO;
static const char *in_name = "standard input";
static const char *out_name = "standard output";

/* A size is a number with b, k or xN factors, as in "2x512" or "4k". */
static uintmax_t
parse_size(const char *text)
{
    uintmax_t total = 1;

    for (;;) {
        char *end;
        uintmax_t value;

        errno = 0;
        value = strtoumax(text, &end, 10);
        if (end == text || errno == ERANGE || *text == '-')
            xn_errx(EXIT_FAILURE, "%s: invalid number", text);
        if (*end == 'b') {
            value *= 512;
            end++;
        } else if (*end == 'k') {
            value *= 1024;
            end++;
        }
        if (value != 0 && total > UINTMAX_MAX / value)
            xn_errx(EXIT_FAILURE, "%s: number too large", text);
        total *= value;
        if (*end == '\0')
            return total;
        if (*end != 'x')
            xn_errx(EXIT_FAILURE, "%s: invalid number", text);
        text = end + 1;
    }
}

static unsigned int
parse_conv(const char *text)
{
    static const struct {
        const char *name;
        unsigned int bit;
    } names[] = {
        { "ascii", CONV_ASCII }, { "ebcdic", CONV_EBCDIC },
        { "ibm", CONV_IBM }, { "block", CONV_BLOCK },
        { "unblock", CONV_UNBLOCK }, { "lcase", CONV_LCASE },
        { "ucase", CONV_UCASE }, { "swab", CONV_SWAB },
        { "noerror", CONV_NOERROR }, { "notrunc", CONV_NOTRUNC },
        { "sync", CONV_SYNC }
    };
    unsigned int bits = 0;

    while (*text != '\0') {
        size_t len = strcspn(text, ",");
        size_t i, count = sizeof(names) / sizeof(names[0]);

        for (i = 0; i < count; i++) {
            if (strlen(names[i].name) == len &&
                strncmp(names[i].name, text, len) == 0) {
                bits |= names[i].bit;
                break;
            }
        }
        if (i == count)
            xn_errx(EXIT_FAILURE, "%.*s: unknown conversion", (int)len, text);
        text += len;
        if (*text == ',')
            text++;
    }
    return bits;
}

static void
report(void)
{
    (void)fprintf(stderr, "%ju+%ju records in\n%ju+%ju records out\n",
                  stats.in_full, stats.in_part,
                  stats.out_full, stats.out_part);
    if (stats.truncated > 0)
        (void)fprintf(stderr, "%ju truncated %s\n", stats.truncated,
                      (stats.truncated == 1) ? "record" : "records");
}

static void
on_interrupt(int sig)
{
    (void)sig;
    interrupted = 1;
}

static void
write_block(const unsigned char *data, size_t len)
{
    if (xn_writeall(out_fd, data, len) == -1) {
        xn_warn("%s", out_name);
        report();
        exit(EXIT_FAILURE);
    }
    if (len == obs)
        stats.out_full++;
    else
        stats.out_part++;
}

/* The last conversion stage: table lookup, then output blocking. */
static void
put_final(const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        obuf[ofill++] = final_table[data[i]];
        if (ofill == obs) {
            write_block(obuf, obs);
            ofill = 0;
        }
    }
}

/* conv=block: newline-terminated records become fixed-length ones. */
static void
stage_block(const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            finish_block();
        } else if (cfill < cbs) {
            cbuf[cfill++] = data[i];
        } else if (cfill == cbs) {
            cfill++;
            stats.truncated++;
        }
    }
}

static void
finish_block(void)
{
    size_t used = (cfill > cbs) ? cbs : cfill;

    memset(cbuf + used, ' ', cbs - used);
    put_final(cbuf, cbs);
    cfill = 0;
}

/* conv=unblock: fixed-length records lose trailing spaces, gain a newline. */
static void
stage_unblock(const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        cbuf[cfill++] = data[i];
        if (cfill == cbs)
            finish_unblock();
    }
}

static void
finish_unblock(void)
{
    static const unsigned char newline = '\n';
    size_t used = cfill;

    while (used > 0 && cbuf[used - 1] == ' ')
        used--;
    put_final(cbuf, used);
    put_final(&newline, 1);
    cfill = 0;
}

static void
process(unsigned char *data, size_t len)
{
    if ((conv & CONV_SWAB) != 0) {
        for (size_t i = 0; i + 1 < len; i += 2) {
            unsigned char byte = data[i];

            data[i] = data[i + 1];
            data[i + 1] = byte;
        }
    }
    for (size_t i = 0; i < len; i++)
        data[i] = input_table[data[i]];

    if ((conv & CONV_BLOCK) != 0)
        stage_block(data, len);
    else if ((conv & CONV_UNBLOCK) != 0)
        stage_unblock(data, len);
    else
        put_final(data, len);
}

static void
skip_input(uintmax_t blocks)
{
    unsigned char *buf;

    if (blocks == 0)
        return;
    if (lseek(in_fd, (off_t)(blocks * ibs), SEEK_CUR) != (off_t)-1)
        return;
    buf = xn_malloc(ibs);
    for (uintmax_t i = 0; i < blocks; i++) {
        ssize_t got;

        do {
            got = read(in_fd, buf, ibs);
        } while (got == -1 && errno == EINTR);
        if (got == -1)
            xn_err(EXIT_FAILURE, "%s", in_name);
        if (got == 0)
            break;
    }
    free(buf);
}

static void
seek_output(uintmax_t blocks)
{
    unsigned char *buf;

    if (blocks == 0)
        return;
    if (lseek(out_fd, (off_t)(blocks * obs), SEEK_CUR) != (off_t)-1)
        return;
    buf = xn_malloc(obs);
    memset(buf, 0, obs);
    for (uintmax_t i = 0; i < blocks; i++) {
        if (xn_writeall(out_fd, buf, obs) == -1)
            xn_err(EXIT_FAILURE, "%s", out_name);
    }
    free(buf);
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [if=file] [of=file] [ibs=n] [obs=n] "
                  "[bs=n] [cbs=n] [skip=n] [seek=n] [count=n] [conv=list]\n",
                  xn_progname());
    exit(EXIT_FAILURE);
}

/* Copy input to output, with conversions, in the requested block sizes. */
int
main(int argc, char **argv)
{
    struct sigaction action;
    uintmax_t skip = 0, seek = 0, count = 0;
    int have_count = 0, have_bs = 0;
    int direct;
    unsigned char *ibuf;

    xn_init(argv);
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = strchr(arg, '=');
        size_t len;

        if (value == NULL)
            usage();
        len = (size_t)(value - arg);
        value++;

#define IS(name) (len == sizeof(name) - 1 && strncmp(arg, name, len) == 0)
        if (IS("if")) {
            in_name = value;
        } else if (IS("of")) {
            out_name = value;
        } else if (IS("ibs")) {
            ibs = (size_t)parse_size(value);
        } else if (IS("obs")) {
            obs = (size_t)parse_size(value);
        } else if (IS("bs")) {
            ibs = obs = (size_t)parse_size(value);
            have_bs = 1;
        } else if (IS("cbs")) {
            cbs = (size_t)parse_size(value);
        } else if (IS("skip")) {
            skip = parse_size(value);
        } else if (IS("seek")) {
            seek = parse_size(value);
        } else if (IS("count")) {
            count = parse_size(value);
            have_count = 1;
        } else if (IS("conv")) {
            conv |= parse_conv(value);
        } else {
            usage();
        }
#undef IS
    }

    if (ibs == 0 || obs == 0)
        xn_errx(EXIT_FAILURE, "block size must not be zero");
    if (((conv & CONV_ASCII) != 0) + ((conv & CONV_EBCDIC) != 0) +
        ((conv & CONV_IBM) != 0) > 1 ||
        ((conv & CONV_BLOCK) != 0 && (conv & CONV_UNBLOCK) != 0) ||
        ((conv & CONV_LCASE) != 0 && (conv & CONV_UCASE) != 0))
        xn_errx(EXIT_FAILURE, "conflicting conversions");
    if ((conv & (CONV_BLOCK | CONV_UNBLOCK)) != 0 && cbs == 0)
        xn_errx(EXIT_FAILURE, "block and unblock need cbs=");

    for (int byte = 0; byte < BYTE_VALUES; byte++) {
        unsigned char value = (unsigned char)byte;

        if ((conv & CONV_LCASE) != 0)
            value = (unsigned char)tolower(value);
        else if ((conv & CONV_UCASE) != 0)
            value = (unsigned char)toupper(value);
        input_table[byte] = value;
        final_table[byte] = (unsigned char)byte;
    }
    if ((conv & CONV_ASCII) != 0) {
        unsigned char from_ebcdic[BYTE_VALUES];
        unsigned char cased[BYTE_VALUES];

        for (int byte = 0; byte < BYTE_VALUES; byte++)
            from_ebcdic[to_ebcdic[byte]] = (unsigned char)byte;
        memcpy(cased, input_table, sizeof(cased));
        for (int byte = 0; byte < BYTE_VALUES; byte++)
            input_table[byte] = cased[from_ebcdic[byte]];
    }
    if ((conv & (CONV_EBCDIC | CONV_IBM)) != 0)
        memcpy(final_table, to_ebcdic, sizeof(final_table));
    if ((conv & CONV_IBM) != 0) {
        size_t n = sizeof(ibm_changes) / sizeof(ibm_changes[0]);

        for (size_t i = 0; i < n; i++)
            final_table[ibm_changes[i].from] = ibm_changes[i].to;
    }

    if (strcmp(in_name, "standard input") != 0 &&
        (in_fd = open(in_name, O_RDONLY)) == -1)
        xn_err(EXIT_FAILURE, "%s", in_name);
    if (strcmp(out_name, "standard output") != 0) {
        struct stat st;

        out_fd = open(out_name, O_WRONLY | O_CREAT, CREATE_MODE);
        if (out_fd == -1)
            xn_err(EXIT_FAILURE, "%s", out_name);
        if ((conv & CONV_NOTRUNC) == 0 && fstat(out_fd, &st) == 0 &&
            S_ISREG(st.st_mode) &&
            ftruncate(out_fd, (off_t)(seek * obs)) == -1)
            xn_err(EXIT_FAILURE, "%s", out_name);
    }

    /* bs= alone means each input block becomes one output block as read. */
    direct = have_bs && (conv & ~(CONV_NOERROR | CONV_NOTRUNC |
                                  CONV_SYNC)) == 0;

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_interrupt;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);

    skip_input(skip);
    seek_output(seek);

    ibuf = xn_malloc(ibs);
    obuf = xn_malloc(obs);
    if ((conv & (CONV_BLOCK | CONV_UNBLOCK)) != 0)
        cbuf = xn_malloc(cbs);

    for (uintmax_t done = 0; !have_count || done < count; done++) {
        ssize_t got = read(in_fd, ibuf, ibs);

        if (interrupted)
            break;
        if (got == -1) {
            if (errno == EINTR) {
                done--;
                continue;
            }
            xn_warn("%s", in_name);
            report();
            if ((conv & CONV_NOERROR) == 0)
                return EXIT_FAILURE;
            (void)lseek(in_fd, (off_t)ibs, SEEK_CUR);
            if ((conv & CONV_SYNC) == 0)
                continue;
            got = 0;
            stats.in_part++;
        } else if (got == 0) {
            break;
        } else if ((size_t)got == ibs) {
            stats.in_full++;
        } else {
            stats.in_part++;
        }

        if ((size_t)got < ibs && (conv & CONV_SYNC) != 0) {
            int fill = ((conv & (CONV_BLOCK | CONV_UNBLOCK)) != 0) ? ' ' : 0;

            memset(ibuf + got, fill, ibs - (size_t)got);
            got = (ssize_t)ibs;
        }

        if (direct) {
            size_t saved = obs;

            /* Report a short block against the full-block count correctly. */
            obs = ibs;
            write_block(ibuf, (size_t)got);
            obs = saved;
        } else {
            process(ibuf, (size_t)got);
        }
    }

    if ((conv & CONV_BLOCK) != 0 && cfill > 0)
        finish_block();
    if ((conv & CONV_UNBLOCK) != 0 && cfill > 0)
        finish_unblock();
    if (ofill > 0)
        write_block(obuf, ofill);
    report();

    if (interrupted) {
        (void)signal(SIGINT, SIG_DFL);
        (void)raise(SIGINT);
    }
    if (out_fd != STDOUT_FILENO && close(out_fd) == -1)
        xn_err(EXIT_FAILURE, "%s", out_name);
    return EXIT_SUCCESS;
}
