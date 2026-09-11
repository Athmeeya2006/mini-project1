/* exec.c - command resolution, forking, pipes, job control and waiting.
 *
 * A line is a sequence of command groups separated by ';' or '&'. Each group
 * is one pipeline and is launched here: a foreground group is waited for, a
 * background group is registered in the job table and left to run.
 *
 * Names are resolved in the parent, before anything is forked, because the
 * shell has to know whether it could start the command at all: that is the
 * only kind of failure that stops the rest of the sequence.
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
#include "jobs.h"
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

/* '%' always means "a program on PATH", so it never names an intrinsic. */
static int is_builtin_cmd(const Command *c)
{
    return c->argv[0][0] != '%' && builtin_is(c->argv[0]);
}

/* --- the release barrier -------------------------------------------------- */

/* A background child blocks on the read end of a pipe until the shell closes
 * the write end, which it does only once the "[n] pid" lines of the whole
 * input line have been printed. Without this the child could reach exec and
 * produce output before the shell got round to announcing it. */
#define PENDING_MAX 64
static int g_pending[PENDING_MAX];
static int g_npending;

static void pending_push(int fd)
{
    if (fd < 0)
        return;
    if (g_npending == PENDING_MAX) {
        close(fd);
        return;
    }
    g_pending[g_npending++] = fd;
}

void exec_release_background(void)
{
    for (int i = 0; i < g_npending; i++)
        close(g_pending[i]);
    g_npending = 0;
}

/* A child must not hold any barrier open: not the one it is waiting on, and
 * not one belonging to a job launched earlier on the same line. */
static void pending_close_in_child(void)
{
    for (int i = 0; i < g_npending; i++)
        close(g_pending[i]);
    g_npending = 0;
}

static void wait_for_release(int read_fd)
{
    char c;
    ssize_t r;

    if (read_fd < 0)
        return;
    while ((r = read(read_fd, &c, 1)) < 0 && errno == EINTR)
        ;
    close(read_fd);
}

/* --- launching ------------------------------------------------------------ */

/* Runs in the child; never returns. `path` is NULL for an intrinsic. */
static void child_exec(Command *cmd, char *path)
{
    sigset_t empty;

    /* The shell forks with SIGCHLD blocked, and the mask survives exec. */
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);

    /* Intrinsics behave like any other command inside a pipeline. */
    if (path == NULL)
        _exit(builtin_run(cmd));

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

static void text_append(char *buf, size_t cap, size_t *len, const char *s)
{
    while (*s != '\0' && *len + 1 < cap)
        buf[(*len)++] = *s++;
    buf[*len] = '\0';
}

/* The command line as the job table wants to show it again later. */
static void pipeline_text(Pipeline *p, char *buf, size_t cap)
{
    size_t len = 0;

    buf[0] = '\0';
    for (Command *c = p->cmds; c != NULL; c = c->next) {
        if (c != p->cmds)
            text_append(buf, cap, &len, " | ");
        for (int i = 0; i < c->argc; i++) {
            if (i > 0)
                text_append(buf, cap, &len, " ");
            text_append(buf, cap, &len, c->argv[i]);
        }
    }
}

