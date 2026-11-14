/* ls - list directory contents. SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "xn.h"

#define SIX_MONTHS ((time_t)15778476)
#define DEFAULT_WIDTH 80
#define COLUMN_GAP 2
#define LINK_BUFSIZE 4096

enum layout {
    LAYOUT_ONE,
    LAYOUT_COLUMNS,
    LAYOUT_ACROSS,
    LAYOUT_COMMAS,
    LAYOUT_LONG
};

enum hidden {
    HIDE_DOT,
    HIDE_DOTDOT,
    HIDE_NONE
};

enum sort_key {
    SORT_NAME,
    SORT_TIME,
    SORT_NONE
};

enum time_field {
    TIME_MODIFIED,
    TIME_ACCESSED,
    TIME_CHANGED
};

struct entry {
    char *name;
    char *path;
    char *label;
    struct stat st;
    size_t width;
    int stat_ok;
};

/* One open directory of a listing; -R keeps a stack of these on the heap. */
struct level {
    struct entry *entries;
    size_t count;
    size_t next;
    struct xn_dirstep step;
    dev_t dev;
    ino_t ino;
    int entered;
};

static int stat_operand(const char *, struct stat *);
static int stat_child(const char *, struct stat *);
static time_t entry_time(const struct stat *);
static int compare_entries(const void *, const void *);
static void sort_entries(struct entry *, size_t);
static uintmax_t blocks_of(const struct stat *);
static char *sanitize(const char *);
static size_t display_width(const char *);
static char classify_char(const struct stat *);
static void build_labels(struct entry *, size_t);
static void mode_string(const struct stat *, char *);
static const char *user_name(uid_t);
static const char *group_name(gid_t);
static void print_long(const struct entry *, size_t);
static void print_columns(const struct entry *, size_t);
static void print_commas(const struct entry *, size_t);
static void print_entries(struct entry *, size_t, int);
static void free_entries(struct entry *, size_t);
static int in_cycle(const struct stat *);
static void open_level(const char *, const char *, int);
static void close_level(void);
static void list_tree(const char *, const char *, int);
static void usage(void);

static enum layout layout;
static enum hidden hidden = HIDE_DOT;
static enum sort_key sort_key = SORT_NAME;
static enum time_field time_field = TIME_MODIFIED;
static int follow_all, follow_operands;
static int recursive, dirs_as_files, classify, slash_dirs;
static int show_inode, show_blocks, numeric_ids, hide_owner, hide_group;
static int reverse, kilobytes, quote_nonprint;
static int term_width = DEFAULT_WIDTH;
static int status = EXIT_SUCCESS;
static time_t now;
static struct level *levels;
static size_t nlevels, levels_cap;

static int
stat_operand(const char *path, struct stat *st)
{
    if (follow_all || follow_operands ||
        (!dirs_as_files && !classify && layout != LAYOUT_LONG)) {
        if (stat(path, st) == 0)
            return 0;
        if (errno != ENOENT)
            return -1;
    }
    return lstat(path, st);
}

/* Children are reached by name, from inside their directory. */
static int
stat_child(const char *name, struct stat *st)
{
    if (follow_all && stat(name, st) == 0)
        return 0;
    return lstat(name, st);
}

static time_t
entry_time(const struct stat *st)
{
    if (time_field == TIME_ACCESSED)
        return st->st_atime;
    if (time_field == TIME_CHANGED)
        return st->st_ctime;
    return st->st_mtime;
}

static int
compare_entries(const void *left, const void *right)
{
    const struct entry *a = left;
    const struct entry *b = right;
    int order = 0;

    if (sort_key == SORT_TIME) {
        time_t ta = entry_time(&a->st);
        time_t tb = entry_time(&b->st);

        if (ta != tb)
            order = (ta > tb) ? -1 : 1;
    }
    if (order == 0)
        order = strcoll(a->name, b->name);
    return reverse ? -order : order;
}

