/* expr - evaluate arguments as an expression.
 * SPDX-License-Identifier: MIT */
#include <inttypes.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xn.h"

#define EXIT_FALSE 1
#define EXIT_INVALID 2
#define EXIT_ERROR 3
#define NUMBER_MAX 32

static void invalid(const char *);
static int to_int(const char *, intmax_t *);
static char *from_int(intmax_t);
static int is_null(const char *);
static int match_op(const char *);
static char *match_regex(const char *, const char *);
static intmax_t arith_operand(const char *);
static char *parse_or(void);
static char *parse_and(void);
static char *parse_compare(void);
static char *parse_add(void);
static char *parse_mul(void);
static char *parse_match(void);
static char *parse_primary(void);

static char **tokens;

static void
invalid(const char *what)
{
    xn_errx(EXIT_INVALID, "%s", what);
}

static int
to_int(const char *text, intmax_t *value)
{
    const char *ptr = text;

    if (*ptr == '-')
        ptr++;
    if (*ptr == '\0')
        return 0;
    for (; *ptr != '\0'; ptr++) {
        if (*ptr < '0' || *ptr > '9')
            return 0;
    }
    return xn_parse_int(text, value) == 0;
}

static char *
from_int(intmax_t value)
{
    char *text = xn_malloc(NUMBER_MAX);

    (void)snprintf(text, NUMBER_MAX, "%jd", value);
    return text;
}

static int
is_null(const char *text)
{
    intmax_t value;

    return text[0] == '\0' || (to_int(text, &value) && value == 0);
}

static int
match_op(const char *op)
{
    if (*tokens != NULL && strcmp(*tokens, op) == 0) {
        tokens++;
        return 1;
    }
    return 0;
}

/*
 * Match a basic regular expression, implicitly anchored at the start. With
 * a subexpression the result is its text; otherwise, the length of the
 * match in characters.
 */
static char *
match_regex(const char *text, const char *pattern)
{
    regex_t regex;
    regmatch_t match[2];
    size_t len = strlen(pattern);
    char *anchored = xn_malloc(len + 2);
    char *result;
    int code;

    anchored[0] = '^';
    memcpy(anchored + 1, pattern, len + 1);
    if ((code = regcomp(&regex, anchored, 0)) != 0) {
        char message[256];

        (void)regerror(code, &regex, message, sizeof(message));
        xn_errx(EXIT_INVALID, "%s: %s", pattern, message);
    }
    free(anchored);

    if (regexec(&regex, text, 2, match, 0) != 0) {
        result = (regex.re_nsub > 0) ? xn_strdup("") : from_int(0);
    } else if (regex.re_nsub > 0) {
        size_t size = 0;

        if (match[1].rm_so != -1)
            size = (size_t)(match[1].rm_eo - match[1].rm_so);
        result = xn_malloc(size + 1);
        if (size > 0)
            memcpy(result, text + match[1].rm_so, size);
        result[size] = '\0';
    } else {
        size_t size = (size_t)match[0].rm_eo;
        char *copy = xn_malloc(size + 1);
        size_t chars;

        memcpy(copy, text, size);
        copy[size] = '\0';
        chars = mbstowcs(NULL, copy, 0);
        free(copy);
        result = from_int((chars != (size_t)-1) ? (intmax_t)chars
                                                : (intmax_t)size);
    }
    regfree(&regex);
    return result;
}

static intmax_t
arith_operand(const char *text)
{
    intmax_t value;

    if (!to_int(text, &value))
        xn_errx(EXIT_INVALID, "%s: non-numeric argument", text);
    return value;
}

static char *
parse_or(void)
{
    char *left = parse_and();

    while (match_op("|")) {
        char *right = parse_and();

        if (is_null(left))
            left = is_null(right) ? from_int(0) : right;
    }
    return left;
}

static char *
parse_and(void)
{
    char *left = parse_compare();

    while (match_op("&")) {
        char *right = parse_compare();

        if (is_null(left) || is_null(right))
            left = from_int(0);
    }
    return left;
}

