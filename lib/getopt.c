/*
 * Strict POSIX getopt. SPDX-License-Identifier: MIT
 *
 * The C library's parser is avoided because some implementations permute
 * operands by default, which breaks utilities such as env and nice that
 * must leave everything after the first operand untouched.
 */
#include <stdio.h>
#include <string.h>

#include "xn.h"

char *xn_optarg;
int xn_optind = 1;
int xn_opterr = 1;
int xn_optopt;

static size_t optpos = 1;

/*
 * Return the next option character, or -1 when options are exhausted. A
 * leading ':' in the specification selects silent error reporting, with
 * ':' returned for a missing option-argument.
 */
int
xn_getopt(int argc, char *const *argv, const char *spec)
{
    const char *match;
    const char *arg;
    int silent = (spec[0] == ':');

    xn_optarg = NULL;
    if (xn_optind >= argc || argv[xn_optind] == NULL)
        return -1;
    arg = argv[xn_optind];
    if (arg[0] != '-' || arg[1] == '\0')
        return -1;
    if (optpos == 1 && strcmp(arg, "--") == 0) {
        xn_optind++;
        return -1;
    }

    xn_optopt = (unsigned char)arg[optpos++];
    match = (xn_optopt == ':') ? NULL : strchr(spec, xn_optopt);
    if (match == NULL) {
        if (arg[optpos] == '\0') {
            xn_optind++;
            optpos = 1;
        }
        if (!silent && xn_opterr)
            xn_warnx("illegal option -- %c", xn_optopt);
        return '?';
    }

    if (match[1] != ':') {
        if (arg[optpos] == '\0') {
            xn_optind++;
            optpos = 1;
        }
        return xn_optopt;
    }

    if (arg[optpos] != '\0') {
        xn_optarg = (char *)&arg[optpos];
        xn_optind++;
    } else if (++xn_optind < argc) {
        xn_optarg = argv[xn_optind++];
    } else {
        optpos = 1;
        if (silent)
            return ':';
        if (xn_opterr)
            xn_warnx("option requires an argument -- %c", xn_optopt);
        return '?';
    }
    optpos = 1;
    return xn_optopt;
}
