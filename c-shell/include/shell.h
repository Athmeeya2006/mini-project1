/* shell.h - global shell state shared by every subsystem. */
#ifndef CSHELL_SHELL_H
#define CSHELL_SHELL_H

#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* The spec guarantees the user never types more than 1024 characters. */
#define CSHELL_MAX_INPUT 1024

/* Every diagnostic the shell prints is prefixed with this. */
#define CSHELL_NAME "cshell"

typedef struct {
    char home[PATH_MAX]; /* directory the shell was launched from */
    char prev[PATH_MAX]; /* previous working directory, for `hop -` */
    int  has_prev;       /* 0 until the first successful directory change */
    int  should_exit;    /* set by the `exit` builtin */
    int  last_status;    /* exit status of the last foreground command */
} ShellState;

extern ShellState g_shell;

void shell_init(void);
void shell_loop(void);

/* Remember `path` as the directory we came from (used by `hop -`). */
void shell_set_prev(const char *path);

#endif /* CSHELL_SHELL_H */
