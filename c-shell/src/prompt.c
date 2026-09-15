/* prompt.c - <username@hostname:path> */
#include "prompt.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "pathutil.h"
#include "shell.h"
#include "utils.h"

static const char *current_user(void)
{
    struct passwd *pw = getpwuid(getuid());

    if (pw != NULL && pw->pw_name != NULL)
        return pw->pw_name;
    return "unknown";
}

static void current_host(char *buf, size_t n)
{
    char *dot;

    if (gethostname(buf, n) != 0) {
        snprintf(buf, n, "unknown");
        return;
    }
    buf[n - 1] = '\0';
    /* Show the short host name, the way bash's \h does. */
    dot = strchr(buf, '.');
    if (dot != NULL)
        *dot = '\0';
}

/* Replace the shell's home prefix with '~'; otherwise show the path as is. */
static char *display_path(void)
{
    char  *cwd  = path_getcwd();
    size_t hlen = strlen(g_shell.home);
    char  *disp;

    if (strcmp(cwd, g_shell.home) == 0) {
        disp = xstrdup("~");
    } else if (strncmp(cwd, g_shell.home, hlen) == 0 &&
               (cwd[hlen] == '/' || strcmp(g_shell.home, "/") == 0)) {
        /* When home is "/" itself its prefix has no separator after it, so
         * the whole path is kept to give "~/usr" rather than "~usr". */
        const char *rest = strcmp(g_shell.home, "/") == 0 ? cwd : cwd + hlen;
        Buf         b;

        buf_init(&b);
        buf_push(&b, '~');
        buf_append(&b, rest, strlen(rest));
        disp = buf_release(&b);
    } else {
        disp = xstrdup(cwd);
    }
    free(cwd);
    return disp;
}

void prompt_display(void)
{
    char  host[256];
    char *path = display_path();

    current_host(host, sizeof host);
    printf("<%s@%s:%s> ", current_user(), host, path);
    fflush(stdout);
    free(path);
}
