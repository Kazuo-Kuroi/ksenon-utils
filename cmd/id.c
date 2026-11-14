/* id - return user identity. SPDX-License-Identifier: MIT */
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "xn.h"

struct identity {
    gid_t *groups;
    size_t ngroups;
    uid_t ruid;
    uid_t euid;
    gid_t rgid;
    gid_t egid;
};

static void add_group(struct identity *, gid_t);
static void load_self(struct identity *);
static void load_user(struct identity *, const char *);
static void put_uid(uid_t, int);
static void put_gid(gid_t, int);
static void put_user_full(const char *, uid_t);
static void put_group_full(const char *, gid_t);
static void report_full(const struct identity *);
static void usage(void);

static void
add_group(struct identity *id, gid_t gid)
{
    for (size_t i = 0; i < id->ngroups; i++) {
        if (id->groups[i] == gid)
            return;
    }
    id->groups = xn_realloc(id->groups,
                            (id->ngroups + 1) * sizeof(*id->groups));
    id->groups[id->ngroups++] = gid;
}

static void
load_self(struct identity *id)
{
    int count = getgroups(0, NULL);
    gid_t *list;

    id->ruid = getuid();
    id->euid = geteuid();
    id->rgid = getgid();
    id->egid = getegid();
    add_group(id, id->rgid);
    add_group(id, id->egid);

    if (count == -1)
        xn_err(EXIT_FAILURE, "getgroups");
    list = xn_malloc((size_t)count * sizeof(*list));
    if (getgroups(count, list) == -1)
        xn_err(EXIT_FAILURE, "getgroups");
    for (int i = 0; i < count; i++)
        add_group(id, list[i]);
    free(list);
}

/* getgrouplist(3) is not POSIX, so scan the group database. */
static void
load_user(struct identity *id, const char *name)
{
    const struct passwd *pw = getpwnam(name);
    const struct group *gr;
    uintmax_t numeric;

    if (pw == NULL && xn_parse_uint(name, &numeric) == 0)
        pw = getpwuid((uid_t)numeric);
    if (pw == NULL)
        xn_errx(EXIT_FAILURE, "%s: no such user", name);

    id->ruid = id->euid = pw->pw_uid;
    id->rgid = id->egid = pw->pw_gid;
    add_group(id, pw->pw_gid);

    {
        char *user = xn_strdup(pw->pw_name);

        setgrent();
        while ((gr = getgrent()) != NULL) {
            for (char **member = gr->gr_mem; *member != NULL; member++) {
                if (strcmp(*member, user) == 0) {
                    add_group(id, gr->gr_gid);
                    break;
                }
            }
        }
        endgrent();
        free(user);
    }
}

static void
put_uid(uid_t uid, int by_name)
{
    const struct passwd *pw = by_name ? getpwuid(uid) : NULL;

    if (pw != NULL)
        (void)fputs(pw->pw_name, stdout);
    else
        (void)printf("%ju", (uintmax_t)uid);
}

static void
put_gid(gid_t gid, int by_name)
{
    const struct group *gr = by_name ? getgrgid(gid) : NULL;

    if (gr != NULL)
        (void)fputs(gr->gr_name, stdout);
    else
        (void)printf("%ju", (uintmax_t)gid);
}

static void
put_user_full(const char *label, uid_t uid)
{
    const struct passwd *pw = getpwuid(uid);

    (void)printf("%s=%ju", label, (uintmax_t)uid);
    if (pw != NULL)
        (void)printf("(%s)", pw->pw_name);
}

static void
put_group_full(const char *label, gid_t gid)
{
    const struct group *gr = getgrgid(gid);

    (void)printf("%s=%ju", label, (uintmax_t)gid);
    if (gr != NULL)
        (void)printf("(%s)", gr->gr_name);
}

static void
report_full(const struct identity *id)
{
    put_user_full("uid", id->ruid);
    (void)putchar(' ');
    put_group_full("gid", id->rgid);
    if (id->euid != id->ruid) {
        (void)putchar(' ');
        put_user_full("euid", id->euid);
    }
    if (id->egid != id->rgid) {
        (void)putchar(' ');
        put_group_full("egid", id->egid);
    }
    for (size_t i = 0; i < id->ngroups; i++) {
        if (i == 0) {
            (void)putchar(' ');
            put_group_full("groups", id->groups[0]);
        } else {
            const struct group *gr = getgrgid(id->groups[i]);

            (void)printf(",%ju", (uintmax_t)id->groups[i]);
            if (gr != NULL)
                (void)printf("(%s)", gr->gr_name);
        }
    }
    (void)putchar('\n');
}

static void
usage(void)
{
    (void)fprintf(stderr,
                  "usage: %s [user]\n"
                  "       %s -G [-n] [user]\n"
                  "       %s -g [-nr] [user]\n"
                  "       %s -u [-nr] [user]\n",
                  xn_progname(), xn_progname(), xn_progname(), xn_progname());
    exit(EXIT_FAILURE);
}

/* Report the real, effective and supplementary identity of a user. */
int
main(int argc, char **argv)
{
    struct identity id;
    int all_groups = 0, group_only = 0, user_only = 0;
    int by_name = 0, real = 0;
    int ch;

    xn_init(argv);
    while ((ch = xn_getopt(argc, argv, "Ggnru")) != -1) {
        switch (ch) {
        case 'G':
            all_groups = 1;
            break;
        case 'g':
            group_only = 1;
            break;
        case 'n':
            by_name = 1;
            break;
        case 'r':
            real = 1;
            break;
        case 'u':
            user_only = 1;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;
    if (argc > 1 || all_groups + group_only + user_only > 1 ||
        (by_name && !all_groups && !group_only && !user_only) ||
        (real && !group_only && !user_only))
        usage();

    memset(&id, 0, sizeof(id));
    if (argc == 1)
        load_user(&id, argv[0]);
    else
        load_self(&id);

    if (user_only) {
        put_uid(real ? id.ruid : id.euid, by_name);
        (void)putchar('\n');
    } else if (group_only) {
        put_gid(real ? id.rgid : id.egid, by_name);
        (void)putchar('\n');
    } else if (all_groups) {
        for (size_t i = 0; i < id.ngroups; i++) {
            if (i > 0)
                (void)putchar(' ');
            put_gid(id.groups[i], by_name);
        }
        (void)putchar('\n');
    } else {
        report_full(&id);
    }
    free(id.groups);
    return xn_finish(EXIT_SUCCESS);
}
