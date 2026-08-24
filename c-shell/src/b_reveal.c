/* b_reveal.c - `reveal`, an ls with an optional recursive mode.
 *
 * Syntax: reveal (-(a|t)*)* (~ | . | .. | - | name)?
 *   -a  include entries beginning with '.' (like ls -A: never . or ..)
 *   -t  walk subdirectories, printing each directory's contents right after it
 * Entries are always sorted by raw ASCII value and printed one per line.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "builtins.h"
#include "pathutil.h"
#include "shell.h"
#include "utils.h"

static int name_needs_quotes(const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n')
            return 1;
    }
    return 0;
}

/* ls quotes names containing blanks; do the same for the displayed path. */
static void print_entry(const char *display, int is_dir_marker)
{
    if (name_needs_quotes(display))
        printf("'%s'%s\n", display, is_dir_marker ? "/" : "");
    else
        printf("%s%s\n", display, is_dir_marker ? "/" : "");
}

/* `dir` is the directory to read, `prefix` is what to print in front of each
 * name (empty at the top level, "sub/" one level down, and so on). */
static void list_dir(const char *dir, const char *prefix, int show_all,
                     int recurse)
{
    DIR           *d = opendir(dir);
    struct dirent *de;
    StrVec         names;

    if (d == NULL)
        return; /* unreadable subdirectory: skip it rather than abort the walk */

    strvec_init(&names);
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!show_all && de->d_name[0] == '.')
            continue;
        strvec_push(&names, xstrdup(de->d_name));
    }
    closedir(d);

    strvec_sort(&names); /* lexicographic on the bare name, no trailing slash */

    for (size_t i = 0; i < names.len; i++) {
        const char *name = names.items[i];
        char       *display;
        Buf         b;

        buf_init(&b);
        buf_append(&b, prefix, strlen(prefix));
        buf_append(&b, name, strlen(name));
        display = buf_release(&b);

        if (recurse) {
            char       *full = path_join(dir, name);
            struct stat st;
            int         isdir = (lstat(full, &st) == 0 && S_ISDIR(st.st_mode));

            print_entry(display, isdir);
            if (isdir) {
                char *sub = path_join(display, ""); /* display + '/' */
                list_dir(full, sub, show_all, recurse);
                free(sub);
            }
            free(full);
        } else {
            print_entry(display, 0);
        }
        free(display);
    }
    strvec_free(&names);
}

int builtin_reveal(int argc, char **argv)
{
    int         show_all = 0, recurse = 0, seen_path = 0;
    const char *path_arg = ".";
    char       *resolved;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        /* A lone "-" is the previous-directory argument, not a flag. */
        if (a[0] == '-' && a[1] != '\0') {
            if (seen_path) { /* flags must precede the path */
                err_printf("reveal: invalid syntax");
                return 1;
            }
            for (const char *p = a + 1; *p != '\0'; p++) {
                if (*p == 'a')
                    show_all = 1;
                else if (*p == 't')
                    recurse = 1;
                else {
                    err_printf("reveal: invalid syntax");
                    return 1;
                }
            }
        } else {
            if (seen_path) { /* at most one directory argument */
                err_printf("reveal: invalid syntax");
                return 1;
            }
            path_arg  = a;
            seen_path = 1;
        }
    }

    resolved = path_resolve_special(path_arg);
    if (resolved == NULL || !path_is_dir(resolved)) {
        free(resolved);
        err_printf("reveal: no such directory");
        return 1;
    }

    list_dir(resolved, "", show_all, recurse);
    fflush(stdout);
    free(resolved);
    return 0;
}
