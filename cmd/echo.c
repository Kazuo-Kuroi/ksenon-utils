/* echo - write arguments to standard output. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>

#include "xn.h"

static int emit(const char *);

/*
 * Write one operand, expanding the XSI escapes. Returns 0 when "\c" asks
 * for all further output to be suppressed.
 */
static int
emit(const char *text)
{
    for (; *text != '\0'; text++) {
        unsigned char byte;

        if (*text != '\\' || text[1] == '\0') {
            (void)putchar((unsigned char)*text);
            continue;
        }
        switch (*++text) {
        case 'a':
            (void)putchar('\a');
            break;
        case 'b':
            (void)putchar('\b');
            break;
        case 'c':
            return 0;
        case 'f':
            (void)putchar('\f');
            break;
        case 'n':
            (void)putchar('\n');
            break;
        case 'r':
            (void)putchar('\r');
            break;
        case 't':
            (void)putchar('\t');
            break;
        case 'v':
            (void)putchar('\v');
            break;
        case '\\':
            (void)putchar('\\');
            break;
        case '0':
            text += xn_unescape_octal(text + 1, 3, &byte);
            (void)putchar(byte);
            break;
        default:
            (void)putchar('\\');
            (void)putchar((unsigned char)*text);
            break;
        }
    }
    return 1;
}

/*
 * Write the operands separated by spaces. The XSI form of echo accepts no
 * options at all, so "-n" and "--" are ordinary operands.
 */
int
main(int argc, char **argv)
{
    xn_init(argv);
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            (void)putchar(' ');
        if (emit(argv[i]) == 0)
            return xn_finish(EXIT_SUCCESS);
    }
    (void)putchar('\n');
    return xn_finish(EXIT_SUCCESS);
}
