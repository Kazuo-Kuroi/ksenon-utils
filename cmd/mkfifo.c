/* mkfifo - make FIFO special files. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "xn.h"

#define FIFO_DEFAULT 0666

static void usage(void);

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-m mode] file...\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* Create each FIFO operand. */
int
main(int argc, char **argv)
{
    const char *mode_text = NULL;
    mode_t mode = FIFO_DEFAULT;
    int status = EXIT_SUCCESS;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "m:")) != -1) {
        if (ch != 'm')
            usage();
        mode_text = xn_optarg;
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc == 0)
        usage();

    if (mode_text != NULL) {
        struct xn_mode *compiled = xn_mode_compile(mode_text);

        if (compiled == NULL)
            xn_errx(EXIT_FAILURE, "%s: invalid mode", mode_text);
        mode = xn_mode_apply(compiled, S_IFIFO | FIFO_DEFAULT, xn_umask());
        free(compiled);
    }

    for (int i = 0; i < argc; i++) {
        if (mkfifo(argv[i], mode) == -1 ||
            (mode_text != NULL && chmod(argv[i], mode) == -1)) {
            xn_warn("%s", argv[i]);
            status = EXIT_FAILURE;
        }
    }
    return status;
}
