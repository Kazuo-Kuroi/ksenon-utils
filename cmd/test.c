/*
 * test, [ - evaluate expression. SPDX-License-Identifier: MIT
 *
 * Up to four arguments are decided by the argument-count rules in POSIX,
 * which is what makes "test ! = x" and similar forms unambiguous. Longer
 * expressions use the XSI -a, -o and parenthesis grammar.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xn.h"

#define EXIT_ERROR 2

static void syntax(const char *);
static intmax_t integer(const char *);
static int is_unary(const char *);
static int is_binary(const char *);
static int unary(const char *, const char *);
static int binary(const char *, const char *, const char *);
static int eval_args(char **, int);
static int parse_or(void);
static int parse_and(void);
static int parse_not(void);
static int parse_primary(void);

static char **cursor;
static char **limit;

static void
syntax(const char *what)
{
    xn_errx(EXIT_ERROR, "%s", what);
}

static intmax_t
integer(const char *text)
{
    intmax_t value;

    if (xn_parse_int(text, &value) == -1)
        xn_errx(EXIT_ERROR, "%s: integer expression expected", text);
    return value;
}

static int
is_unary(const char *op)
{
    return op[0] == '-' && op[1] != '\0' && op[2] == '\0' &&
           strchr("bcdefghLnprSstuwxz", op[1]) != NULL;
}

static int
is_binary(const char *op)
{
    static const char *const ops[] = {
        "=", "!=", "-eq", "-ne", "-gt", "-ge", "-lt", "-le", NULL
    };

    for (size_t i = 0; ops[i] != NULL; i++) {
        if (strcmp(op, ops[i]) == 0)
            return 1;
    }
    return 0;
}

static int
unary(const char *op, const char *arg)
{
    struct stat st;
    int code = op[1];

    switch (code) {
    case 'n':
        return arg[0] != '\0';
    case 'z':
        return arg[0] == '\0';
    case 't':
        return isatty((int)integer(arg));
    case 'r':
        return access(arg, R_OK) == 0;
    case 'w':
        return access(arg, W_OK) == 0;
    case 'x':
        return access(arg, X_OK) == 0;
    case 'h':
    case 'L':
        return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);
    default:
        break;
    }

    if (stat(arg, &st) == -1)
        return 0;
    switch (code) {
    case 'b':
        return S_ISBLK(st.st_mode);
    case 'c':
        return S_ISCHR(st.st_mode);
    case 'd':
        return S_ISDIR(st.st_mode);
    case 'e':
        return 1;
    case 'f':
        return S_ISREG(st.st_mode);
    case 'g':
        return (st.st_mode & S_ISGID) != 0;
    case 'p':
        return S_ISFIFO(st.st_mode);
    case 'S':
        return S_ISSOCK(st.st_mode);
    case 's':
        return st.st_size > 0;
    case 'u':
        return (st.st_mode & S_ISUID) != 0;
    default:
        syntax("unknown unary operator");
    }
    return 0;
}

static int
binary(const char *left, const char *op, const char *right)
{
    intmax_t a, b;

    if (strcmp(op, "=") == 0)
        return strcmp(left, right) == 0;
    if (strcmp(op, "!=") == 0)
        return strcmp(left, right) != 0;
    a = integer(left);
    b = integer(right);
    if (strcmp(op, "-eq") == 0)
        return a == b;
    if (strcmp(op, "-ne") == 0)
        return a != b;
    if (strcmp(op, "-gt") == 0)
        return a > b;
    if (strcmp(op, "-ge") == 0)
        return a >= b;
    if (strcmp(op, "-lt") == 0)
        return a < b;
    return a <= b;
}

static int
parse_or(void)
{
    int value = parse_and();

    while (cursor < limit && strcmp(*cursor, "-o") == 0) {
        int right;

        cursor++;
        right = parse_and();
        value = value || right;
    }
    return value;
}

static int
parse_and(void)
{
    int value = parse_not();

    while (cursor < limit && strcmp(*cursor, "-a") == 0) {
        int right;

        cursor++;
        right = parse_not();
        value = value && right;
    }
    return value;
}

static int
parse_not(void)
{
    if (cursor < limit && strcmp(*cursor, "!") == 0) {
        cursor++;
        return !parse_not();
    }
    return parse_primary();
}

static int
parse_primary(void)
{
    const char *token;

    if (cursor >= limit)
        syntax("argument expected");
    token = *cursor;

    if (limit - cursor >= 3 && is_binary(cursor[1])) {
        cursor += 3;
        return binary(token, cursor[-2], cursor[-1]);
    }
    if (strcmp(token, "(") == 0) {
        int value;

        cursor++;
        value = parse_or();
        if (cursor >= limit || strcmp(*cursor, ")") != 0)
            syntax("closing parenthesis expected");
        cursor++;
        return value;
    }
    if (is_unary(token) && limit - cursor >= 2) {
        cursor += 2;
        return unary(token, cursor[-1]);
    }
    cursor++;
    return token[0] != '\0';
}

/* Apply the POSIX rules that depend only on the number of arguments. */
static int
eval_args(char **args, int count)
{
    switch (count) {
    case 0:
        return 0;
    case 1:
        return args[0][0] != '\0';
    case 2:
        if (strcmp(args[0], "!") == 0)
            return args[1][0] == '\0';
        if (is_unary(args[0]))
            return unary(args[0], args[1]);
        break;
    case 3:
        if (is_binary(args[1]))
            return binary(args[0], args[1], args[2]);
        if (strcmp(args[0], "!") == 0)
            return !eval_args(args + 1, 2);
        if (strcmp(args[0], "(") == 0 && strcmp(args[2], ")") == 0)
            return eval_args(args + 1, 1);
        break;
    case 4:
        if (strcmp(args[0], "!") == 0)
            return !eval_args(args + 1, 3);
        if (strcmp(args[0], "(") == 0 && strcmp(args[3], ")") == 0)
            return eval_args(args + 1, 2);
        break;
    default:
        break;
    }

    cursor = args;
    limit = args + count;
    {
        int value = parse_or();

        if (cursor != limit)
            syntax("too many arguments");
        return value;
    }
}

/* Exit 0 when the expression is true, 1 when false, 2 on error. */
int
main(int argc, char **argv)
{
    xn_init(argv);
    argc--;
    argv++;
    if (strcmp(xn_progname(), "[") == 0) {
        if (argc == 0 || strcmp(argv[argc - 1], "]") != 0)
            syntax("missing ]");
        argc--;
    }
    return eval_args(argv, argc) ? EXIT_SUCCESS : EXIT_FAILURE;
}