static char *
parse_compare(void)
{
    char *left = parse_add();

    for (;;) {
        static const char *const ops[] = {
            "=", "!=", ">", ">=", "<", "<=", NULL
        };
        const char *op = NULL;
        intmax_t a, b;
        char *right;
        int order, result;

        for (size_t i = 0; ops[i] != NULL; i++) {
            if (match_op(ops[i])) {
                op = ops[i];
                break;
            }
        }
        if (op == NULL)
            return left;
        right = parse_add();

        if (to_int(left, &a) && to_int(right, &b))
            order = (a > b) - (a < b);
        else
            order = strcoll(left, right);

        if (strcmp(op, "=") == 0)
            result = (order == 0);
        else if (strcmp(op, "!=") == 0)
            result = (order != 0);
        else if (strcmp(op, ">") == 0)
            result = (order > 0);
        else if (strcmp(op, ">=") == 0)
            result = (order >= 0);
        else if (strcmp(op, "<") == 0)
            result = (order < 0);
        else
            result = (order <= 0);
        left = from_int(result);
    }
}

static char *
parse_add(void)
{
    char *left = parse_mul();

    for (;;) {
        int add;
        intmax_t a, b;

        if (match_op("+"))
            add = 1;
        else if (match_op("-"))
            add = 0;
        else
            return left;

        a = arith_operand(left);
        b = arith_operand(parse_mul());
        if (add ? ((b > 0 && a > INTMAX_MAX - b) ||
                   (b < 0 && a < INTMAX_MIN - b))
                : ((b < 0 && a > INTMAX_MAX + b) ||
                   (b > 0 && a < INTMAX_MIN + b)))
            xn_errx(EXIT_ERROR, "integer overflow");
        left = from_int(add ? a + b : a - b);
    }
}

static char *
parse_mul(void)
{
    char *left = parse_match();

    for (;;) {
        char op;
        intmax_t a, b, result;

        if (match_op("*"))
            op = '*';
        else if (match_op("/"))
            op = '/';
        else if (match_op("%"))
            op = '%';
        else
            return left;

        a = arith_operand(left);
        b = arith_operand(parse_match());
        if (op == '*') {
            if ((b > 0 && (a > INTMAX_MAX / b || a < INTMAX_MIN / b)) ||
                (b < -1 && (a < INTMAX_MAX / b || a > INTMAX_MIN / b)) ||
                (b == -1 && a == INTMAX_MIN))
                xn_errx(EXIT_ERROR, "integer overflow");
            result = a * b;
        } else {
            if (b == 0)
                invalid("division by zero");
            if (a == INTMAX_MIN && b == -1)
                xn_errx(EXIT_ERROR, "integer overflow");
            result = (op == '/') ? a / b : a % b;
        }
        left = from_int(result);
    }
}

static char *
parse_match(void)
{
    char *left = parse_primary();

    while (match_op(":"))
        left = match_regex(left, parse_primary());
    return left;
}

static char *
parse_primary(void)
{
    char *token;

    if (*tokens == NULL)
        invalid("syntax error: missing argument");
    if (match_op("(")) {
        char *value = parse_or();

        if (!match_op(")"))
            invalid("syntax error: expected )");
        return value;
    }
    token = *tokens++;
    return token;
}

/* Exit 0 for a non-null non-zero result, 1 otherwise, 2 or 3 on error. */
int
main(int argc, char **argv)
{
    char *result;

    xn_init(argv);
    if (argc > 1 && strcmp(argv[1], "--") == 0)
        argv++;
    tokens = argv + 1;
    result = parse_or();
    if (*tokens != NULL)
        invalid("syntax error: unexpected argument");
    (void)puts(result);
    if (xn_finish(EXIT_SUCCESS) != EXIT_SUCCESS)
        return EXIT_ERROR;
    return is_null(result) ? EXIT_FALSE : EXIT_SUCCESS;
}