static void
sort_entries(struct entry *entries, size_t count)
{
    /* An empty directory has a NULL array, which qsort must not receive. */
    if (sort_key != SORT_NONE && count > 1)
        qsort(entries, count, sizeof(*entries), compare_entries);
}

/* st_blocks counts 512-byte units on the platforms supported here. */
static uintmax_t
blocks_of(const struct stat *st)
{
    uintmax_t blocks = (uintmax_t)st->st_blocks;

    return kilobytes ? (blocks + 1) / 2 : blocks;
}

/* Replace non-printable characters with '?' for terminal display. */
static char *
sanitize(const char *name)
{
    char *out = xn_strdup(name);
    size_t len = strlen(name);
    size_t in = 0, pos = 0;
    mbstate_t state;

    memset(&state, 0, sizeof(state));
    while (in < len) {
        wchar_t wc;
        size_t used = mbrtowc(&wc, name + in, len - in, &state);

        if (used == (size_t)-1 || used == (size_t)-2 || used == 0) {
            memset(&state, 0, sizeof(state));
            out[pos++] = '?';
            in++;
        } else if (!iswprint((wint_t)wc)) {
            out[pos++] = '?';
            in += used;
        } else {
            memcpy(out + pos, name + in, used);
            pos += used;
            in += used;
        }
    }
    out[pos] = '\0';
    return out;
}

static size_t
display_width(const char *text)
{
    size_t chars = mbstowcs(NULL, text, 0);

    return (chars != (size_t)-1) ? chars : strlen(text);
}

static char
classify_char(const struct stat *st)
{
    if (S_ISDIR(st->st_mode))
        return (classify || slash_dirs) ? '/' : '\0';
    if (!classify)
        return '\0';
    if (S_ISFIFO(st->st_mode))
        return '|';
    if (S_ISLNK(st->st_mode))
        return '@';
    if (S_ISSOCK(st->st_mode))
        return '=';
    if (S_ISREG(st->st_mode) && (st->st_mode & 0111) != 0)
        return '*';
    return '\0';
}

/* Compose each entry's visible name, including any -i and -s prefixes. */
static void
build_labels(struct entry *entries, size_t count)
{
    int inode_width = 0, block_width = 0;
    char number[32];

    for (size_t i = 0; i < count; i++) {
        int width;

        width = snprintf(number, sizeof(number), "%ju",
                         (uintmax_t)entries[i].st.st_ino);
        if (width > inode_width)
            inode_width = width;
        width = snprintf(number, sizeof(number), "%ju",
                         blocks_of(&entries[i].st));
        if (width > block_width)
            block_width = width;
    }

    for (size_t i = 0; i < count; i++) {
        struct entry *ent = &entries[i];
        char *shown = quote_nonprint ? sanitize(ent->name)
                                     : xn_strdup(ent->name);
        char mark = ent->stat_ok ? classify_char(&ent->st) : '\0';
        size_t size = strlen(shown) + 2 * sizeof(number) + 4;
        size_t len = 0;

        ent->label = xn_malloc(size);
        if (show_inode)
            len += (size_t)snprintf(ent->label + len, size - len, "%*ju ",
                                    (layout == LAYOUT_COMMAS) ? 0 : inode_width,
                                    (uintmax_t)ent->st.st_ino);
        if (show_blocks && layout != LAYOUT_LONG)
            len += (size_t)snprintf(ent->label + len, size - len, "%*ju ",
                                    (layout == LAYOUT_COMMAS) ? 0 : block_width,
                                    blocks_of(&ent->st));
        len += (size_t)snprintf(ent->label + len, size - len, "%s", shown);
        if (mark != '\0') {
            ent->label[len++] = mark;
            ent->label[len] = '\0';
        }
        ent->width = display_width(ent->label);
        free(shown);
    }
}

