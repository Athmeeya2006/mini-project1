/* exec.c - command resolution, forking, pipes and waiting.
 *
 * Part C only runs the first command group of a line, so ';' and '&' chains are
 * parsed and validated but the remaining groups are ignored.
 */
#include "exec.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "builtins.h"
#include "pathutil.h"
#include "redirect.h"
#include "shell.h"
#include "utils.h"

/* First executable called `name` in PATH, or NULL. */
static char *search_path(const char *name)
{
    const char *path_env = getenv("PATH");
    char       *copy, *p, *result = NULL;

    if (path_env == NULL || path_env[0] == '\0')
        return NULL;

    copy = xstrdup(path_env);
    p    = copy;
    while (p != NULL && result == NULL) {
        char *colon = strchr(p, ':');
        char *dir   = p;
        char *cand;

        if (colon != NULL) {
            *colon = '\0';
            p = colon + 1;
        } else {
            p = NULL;
        }
        if (dir[0] == '\0')
            dir = ".";

        cand = path_join(dir, name);
        if (path_is_executable(cand))
            result = cand;
        else
            free(cand);
    }
    free(copy);
    return result;
}

char *exec_resolve(const char *name)
{
    char *cwd, *cand, *found;

    /* '%' forces a PATH lookup, skipping the current directory. */
    if (name[0] == '%')
        return search_path(name + 1);

    /* A name containing '/' is a literal path. */
    if (strchr(name, '/') != NULL) {
        if (path_is_executable(name))
            return xstrdup(name);
        return NULL;
    }

    /* A bare name is looked up in the current directory first. */
    cwd  = path_getcwd();
    cand = path_join(cwd, name);
    free(cwd);
    if (path_is_executable(cand))
        return cand;
    free(cand);

    found = search_path(name);
    return found;
}

/* The name to show in diagnostics: '%prog' reports as 'prog'. */
static const char *display_name(const char *name)
{
    return name[0] == '%' ? name + 1 : name;
}

/* Runs in the child; never returns. */
static void child_exec(Command *cmd)
{
    char *path;

    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);

    /* Intrinsics behave like any other command inside a pipeline. */
    if (cmd->argv[0][0] != '%' && builtin_is(cmd->argv[0]))
        _exit(builtin_run(cmd));

    path = exec_resolve(cmd->argv[0]);
    if (path == NULL) {
        err_printf("%s: command not found (%s)", CSHELL_NAME,
                   display_name(cmd->argv[0]));
        _exit(127);
    }

    /* argv[0] is handed over without the '%' marker. */
    cmd->argv[0] = (char *)display_name(cmd->argv[0]);
    execv(path, cmd->argv);

    err_printf("%s: command not found (%s)", CSHELL_NAME, cmd->argv[0]);
    _exit(127);
}

/* A single builtin with no pipe runs inside the shell, so `hop` and friends
 * can change the shell's own state. Its redirections are installed on the
 * shell's fds and removed again afterwards. */
static int run_builtin_in_shell(Command *cmd)
{
    RedirPlan plan;
    int       saved_in = -1, saved_out = -1;
    int       status;

    if (redirect_plan(cmd, &plan) != 0)
        return 1;

    fflush(stdout);
    if (plan.in_fd >= 0) {
        saved_in = dup(STDIN_FILENO);
        dup2(plan.in_fd, STDIN_FILENO);
    }
    if (plan.out_fd >= 0) {
        saved_out = dup(STDOUT_FILENO);
        dup2(plan.out_fd, STDOUT_FILENO);
    }

    status = builtin_run(cmd);
    fflush(stdout);

    if (saved_in >= 0) {
        dup2(saved_in, STDIN_FILENO);
        close(saved_in);
    }
    if (saved_out >= 0) {
        dup2(saved_out, STDOUT_FILENO);
        close(saved_out);
    }

    redirect_finish(&plan);
    redirect_release(&plan);
    return status;
}

int exec_pipeline(Pipeline *p)
{
    Command   *c;
    RedirPlan *plans;
    pid_t     *pids;
    int      (*pipes)[2] = NULL;
    int        n = 0, i, status = 0;

    for (c = p->cmds; c != NULL; c = c->next)
        n++;
    if (n == 0)
        return 0;

    if (n == 1 && p->cmds->argv[0][0] != '%' && builtin_is(p->cmds->argv[0]))
        return run_builtin_in_shell(p->cmds);

    plans = xmalloc((size_t)n * sizeof *plans);
    pids  = xmalloc((size_t)n * sizeof *pids);

    /* Open every redirection before anything is executed. */
    for (i = 0, c = p->cmds; c != NULL; c = c->next, i++) {
        if (redirect_plan(c, &plans[i]) != 0) {
            for (int k = 0; k < i; k++)
                redirect_release(&plans[k]);
            free(plans);
            free(pids);
            return 1;
        }
    }

    if (n > 1) {
        pipes = xmalloc((size_t)(n - 1) * sizeof *pipes);
        for (i = 0; i < n - 1; i++) {
            if (pipe(pipes[i]) != 0) {
                err_printf("%s: unable to create pipe", CSHELL_NAME);
                for (int k = 0; k < i; k++) {
                    close(pipes[k][0]);
                    close(pipes[k][1]);
                }
                for (int k = 0; k < n; k++)
                    redirect_release(&plans[k]);
                free(pipes);
                free(plans);
                free(pids);
                return 1;
            }
        }
    }

    for (i = 0, c = p->cmds; c != NULL; c = c->next, i++) {
        pid_t pid = fork();

        if (pid < 0) {
            err_printf("%s: unable to fork", CSHELL_NAME);
            pids[i] = -1;
            continue;
        }
        if (pid == 0) {
            /* Wire this stage into the pipeline... */
            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < n - 1)
                dup2(pipes[i][1], STDOUT_FILENO);
            for (int k = 0; k < n - 1; k++) {
                close(pipes[k][0]);
                close(pipes[k][1]);
            }
            /* ...then let explicit redirections override the pipe. */
            redirect_apply(&plans[i]);
            for (int k = 0; k < n; k++)
                if (k != i)
                    redirect_close(&plans[k]);

            child_exec(c);
        }
        pids[i] = pid;
    }

    /* The shell must hold no pipe fd, or the readers would never see EOF. */
    for (i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    for (i = 0; i < n; i++) {
        int wstatus;

        if (pids[i] < 0) {
            status = 1;
            continue;
        }
        while (waitpid(pids[i], &wstatus, 0) < 0 && errno == EINTR)
            ;
        if (i == n - 1)
            status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
    }

    for (i = 0; i < n; i++) {
        redirect_finish(&plans[i]);
        redirect_release(&plans[i]);
    }

    free(pipes);
    free(plans);
    free(pids);
    return status;
}
