/* shell.c - shell state and the read/lex/parse/execute loop. */
#include "shell.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "exec.h"
#include "frecency.h"
#include "lexer.h"
#include "parser.h"
#include "pathutil.h"
#include "prompt.h"
#include "utils.h"

ShellState g_shell;

void shell_init(void)
{
    char *cwd = path_getcwd();

    memset(&g_shell, 0, sizeof g_shell);
    snprintf(g_shell.home, sizeof g_shell.home, "%s", cwd);
    free(cwd);

    /* Ctrl-C must kill the foreground job, not the shell itself. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    frecency_init();
}

void shell_set_prev(const char *path)
{
    snprintf(g_shell.prev, sizeof g_shell.prev, "%s", path);
    g_shell.has_prev = 1;
}

/* Read one line (at most CSHELL_MAX_INPUT characters) into buf.
 * Returns 0 on success, -1 at end of input. */
static int read_line(char *buf, size_t cap)
{
    if (fgets(buf, (int)cap, stdin) == NULL)
        return -1;
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

void shell_loop(void)
{
    /* +2 leaves room for the newline and the terminator. */
    char line[CSHELL_MAX_INPUT + 2];

    while (!g_shell.should_exit) {
        Token    *toks = NULL;
        Pipeline *pipes = NULL;

        prompt_display();

        if (read_line(line, sizeof line) != 0) {
            putchar('\n');
            break; /* end of input: Ctrl-D exits the shell */
        }

        if (lex_line(line, &toks) != 0) {
            err_printf("%s: invalid syntax", CSHELL_NAME);
            continue;
        }
        if (parse_tokens(toks, &pipes) != 0) {
            err_printf("%s: invalid syntax", CSHELL_NAME);
            token_list_free(toks);
            continue;
        }
        token_list_free(toks);

        /* Part C: only the first command group runs; ';' and '&' chains are
         * parsed and validated, but the remaining groups are ignored. */
        if (pipes != NULL)
            g_shell.last_status = exec_pipeline(pipes);

        pipeline_list_free(pipes);
    }

    frecency_shutdown();
}