static void
mode_string(const struct stat *st, char *out)
{
    mode_t mode = st->st_mode;

    if (S_ISDIR(mode))
        out[0] = 'd';
    else if (S_ISBLK(mode))
        out[0] = 'b';
    else if (S_ISCHR(mode))
        out[0] = 'c';
    else if (S_ISLNK(mode))
        out[0] = 'l';
    else if (S_ISFIFO(mode))
        out[0] = 'p';
    else if (S_ISSOCK(mode))
        out[0] = 's';
    else
        out[0] = '-';
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_ISUID) ? ((mode & S_IXUSR) ? 's' : 'S')
                              : ((mode & S_IXUSR) ? 'x' : '-');
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_ISGID) ? ((mode & S_IXGRP) ? 's' : 'S')
                              : ((mode & S_IXGRP) ? 'x' : '-');
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_ISVTX) ? ((mode & S_IXOTH) ? 't' : 'T')
                              : ((mode & S_IXOTH) ? 'x' : '-');
    out[10] = ' ';
    out[11] = '\0';
}

/* A one-entry cache: listings repeat the same owner for long runs. */
static const char *
user_name(uid_t uid)
{
    static uid_t cached_uid;
    static char name[64];
    static int valid;

    if (!valid || uid != cached_uid) {
        const struct passwd *entry = numeric_ids ? NULL : getpwuid(uid);

        if (entry != NULL)
            (void)snprintf(name, sizeof(name), "%s", entry->pw_name);
        else
            (void)snprintf(name, sizeof(name), "%ju", (uintmax_t)uid);
        cached_uid = uid;
        valid = 1;
    }
    return name;
}

static const char *
group_name(gid_t gid)
{
    static gid_t cached_gid;
    static char name[64];
    static int valid;

    if (!valid || gid != cached_gid) {
        const struct group *entry = numeric_ids ? NULL : getgrgid(gid);

        if (entry != NULL)
            (void)snprintf(name, sizeof(name), "%s", entry->gr_name);
        else
            (void)snprintf(name, sizeof(name), "%ju", (uintmax_t)gid);
        cached_gid = gid;
        valid = 1;
    }
    return name;
}

static void
print_long(const struct entry *entries, size_t count)
{
    int link_width = 1, owner_width = 1, group_width = 1, size_width = 1;
    int block_width = 1;
    char number[64];

    for (size_t i = 0; i < count; i++) {
        const struct stat *st = &entries[i].st;
        int width;

        width = snprintf(number, sizeof(number), "%ju",
                         (uintmax_t)st->st_nlink);
        if (width > link_width)
            link_width = width;
        width = (int)strlen(user_name(st->st_uid));
        if (width > owner_width)
            owner_width = width;
        width = (int)strlen(group_name(st->st_gid));
        if (width > group_width)
            group_width = width;
        if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode))
            width = snprintf(number, sizeof(number), "%lu, %lu",
                             xn_dev_major(st->st_rdev),
                             xn_dev_minor(st->st_rdev));
        else
            width = snprintf(number, sizeof(number), "%ju",
                             (uintmax_t)st->st_size);
        if (width > size_width)
            size_width = width;
        width = snprintf(number, sizeof(number), "%ju", blocks_of(st));
        if (width > block_width)
            block_width = width;
    }

    for (size_t i = 0; i < count; i++) {
        const struct entry *ent = &entries[i];
        const struct stat *st = &ent->st;
        const struct tm *tm;
        time_t stamp = entry_time(st);
        char mode[12];
        char date[64];

        if (show_blocks)
            (void)printf("%*ju ", block_width, blocks_of(st));
        mode_string(st, mode);
        (void)printf("%s %*ju ", mode, link_width, (uintmax_t)st->st_nlink);
        if (!hide_owner)
            (void)printf("%-*s ", owner_width, user_name(st->st_uid));
        if (!hide_group)
            (void)printf("%-*s ", group_width, group_name(st->st_gid));
        if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode)) {
            (void)snprintf(number, sizeof(number), "%lu, %lu",
                           xn_dev_major(st->st_rdev),
                           xn_dev_minor(st->st_rdev));
            (void)printf("%*s ", size_width, number);
        } else {
            (void)printf("%*ju ", size_width, (uintmax_t)st->st_size);
        }

        tm = localtime(&stamp);
        date[0] = '\0';
        if (tm != NULL) {
            int recent = stamp > now - SIX_MONTHS && stamp <= now;

            (void)strftime(date, sizeof(date),
                           recent ? "%b %e %H:%M" : "%b %e  %Y", tm);
        }
        (void)printf("%s %s", date, ent->label);
        if (S_ISLNK(st->st_mode)) {
            char target[LINK_BUFSIZE];
            ssize_t len = readlink(ent->name, target, sizeof(target) - 1);

            if (len != -1) {
                target[len] = '\0';
                (void)printf(" -> %s", target);
            }
        }
        (void)putchar('\n');
    }
}

