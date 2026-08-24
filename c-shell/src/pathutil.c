#include "pathutil.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shell.h"
#include "utils.h"

char *path_getcwd(void)
{
    char buf[PATH_MAX];

    if (getcwd(buf, sizeof buf) == NULL)
        return xstrdup(".");
    return xstrdup(buf);
}

char *path_join(const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    Buf    b;

    buf_init(&b);
    buf_append(&b, dir, dl);
    if (dl > 0 && dir[dl - 1] != '/')
        buf_push(&b, '/');
    buf_append(&b, name, strlen(name));
    return buf_release(&b);
}

char *path_absolute(const char *path)
{
    char *cwd, *abs;

    if (path[0] == '/')
        return xstrdup(path);
    cwd = path_getcwd();
    abs = path_join(cwd, path);
    free(cwd);
    return abs;
}

char *path_resolve_special(const char *arg)
{
    if (strcmp(arg, "~") == 0)
        return xstrdup(g_shell.home);

    if (strncmp(arg, "~/", 2) == 0)
        return path_join(g_shell.home, arg + 2);

    if (strcmp(arg, "-") == 0) {
        if (!g_shell.has_prev)
            return NULL;
        return xstrdup(g_shell.prev);
    }

    /* ".", ".." and every ordinary path are handed to the kernel as-is. */
    return xstrdup(arg);
}

int path_is_dir(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode);
}

int path_is_executable(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return 0;
    if (S_ISDIR(st.st_mode))
        return 0;
    /* access() uses the real uid, i.e. "can the current user run this?". */
    return access(path, X_OK) == 0;
}
