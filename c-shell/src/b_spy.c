/* b_spy.c - `spy [pid]`, the open files of a process.
 *
 * Everything comes out of /proc: the working directory and the executable are
 * symbolic links, the mapped files are listed in `maps`, and the descriptors
 * the process holds are one symbolic link each in `fd`. What each of them
 * points at is read with readlink, and what kind of object it is comes from
 * stat, which follows the link to the object itself.
 */
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "builtins.h"
#include "shell.h"
#include "utils.h"

#define SPY_LINE_MAX 8192
/* Long enough for "/proc/<pid>/maps" and friends. */
#define SPY_PROC_MAX 64

/* The object a path leads to, named the way lsof names it. */
static const char *type_of(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return "unknown";
    switch (st.st_mode & S_IFMT) {
    case S_IFREG:  return "REG";
    case S_IFDIR:  return "DIR";
    case S_IFCHR:  return "CHR";
    case S_IFBLK:  return "BLK";
    case S_IFIFO:  return "FIFO";
    case S_IFLNK:  return "LNK";
    case S_IFSOCK: return "SOCK";
    default:       return "unknown";
    }
}

/* A descriptor can point at something with no path of its own, which stat
 * cannot reach from outside the process. What it is is then read off the link
 * itself, the way lsof names these. */
static const char *type_of_fd(const char *link, const char *target)
{
    const char *t = type_of(link);

    if (strcmp(t, "unknown") != 0)
        return t;
    if (strncmp(target, "socket:", 7) == 0)
        return "SOCK";
    if (strncmp(target, "pipe:", 5) == 0)
        return "FIFO";
    if (strncmp(target, "anon_inode:", 11) == 0)
        return "a_inode";
    return t;
}

static void print_row(pid_t pid, const char *fd, const char *type,
                      const char *path)
{
    printf("%-6d %-5s %-6s %s\n", (int)pid, fd, type, path);
}

/* Read a /proc symbolic link. Returns 0 and fills buf, or -1. */
static int read_link(const char *path, char *buf, size_t cap)
{
    ssize_t n = readlink(path, buf, cap - 1);

    if (n < 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

/* One of the links that stand on their own: cwd and exe. The target is copied
 * out when the caller has a use for it, which the executable has: it is also
 * one of the mapped files and must not be listed twice. */
static int show_link(pid_t pid, const char *proc_dir, const char *name,
                     const char *label, char *out, size_t cap)
{
    char link[SPY_PROC_MAX + 8], target[PATH_MAX];

    snprintf(link, sizeof link, "%s/%s", proc_dir, name);
    if (read_link(link, target, sizeof target) != 0)
        return -1;
    print_row(pid, label, type_of(link), target);
    if (out != NULL)
        snprintf(out, cap, "%s", target);
    return 0;
}

/* The pathname of a mapping is the sixth field of the line, and anything
 * before it cannot contain a space. A mapping with no name at all, or one
 * naming a kernel area such as [stack], is not a file and is left out. */
static const char *maps_pathname(char *line)
{
    char  *p = line;
    size_t len = strlen(line);

    if (len > 0 && line[len - 1] == '\n')
        line[len - 1] = '\0';

    for (int field = 0; field < 5; field++) {
        while (*p != '\0' && *p != ' ' && *p != '\t')
            p++;
        while (*p == ' ' || *p == '\t')
            p++;
    }
    if (*p != '/')
        return NULL;

    /* A file that has been replaced or removed since it was mapped is marked
     * as such in maps. The mark is not part of the path, so it is cut off. */
    char *mark = strstr(p, " (deleted)");
    if (mark != NULL && mark[sizeof " (deleted)" - 1] == '\0')
        *mark = '\0';
    return p;
}

/* Every file the process has mapped, each one only once, and never the
 * executable itself since that has already been listed as txt. */
static void show_maps(pid_t pid, const char *proc_dir, const char *exe)
{
    char    path[SPY_PROC_MAX + 8];
    char   *line;
    FILE   *f;
    StrVec  seen;

    snprintf(path, sizeof path, "%s/maps", proc_dir);
    f = fopen(path, "r");
    if (f == NULL)
        return;

    strvec_init(&seen);
    line = xmalloc(SPY_LINE_MAX);
    while (fgets(line, SPY_LINE_MAX, f) != NULL) {
        const char *name = maps_pathname(line);
        const char *type;
        int         dup  = 0;

        if (name == NULL)
            continue;
        if (exe[0] != '\0' && strcmp(name, exe) == 0)
            continue;
        for (size_t i = 0; i < seen.len && !dup; i++)
            if (strcmp(seen.items[i], name) == 0)
                dup = 1;
        if (dup)
            continue;
        strvec_push(&seen, xstrdup(name));
        /* Only a file can be mapped, so one that can no longer be stat'ed -
         * because it has been deleted or replaced - is still a regular
         * file. */
        type = type_of(name);
        print_row(pid, "mem", strcmp(type, "unknown") == 0 ? "REG" : type,
                  name);
    }
    free(line);
    strvec_free(&seen);
    fclose(f);
}

/* Sort the descriptor names by number, not as text, so 2 comes before 10. */
static int fd_cmp(const void *a, const void *b)
{
    long x = atol(*(char *const *)a);
    long y = atol(*(char *const *)b);

    return x < y ? -1 : (x > y ? 1 : 0);
}

static void show_fds(pid_t pid, const char *proc_dir)
{
    char           dirpath[SPY_PROC_MAX + 8];
    DIR           *d;
    struct dirent *ent;
    StrVec         names;

    snprintf(dirpath, sizeof dirpath, "%s/fd", proc_dir);
    d = opendir(dirpath);
    if (d == NULL)
        return;

    strvec_init(&names);
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        strvec_push(&names, xstrdup(ent->d_name));
    }
    closedir(d);

    if (names.len > 1)
        qsort(names.items, names.len, sizeof *names.items, fd_cmp);

    for (size_t i = 0; i < names.len; i++) {
        char link[PATH_MAX], target[PATH_MAX];

        snprintf(link, sizeof link, "%s/%s", dirpath, names.items[i]);
        if (read_link(link, target, sizeof target) != 0)
            continue;
        print_row(pid, names.items[i], type_of_fd(link, target), target);
    }
    strvec_free(&names);
}

int builtin_spy(int argc, char **argv)
{
    char        proc_dir[SPY_PROC_MAX];
    char        exe[PATH_MAX] = "";
    struct stat st;
    pid_t       pid;

    if (argc > 2) {
        err_printf("%s: invalid syntax", "spy");
        return 1;
    }
    if (argc == 2) {
        long given = parse_nonneg(argv[1]);

        if (given <= 0) {
            err_printf("%s: no such process", "spy");
            return 1;
        }
        pid = (pid_t)given;
    } else {
        /* No argument: the shell itself, which is what is running this. */
        pid = getpid();
    }

    snprintf(proc_dir, sizeof proc_dir, "/proc/%d", (int)pid);
    if (stat(proc_dir, &st) != 0) {
        err_printf("%s: no such process", "spy");
        return 1;
    }

    printf("%-6s %-5s %-6s %s\n", "PID", "FD", "TYPE", "PATH");
    show_link(pid, proc_dir, "cwd", "cwd", NULL, 0);
    if (show_link(pid, proc_dir, "exe", "txt", exe, sizeof exe) != 0)
        exe[0] = '\0';
    show_maps(pid, proc_dir, exe);
    show_fds(pid, proc_dir);
    fflush(stdout);
    return 0;
}