/* Lay entries out in the most columns that fit the terminal width. */
static void
print_columns(const struct entry *entries, size_t count)
{
    size_t widest = 0;
    size_t columns, rows;

    if (count == 0)
        return;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].width > widest)
            widest = entries[i].width;
    }
    columns = (size_t)term_width / (widest + COLUMN_GAP);
    if (columns == 0)
        columns = 1;
    rows = (count + columns - 1) / columns;

    for (size_t row = 0; row < rows; row++) {
        for (size_t col = 0; col < columns; col++) {
            size_t index = (layout == LAYOUT_ACROSS) ? row * columns + col
                                                     : col * rows + row;
            size_t next = (layout == LAYOUT_ACROSS) ? index + 1 : index + rows;
            const struct entry *ent;

            if (index >= count)
                continue;
            ent = &entries[index];
            (void)fputs(ent->label, stdout);
            if (next < count && col + 1 < columns) {
                for (size_t pad = ent->width; pad < widest + COLUMN_GAP;
                     pad++)
                    (void)putchar(' ');
            }
        }
        (void)putchar('\n');
    }
}

static void
print_commas(const struct entry *entries, size_t count)
{
    size_t column = 0;

    for (size_t i = 0; i < count; i++) {
        size_t need = entries[i].width + ((i + 1 < count) ? 2 : 0);

        if (column > 0 && column + need >= (size_t)term_width) {
            (void)putchar('\n');
            column = 0;
        }
        (void)fputs(entries[i].label, stdout);
        if (i + 1 < count) {
            (void)fputs(", ", stdout);
        }
        column += need;
    }
    if (count > 0)
        (void)putchar('\n');
}

/* Print a sorted list, with a block total when it is a directory's. */
static void
print_entries(struct entry *entries, size_t count, int with_total)
{
    build_labels(entries, count);
    if (with_total && (layout == LAYOUT_LONG || show_blocks)) {
        uintmax_t total = 0;

        for (size_t i = 0; i < count; i++)
            total += blocks_of(&entries[i].st);
        (void)printf("total %ju\n", total);
    }
    switch (layout) {
    case LAYOUT_LONG:
        print_long(entries, count);
        break;
    case LAYOUT_COLUMNS:
    case LAYOUT_ACROSS:
        print_columns(entries, count);
        break;
    case LAYOUT_COMMAS:
        print_commas(entries, count);
        break;
    case LAYOUT_ONE:
        for (size_t i = 0; i < count; i++)
            (void)puts(entries[i].label);
        break;
    }
}

static void
free_entries(struct entry *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].path);
        free(entries[i].label);
    }
    free(entries);
}

/* Only -L can revisit a directory, so only -L pays for this scan. */
static int
in_cycle(const struct stat *st)
{
    if (!follow_all)
        return 0;
    for (size_t i = 0; i < nlevels; i++) {
        if (levels[i].dev == st->st_dev && levels[i].ino == st->st_ino)
            return 1;
    }
    return 0;
}

