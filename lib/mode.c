/*
 * chmod(1) mode expression compiler. SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "xn.h"

#define BITS_ALL 07777
#define BITS_USER 04700
#define BITS_GROUP 02070
#define BITS_OTHER 01007
#define BITS_R 0444
#define BITS_W 0222
#define BITS_X 0111
#define BITS_ID 06000
#define BITS_STICKY 01000

struct mode_op {
    mode_t who;
    mode_t perm;
    char op;
    char copy;
    char cond_exec;
    char has_who;
};

struct xn_mode {
    size_t count;
    struct mode_op ops[];
};

static struct xn_mode *compile_octal(const char *);
static mode_t copied_bits(mode_t, char);

static struct xn_mode *
compile_octal(const char *text)
{
    struct xn_mode *mode;
    unsigned long value = 0;

    for (; *text != '\0'; text++) {
        if (*text < '0' || *text > '7')
            return NULL;
        value = value * 8 + (unsigned long)(*text - '0');
        if (value > BITS_ALL)
            return NULL;
    }
    mode = xn_malloc(sizeof(*mode) + sizeof(mode->ops[0]));
    mode->count = 1;
    mode->ops[0].who = BITS_ALL;
    mode->ops[0].perm = (mode_t)value;
    mode->ops[0].op = '=';
    mode->ops[0].copy = '\0';
    mode->ops[0].cond_exec = 0;
    mode->ops[0].has_who = 1;
    return mode;
}

/*
 * Parse an octal or symbolic mode. Returns NULL when the expression is
 * invalid. Every op character produces one compiled operation.
 */
struct xn_mode *
xn_mode_compile(const char *text)
{
    struct xn_mode *mode;
    const char *ptr;
    size_t limit = 1;

    if (text[0] >= '0' && text[0] <= '7')
        return compile_octal(text);

    for (ptr = text; *ptr != '\0'; ptr++) {
        if (*ptr == '+' || *ptr == '-' || *ptr == '=')
            limit++;
    }
    mode = xn_malloc(sizeof(*mode) + limit * sizeof(mode->ops[0]));
    mode->count = 0;

    ptr = text;
    for (;;) {
        mode_t who = 0;
        int has_who = 0;

        for (;; ptr++) {
            if (*ptr == 'u')
                who |= BITS_USER;
            else if (*ptr == 'g')
                who |= BITS_GROUP;
            else if (*ptr == 'o')
                who |= BITS_OTHER;
            else if (*ptr == 'a')
                who = BITS_ALL;
            else
                break;
            has_who = 1;
        }
        if (*ptr != '+' && *ptr != '-' && *ptr != '=')
            goto invalid;

        while (*ptr == '+' || *ptr == '-' || *ptr == '=') {
            struct mode_op *op = &mode->ops[mode->count++];

            op->op = *ptr++;
            op->who = has_who ? who : BITS_ALL;
            op->has_who = (char)has_who;
            op->perm = 0;
            op->copy = '\0';
            op->cond_exec = 0;
            if (*ptr == 'u' || *ptr == 'g' || *ptr == 'o') {
                op->copy = *ptr++;
                continue;
            }
            for (;; ptr++) {
                if (*ptr == 'r')
                    op->perm |= BITS_R;
                else if (*ptr == 'w')
                    op->perm |= BITS_W;
                else if (*ptr == 'x')
                    op->perm |= BITS_X;
                else if (*ptr == 'X')
                    op->cond_exec = 1;
                else if (*ptr == 's')
                    op->perm |= BITS_ID;
                else if (*ptr == 't')
                    op->perm |= BITS_STICKY;
                else
                    break;
            }
        }
        if (*ptr == '\0')
            return mode;
        if (*ptr != ',')
            goto invalid;
        ptr++;
    }

invalid:
    free(mode);
    return NULL;
}

/* Expand one permission class of the current mode into all three classes. */
static mode_t
copied_bits(mode_t cur, char source)
{
    mode_t bits;

    if (source == 'u')
        bits = (cur >> 6) & 07;
    else if (source == 'g')
        bits = (cur >> 3) & 07;
    else
        bits = cur & 07;
    return (bits << 6) | (bits << 3) | bits;
}

/*
 * Apply a compiled mode to the current st_mode. cmask is the file mode
 * creation mask, which limits operations that name no "who".
 */
mode_t
xn_mode_apply(const struct xn_mode *mode, mode_t cur, mode_t cmask)
{
    mode_t bits = cur & BITS_ALL;

    for (size_t i = 0; i < mode->count; i++) {
        const struct mode_op *op = &mode->ops[i];
        mode_t perm = op->perm;
        mode_t mask = op->has_who ? op->who : (op->who & ~cmask);
        mode_t clear = op->who;

        if (op->copy != '\0')
            perm |= copied_bits(bits, op->copy);
        if (op->cond_exec && (S_ISDIR(cur) || (bits & BITS_X) != 0))
            perm |= BITS_X;
        perm &= mask;

        /* "o=" must not clear the sticky bit, which "o" only grants. */
        if (op->who != BITS_ALL)
            clear &= ~(mode_t)BITS_STICKY;

        if (op->op == '+') {
            bits |= perm;
        } else if (op->op == '-') {
            bits &= ~perm;
        } else {
            bits &= ~clear;
            bits |= perm;
        }
    }
    return bits;
}

mode_t
xn_umask(void)
{
    mode_t mask = umask(0);

    (void)umask(mask);
    return mask;
}
