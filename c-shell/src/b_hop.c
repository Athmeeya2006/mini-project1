/* b_hop.c - `hop`, a cd that falls back to a frecency lookup.
 *
 * Syntax: hop ((~ | . | .. | - | name)*)?
 * Every argument is applied in order, so `hop a .. -` performs three moves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "builtins.h"
#include "frecency.h"
#include "pathutil.h"
#include "shell.h"
#include "utils.h"

/* chdir to `target`, remembering where we came from and scoring the arrival. */
static int hop_to(const char *target)
{
    char *old, *now;

    old = path_getcwd();
    if (chdir(target) != 0) {
        free(old);
        return -1;
    }
    shell_set_prev(old);
    free(old);

    now = path_getcwd();
    frecency_record(now);
    free(now);
    return 0;
}

static int hop_one(const char *arg)
{
    char *target;
    int   rc;

    /* "." is explicitly a no-op, so it must not touch the frecency store. */
    if (strcmp(arg, ".") == 0)
        return 0;

    if (strcmp(arg, "..") == 0) {
        char *cwd = path_getcwd();
        int   at_root = (strcmp(cwd, "/") == 0);
        free(cwd);
        if (at_root)
            return 0;               /* no parent: do nothing */
        return hop_to("..");
    }

    if (strcmp(arg, "-") == 0) {
        if (!g_shell.has_prev)
            return 0;               /* nowhere to go back to: do nothing */
        target = xstrdup(g_shell.prev);
        rc = hop_to(target);
        free(target);
        return rc;
    }

    /* "~", "~/x" and ordinary paths resolve directly. */
    target = path_resolve_special(arg);
    if (target == NULL)
        return -1;
    rc = hop_to(target);
    free(target);
    if (rc == 0)
        return 0;

    /* The name did not resolve on disk: fall back to the frecency store and
     * jump to the best ranked directory whose path contains it. */
    target = frecency_lookup(arg);
    if (target != NULL) {
        rc = hop_to(target);
        free(target);
        if (rc == 0)
            return 0;
    }

    err_printf("hop: no such directory");
    return -1;
}

int builtin_hop(int argc, char **argv)
{
    int status = 0;

    if (argc == 1)
        return hop_one("~") == 0 ? 0 : 1;

    for (int i = 1; i < argc; i++) {
        if (hop_one(argv[i]) != 0)
            status = 1;
    }
    return status;
}