/* Read, print and push one directory, leaving the process inside it. */
static void
open_level(const char *path, const char *title, int show_title)
{
    DIR *dir;
    const struct dirent *dent;
    struct entry *entries = NULL;
    size_t count = 0, cap = 0;
    struct level *level;
    struct xn_dirstep step;
    struct stat here;
    int entered, enter_err = 0;

    if (stat(path, &here) == -1) {
        xn_warn("%s", title);
        status = EXIT_FAILURE;
        return;
    }
    if (in_cycle(&here)) {
        xn_warnx("%s: directory cycle", title);
        status = EXIT_FAILURE;
        return;
    }

    if ((dir = opendir(path)) == NULL) {
        xn_warn("%s", title);
        status = EXIT_FAILURE;
        return;
    }
    if (show_title)
        (void)printf("%s:\n", title);

    /* Inside the directory, names stay short however deep the tree is. */
    entered = (xn_dir_enter(path, &here, &step) == 0);
    if (!entered)
        enter_err = errno;

    for (;;) {
        const char *name;
        struct entry *ent;

        errno = 0;
        if ((dent = readdir(dir)) == NULL)
            break;
        name = dent->d_name;
        if (name[0] == '.') {
            int is_dots = name[1] == '\0' ||
                          (name[1] == '.' && name[2] == '\0');

            if (hidden == HIDE_DOT || (hidden == HIDE_DOTDOT && is_dots))
                continue;
        }
        if (count == cap) {
            cap = (cap != 0) ? cap * 2 : 64;
            entries = xn_realloc(entries, cap * sizeof(*entries));
        }
        ent = &entries[count++];
        ent->name = xn_strdup(name);
        ent->path = xn_pathjoin(title, name);
        ent->label = NULL;
        ent->stat_ok = entered && stat_child(ent->name, &ent->st) == 0;
        if (!ent->stat_ok) {
            if (!entered)
                errno = enter_err;
            xn_warn("%s", ent->path);
            status = EXIT_FAILURE;
            memset(&ent->st, 0, sizeof(ent->st));
        }
    }
    if (errno != 0) {
        xn_warn("%s", title);
        status = EXIT_FAILURE;
    }
    (void)closedir(dir);

    sort_entries(entries, count);
    print_entries(entries, count, 1);

    if (nlevels == levels_cap) {
        levels_cap = (levels_cap != 0) ? levels_cap * 2 : 32;
        levels = xn_realloc(levels, levels_cap * sizeof(*levels));
    }
    level = &levels[nlevels++];
    level->entries = entries;
    level->count = count;
    level->next = 0;
    level->step = step;
    level->dev = here.st_dev;
    level->ino = here.st_ino;
    level->entered = entered;
}

static void
close_level(void)
{
    struct level *level = &levels[--nlevels];

    free_entries(level->entries, level->count);
    if (level->entered)
        xn_dir_leave(&level->step);
}

/* List a directory and, with -R, every directory below it. */
static void
list_tree(const char *path, const char *title, int show_title)
{
    size_t base = nlevels;

    open_level(path, title, show_title);
    while (recursive && nlevels > base) {
        struct level *level = &levels[nlevels - 1];
        const struct entry *ent = NULL;

        while (level->next < level->count) {
            const struct entry *item = &level->entries[level->next++];

            if (item->stat_ok && S_ISDIR(item->st.st_mode) &&
                strcmp(item->name, ".") != 0 &&
                strcmp(item->name, "..") != 0) {
                ent = item;
                break;
            }
        }
        if (ent == NULL) {
            close_level();
            continue;
        }
        (void)putchar('\n');
        open_level(ent->name, ent->path, 1);
    }
    while (nlevels > base)
        close_level();
}

static void
usage(void)
{
    (void)fprintf(stderr, "usage: %s [-ACFRacdfgiklmnopqrstux1] [-H|-L] "
                  "[file...]\n", xn_progname());
    exit(EXIT_FAILURE);
}