int exec_pipeline(Pipeline *p, int *launch_failed)
{
    Command   *c;
    RedirPlan *plans;
    pid_t     *pids;
    char     **paths;
    int      (*pipes)[2] = NULL;
    int        sync_fd[2] = { -1, -1 };
    int        n = 0, i, status = 0;
    pid_t      pgid = 0;
    sigset_t   saved;
    Job       *job;
    char       text[JOB_CMD_MAX];

    if (launch_failed != NULL)
        *launch_failed = 0;

    for (c = p->cmds; c != NULL; c = c->next)
        n++;
    if (n == 0)
        return 0;

    if (n == 1 && !p->background && is_builtin_cmd(p->cmds))
        return run_builtin_in_shell(p->cmds);

    /* Resolve everything first: a name the shell cannot resolve is the one
     * failure that must stop the sequence, and the parent has to see it. */
    paths = xmalloc((size_t)n * sizeof *paths);
    for (i = 0, c = p->cmds; c != NULL; c = c->next, i++) {
        paths[i] = NULL;
        if (is_builtin_cmd(c))
            continue;
        paths[i] = exec_resolve(c->argv[0]);
        if (paths[i] == NULL) {
            err_printf("%s: command not found (%s)", CSHELL_NAME,
                       display_name(c->argv[0]));
            for (int k = 0; k < i; k++)
                free(paths[k]);
            free(paths);
            if (launch_failed != NULL)
                *launch_failed = 1;
            return 127;
        }
    }

    plans = xmalloc((size_t)n * sizeof *plans);
    pids  = xmalloc((size_t)n * sizeof *pids);

    /* Open every redirection before anything is executed. */
    for (i = 0, c = p->cmds; c != NULL; c = c->next, i++) {
        if (redirect_plan(c, &plans[i]) != 0) {
            for (int k = 0; k < i; k++)
                redirect_release(&plans[k]);
            goto fail;
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
                goto fail;
            }
        }
    }

    if (p->background && pipe(sync_fd) != 0)
        sync_fd[0] = sync_fd[1] = -1;

    /* SIGCHLD stays blocked from here: the handler must not reap a child of
     * this pipeline before it has been recorded, and for a foreground group it
     * must not reap one the shell is about to wait for itself. */
    jobs_block(&saved);

    for (i = 0, c = p->cmds; c != NULL; c = c->next, i++) {
        pid_t pid = fork();

        if (pid < 0) {
            err_printf("%s: unable to fork", CSHELL_NAME);
            pids[i] = -1;
            continue;
        }
        if (pid == 0) {
            if (p->background) {
                /* Every background group is a process group of its own, so it
                 * never shares the terminal with the shell: a read from the
                 * terminal raises SIGTTIN and stops it instead. */
                setpgid(0, pgid);
                close(sync_fd[1]);
                pending_close_in_child();
                wait_for_release(sync_fd[0]);
            } else {
                pending_close_in_child();
            }
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

            child_exec(c, paths[i]);
        }
        if (p->background) {
            /* Set in both processes, so neither can race the other. */
            setpgid(pid, pgid);
            if (pgid == 0)
                pgid = pid;
        }
        pids[i] = pid;
    }

    /* The shell must hold no pipe fd, or the readers would never see EOF. */
    for (i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    pipeline_text(p, text, sizeof text);
    job = jobs_new(text, p->background);
    if (job == NULL)
        err_printf("%s: too many jobs", CSHELL_NAME);
    /* one entry per stage that actually started */
    for (i = 0, c = p->cmds; c != NULL; c = c->next, i++) {
        if (pids[i] > 0)
            jobs_add_proc(job, pids[i], display_name(c->argv[0]));
    }
    if (job != NULL)
        job->pgid = p->background ? pgid : getpgrp();

    if (p->background) {
        int spooled = 0;

        /* Output that is fanned out into several files can only be copied once
         * the job has finished, so the plans travel with it. */
        for (i = 0; i < n; i++)
            spooled |= plans[i].out_is_temp;
        if (spooled && job != NULL) {
            job->plans  = plans;
            job->nplans = n;
            plans = NULL;
        } else {
            for (i = 0; i < n; i++)
                redirect_release(&plans[i]);
        }

        printf("[%d] %d\n", job != NULL ? job->number : 0, (int)pids[0]);
        fflush(stdout);

        close(sync_fd[0]);
        pending_push(sync_fd[1]);
        jobs_unblock(&saved);

        if (pids[0] < 0 && launch_failed != NULL)
            *launch_failed = 1;
        status = 0;
    } else {
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
        jobs_remove(job);
        jobs_unblock(&saved);

        for (i = 0; i < n; i++) {
            redirect_finish(&plans[i]);
            redirect_release(&plans[i]);
        }
    }

    for (i = 0; i < n; i++)
        free(paths[i]);
    free(paths);
    free(pipes);
    free(plans);
    free(pids);
    return status;

fail:
    for (i = 0; i < n; i++)
        free(paths[i]);
    free(paths);
    free(plans);
    free(pids);
    return 1;
}
