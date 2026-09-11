/* shell.c - shell state and the read/lex/parse/execute loop. */
#include "shell.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "exec.h"
#include "frecency.h"
#include "jobs.h"
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

    jobs_init();
    frecency_init();
}

void shell_set_prev(const char *path)
{
    snprintf(g_shell.prev, sizeof g_shell.prev, "%s", path);
    g_shell.has_prev = 1;
}

/* Read one line (at most CSHELL_MAX_INPUT characters) into buf.
 * Returns 0 on success, 1 if a signal interrupted the read and the prompt has
 * to be drawn again, 2 if it was interrupted with nothing printed, and -1 at
 * end of input. */
static int read_line(char *buf, size_t cap)
{
    char *r;

    /* A background job that finishes while the shell sits here must report
     * itself straight away, so the handler is allowed to print and the read
     * comes back with EINTR instead of being restarted. */
    jobs_set_at_prompt(1);
    r = fgets(buf, (int)cap, stdin);
    jobs_set_at_prompt(0);

    if (r == NULL) {
        if (ferror(stdin) && errno == EINTR) {
            clearerr(stdin);
            /* Only a message that was actually printed needs a new prompt;
             * a job merely being stopped must not leave one behind. */
            return jobs_take_notified() ? 1 : 2;
        }
        return -1;
    }
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

void shell_loop(void)
{
    /* +2 leaves room for the newline and the terminator. */
    char line[CSHELL_MAX_INPUT + 2];

    while (!g_shell.should_exit) {
        Token    *toks = NULL;
        Pipeline *pipes = NULL, *p;
        int       rc;

        /* Retire whatever finished while the last line was running. */
        jobs_sweep();
        prompt_display();

        rc = read_line(line, sizeof line);
        while (rc == 2)
            rc = read_line(line, sizeof line); /* nothing was printed */
        if (rc > 0)
            continue; /* a job reported itself: draw a fresh prompt */
        if (rc < 0) {
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

        /* The groups of a ';' / '&' sequence run left to right. A group that
         * could not be started at all ends the sequence; one that merely
         * returned a non-zero status does not. */
        for (p = pipes; p != NULL; p = p->next) {
            int launch_failed = 0;

            if (p->background) {
                exec_pipeline(p, &launch_failed);
            } else {
                /* Anything launched earlier on this line is still held back
                 * waiting to be announced, and must not be left blocked while
                 * a foreground command runs. */
                exec_release_background();
                g_shell.last_status = exec_pipeline(p, &launch_failed);
            }
            if (launch_failed || g_shell.should_exit)
                break;
        }
        exec_release_background();

        pipeline_list_free(pipes);
    }

    frecency_shutdown();
}
