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

/* Set by the Ctrl-C / Ctrl-Z handlers so the reader knows the prompt has to be
 * drawn again. */
static volatile sig_atomic_t g_interrupted;

/* The shell must survive SIGINT and SIGTSTP rather than ignore them: a handler
 * is what makes the read at the prompt come back so the prompt can be redrawn.
 * Writing the newline here rather than in the loop keeps it in step with the
 * "^C" the terminal has just echoed; write() is safe in a handler, printf is
 * not. */
static void interrupt_handler(int sig)
{
    (void)sig;
    g_interrupted = 1;
    (void)!write(STDOUT_FILENO, "\n", 1);
}

static void install_handler(int sig, void (*fn)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: the read has to come back with EINTR */
    sigaction(sig, &sa, NULL);
}

void shell_init(void)
{
    char *cwd = path_getcwd();

    memset(&g_shell, 0, sizeof g_shell);
    snprintf(g_shell.home, sizeof g_shell.home, "%s", cwd);
    free(cwd);

    /* Ctrl-C and Ctrl-Z reach the foreground job, which is a process group of
     * its own; the shell only ever sees them when it is the foreground group
     * itself, that is, while it waits at the prompt. Neither may kill or stop
     * it. */
    install_handler(SIGINT, interrupt_handler);
    install_handler(SIGTSTP, interrupt_handler);
    signal(SIGQUIT, SIG_IGN);
    /* Handing the terminal back and forth with tcsetpgrp() is done from a
     * process group that is not the terminal's own, which would otherwise stop
     * the shell with SIGTTOU. */
    signal(SIGTTOU, SIG_IGN);

    /* Job control needs the shell to be a process group of its own, holding
     * the terminal, so that every pipeline it launches can be given a group
     * of its own and handed the terminal in turn. */
    g_shell.terminal_fd = STDIN_FILENO;
    g_shell.interactive = isatty(g_shell.terminal_fd);
    setpgid(0, 0);
    g_shell.pgid = getpgrp();
    if (g_shell.interactive)
        tcsetpgrp(g_shell.terminal_fd, g_shell.pgid);

    jobs_init();
    frecency_init();
}

void shell_set_prev(const char *path)
{
    snprintf(g_shell.prev, sizeof g_shell.prev, "%s", path);
    g_shell.has_prev = 1;
}

/* Read one line (at most CSHELL_MAX_INPUT characters) into buf.
 * Returns 0 on a complete line, 1 if something was printed over the prompt and
 * it has to be drawn again, 2 if the read was interrupted with nothing
 * printed, and -1 at end of input.
 *
 * Ctrl-D is only end of input on an empty line. On a terminal it makes the
 * read come back with whatever has been typed so far and no newline, and the
 * shell then keeps that text and carries on reading, so the user sees nothing
 * happen. Input that is not a terminal has no Ctrl-D, so a last line with no
 * closing newline is simply run.
 */
static int read_line(char *buf, size_t cap)
{
    size_t len = 0;

    buf[0] = '\0';
    for (;;) {
        char *r;

        /* A background job that finishes while the shell sits here must report
         * itself straight away, so the handler is allowed to print and the
         * read comes back with EINTR instead of being restarted. */
        jobs_set_at_prompt(1);
        r = fgets(buf + len, (int)(cap - len), stdin);
        jobs_set_at_prompt(0);

        if (r == NULL) {
            if (ferror(stdin) && errno == EINTR) {
                clearerr(stdin);
                /* Only something that was actually printed needs a new prompt;
                 * a job merely being stopped must not leave one behind. */
                if (g_interrupted) {
                    g_interrupted = 0;
                    return 1;
                }
                return jobs_take_notified() ? 1 : 2;
            }
            /* End of input. On a terminal that is not permanent, so the flag
             * has to be cleared or every later read would report it again. */
            clearerr(stdin);
            if (len == 0)
                return -1;
            if (g_shell.interactive)
                continue; /* Ctrl-D with text typed: keep it, read on */
            return 0;
        }

        len += strlen(buf + len);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[len - 1] = '\0';
            return 0;
        }
        if (len + 1 >= cap)
            return 0; /* the line filled the buffer */
    }
}

void shell_loop(void)
{
    /* +2 leaves room for the newline and the terminator. */
    char line[CSHELL_MAX_INPUT + 2];
    int  warned_stopped = 0;

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
            continue; /* something was printed over the prompt: draw it again */
        if (rc < 0) {
            /* Ctrl-D on an empty line. Stopped jobs would be killed by the
             * SIGHUP that leaving sends, so the first Ctrl-D only warns; a
             * second one with nothing typed in between goes through. */
            putchar('\n');
            if (!warned_stopped && jobs_any_stopped()) {
                err_printf("%s: there are stopped jobs", CSHELL_NAME);
                warned_stopped = 1;
                continue;
            }
            break;
        }
        warned_stopped = 0;

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

    /* Nothing the shell started outlives it unnoticed. */
    jobs_hangup();
    frecency_shutdown();
}