/* List files first, then the contents of each directory operand. */
int
main(int argc, char **argv)
{
    static char dot[] = ".";
    static char *default_operand[] = { dot, NULL };
    struct entry *files, *dirs;
    size_t nfiles = 0, ndirs = 0;
    const char *columns = getenv("COLUMNS");
    int is_tty = isatty(STDOUT_FILENO);
    int ch;

    xn_init(argv);
    layout = is_tty ? LAYOUT_COLUMNS : LAYOUT_ONE;
    quote_nonprint = is_tty;

    while ((ch = xn_getopt(argc, argv, "ACFHLRacdfgiklmnopqrstux1")) != -1) {
        switch (ch) {
        case 'A':
            hidden = HIDE_DOTDOT;
            break;
        case 'C':
            layout = LAYOUT_COLUMNS;
            break;
        case 'F':
            classify = 1;
            break;
        case 'H':
            follow_operands = 1;
            follow_all = 0;
            break;
        case 'L':
            follow_all = 1;
            follow_operands = 0;
            break;
        case 'R':
            recursive = 1;
            break;
        case 'a':
            hidden = HIDE_NONE;
            break;
        case 'c':
            time_field = TIME_CHANGED;
            break;
        case 'd':
            dirs_as_files = 1;
            break;
        case 'f':
            sort_key = SORT_NONE;
            hidden = HIDE_NONE;
            reverse = 0;
            show_blocks = 0;
            if (layout == LAYOUT_LONG)
                layout = LAYOUT_ONE;
            break;
        case 'g':
            layout = LAYOUT_LONG;
            hide_owner = 1;
            break;
        case 'i':
            show_inode = 1;
            break;
        case 'k':
            kilobytes = 1;
            break;
        case 'l':
            layout = LAYOUT_LONG;
            break;
        case 'm':
            layout = LAYOUT_COMMAS;
            break;
        case 'n':
            layout = LAYOUT_LONG;
            numeric_ids = 1;
            break;
        case 'o':
            layout = LAYOUT_LONG;
            hide_group = 1;
            break;
        case 'p':
            slash_dirs = 1;
            break;
        case 'q':
            quote_nonprint = 1;
            break;
        case 'r':
            reverse = 1;
            break;
        case 's':
            show_blocks = 1;
            break;
        case 't':
            sort_key = SORT_TIME;
            break;
        case 'u':
            time_field = TIME_ACCESSED;
            break;
        case 'x':
            layout = LAYOUT_ACROSS;
            break;
        case '1':
            layout = LAYOUT_ONE;
            break;
        default:
            usage();
        }
    }
    argc -= xn_optind;
    argv += xn_optind;

    /* -f overrides these whichever order the options came in. */
    if (sort_key == SORT_NONE) {
        hidden = HIDE_NONE;
        reverse = 0;
        show_blocks = 0;
        if (layout == LAYOUT_LONG)
            layout = LAYOUT_ONE;
    }

    if (columns != NULL && atoi(columns) > 0)
        term_width = atoi(columns);
    else if (is_tty && xn_term_width(STDOUT_FILENO) > 0)
        term_width = xn_term_width(STDOUT_FILENO);
    now = time(NULL);

    if (argc == 0) {
        argc = 1;
        argv = default_operand;
    }

    files = xn_malloc((size_t)argc * sizeof(*files));
    dirs = xn_malloc((size_t)argc * sizeof(*dirs));
    for (int i = 0; i < argc; i++) {
        struct entry ent;

        if (stat_operand(argv[i], &ent.st) == -1) {
            xn_warn("%s", argv[i]);
            status = EXIT_FAILURE;
            continue;
        }
        ent.name = xn_strdup(argv[i]);
        ent.path = xn_strdup(argv[i]);
        ent.label = NULL;
        ent.stat_ok = 1;
        if (S_ISDIR(ent.st.st_mode) && !dirs_as_files)
            dirs[ndirs++] = ent;
        else
            files[nfiles++] = ent;
    }

    sort_entries(files, nfiles);
    sort_entries(dirs, ndirs);
    print_entries(files, nfiles, 0);
    for (size_t i = 0; i < ndirs; i++) {
        if (i > 0 || nfiles > 0)
            (void)putchar('\n');
        list_tree(dirs[i].path, dirs[i].name,
                  argc > 1 || nfiles > 0 || recursive);
    }
    free_entries(files, nfiles);
    free_entries(dirs, ndirs);
    return xn_finish(status);
}
