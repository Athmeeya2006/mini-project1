/* b_locate.c - `locate`, a `type -a` style lookup.
 *
 * Syntax: locate filename+
 * The current directory is searched first, then every PATH entry in order, and
 * every match is printed (a name found in three places prints three lines).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "pathutil.h"
#include "utils.h"

static int locate_one(const char *name)
{
    const char *path_env;
    char       *cwd, *cand;
    int         found = 0;

    /* 1. the current working directory */
    cwd  = path_getcwd();
    cand = path_join(cwd, name);
    if (path_is_executable(cand)) {
        printf("%s\n", cand);
        found = 1;
    }
    free(cand);

    /* 2. every directory in PATH, in order */
    path_env = getenv("PATH");
    if (path_env != NULL && path_env[0] != '\0') {
        char *copy = xstrdup(path_env);
        char *p    = copy;

        while (p != NULL) {
            char *colon = strchr(p, ':');
            char *dir   = p;
            char *full;

            if (colon != NULL) {
                *colon = '\0';
                p = colon + 1;
            } else {
                p = NULL;
            }
            /* An empty PATH entry means the current directory. */
            if (dir[0] == '\0')
                dir = ".";

            full = path_join(dir, name);
            if (path_is_executable(full)) {
                /* The absolute path must be printed, but symlinks are not
                 * resolved: we show the path as it was found. */
                char *abs = path_absolute(full);
                printf("%s\n", abs);
                free(abs);
                found = 1;
            }
            free(full);
        }
        free(copy);
    }

    free(cwd);

    if (!found) {
        err_printf("locate: command not found (%s)", name);
        return 1;
    }
    return 0;
}

int builtin_locate(int argc, char **argv)
{
    int status = 0;

    if (argc < 2) {
        err_printf("locate: invalid syntax");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (locate_one(argv[i]) != 0)
            status = 1;
    }
    fflush(stdout);
    return status;
}
