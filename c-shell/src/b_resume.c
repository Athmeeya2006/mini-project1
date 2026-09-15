/* b_resume.c - `resume %n fg|bg`, putting a stopped or backgrounded job back
 * to work either in the foreground or in the background.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "builtins.h"
#include "jobs.h"
#include "shell.h"
#include "utils.h"

/* Set by the timeout timer. The job is waited for with the signal handler
 * installed without SA_RESTART, so the wait comes back when it fires. */
static volatile sig_atomic_t g_timed_out;

static void alarm_handler(int sig)
{
    (void)sig;
    g_timed_out = 1;
}

/* The timer repeats every second until it is taken down again. A signal that
 * arrives in the gap between the flag being checked and the wait starting
 * would otherwise be missed and the wait would never come back. */
static void timer_set(long seconds)
{
    struct itimerval it;

    memset(&it, 0, sizeof it);
    it.it_value.tv_sec    = seconds;
    it.it_interval.tv_sec = seconds > 0 ? 1 : 0;
    setitimer(ITIMER_REAL, &it, NULL);
}

static void timer_clear(void)
{
    struct itimerval it;

    memset(&it, 0, sizeof it);
    setitimer(ITIMER_REAL, &it, NULL);
}

/* Wait for everything in `j` that has not exited yet. Returns 1 if the job
 * stopped again, 0 if it finished, and leaves g_timed_out set if the timer
 * went off first. *interrupted is set when Ctrl-C ended it, which leaves the
 * cursor after the echoed "^C". */
static int wait_for_job(Job *j, int *status, int *interrupted)
{
    int stopped = 0;

    for (int k = 0; k < j->nprocs && !stopped && !g_timed_out; k++) {
        JobProc *pr = &j->procs[k];
        int      wstatus = 0;

        if (pr->state == PROC_DONE)
            continue;
        for (;;) {
            pid_t r;

            if (g_timed_out)
                break;
            r = waitpid(pr->pid, &wstatus, WUNTRACED);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                /* Already gone; nothing left to wait for. */
                pr->state = PROC_DONE;
                j->ndone++;
                break;
            }
            if (WIFSTOPPED(wstatus)) {
                stopped = 1;
                break;
            }
            pr->state = PROC_DONE;
            j->ndone++;
            if (k == 0)
                j->leader_status = wstatus;
            if (WIFSIGNALED(wstatus) && (WTERMSIG(wstatus) == SIGINT ||
                                         WTERMSIG(wstatus) == SIGQUIT))
                *interrupted = 1;
            *status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
            break;
        }
    }
    return stopped;
}

static int resume_fg(Job *j, int has_timeout, long seconds)
{
    struct sigaction sa, old_sa;
    sigset_t         saved;
    int              status = 0, stopped, interrupted = 0, timed_out;

    /* Exactly what a foreground launch shows. */
    printf("%s\n", j->cmd);
    fflush(stdout);

    /* As in a normal foreground launch, the reaper must not take the children
     * the shell is about to wait for itself. */
    jobs_block(&saved);
    if (g_shell.interactive && j->pgid > 0)
        tcsetpgrp(g_shell.terminal_fd, j->pgid);
    kill(-j->pgid, SIGCONT);
    jobs_mark_running(j);

    g_timed_out = 0;
    if (has_timeout) {
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = alarm_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; /* no SA_RESTART: the wait has to come back */
        sigaction(SIGALRM, &sa, &old_sa);
        if (seconds == 0)
            g_timed_out = 1;
        else
            timer_set(seconds);
    }

    stopped = wait_for_job(j, &status, &interrupted);

    if (has_timeout) {
        /* Whatever happened first, the timer is taken down again. */
        timer_clear();
        sigaction(SIGALRM, &old_sa, NULL);
    }
    if (g_shell.interactive)
        tcsetpgrp(g_shell.terminal_fd, g_shell.pgid);

    if (interrupted) {
        putchar('\n');
        fflush(stdout);
    }

    /* The timer can go off in the moment between the job finishing or
     * stopping and the timer being taken down; that is not a timeout. */
    timed_out = g_timed_out && !stopped && j->ndone < j->nprocs;

    if (timed_out) {
        kill(-j->pgid, SIGTERM);
        err_printf("resume: job timed out");
        /* It has been killed, so it is no longer a job. Its children are
         * reaped by the handler once the signal is unblocked. */
        jobs_remove(j);
        status = 1;
    } else if (stopped) {
        jobs_report_stopped(j);
        status = 128 + SIGTSTP;
    } else {
        jobs_remove(j);
    }
    jobs_unblock(&saved);
    return status;
}

static int resume_bg(Job *j)
{
    kill(-j->pgid, SIGCONT);
    jobs_mark_running(j);
    /* It is a background job from now on, so its end is announced. */
    j->background = 1;
    j->notify     = 1;
    printf("[%d] + Running    %s\n", jobs_assign_number(j), j->cmd);
    fflush(stdout);
    return 0;
}

int builtin_resume(int argc, char **argv)
{
    sigset_t saved;
    Job     *j;
    long     number, seconds = 0;
    int      want_fg, has_timeout = 0;
    int      status;

    /* resume %n fg [--timeout s] | resume %n bg */
    if (argc < 3 || argv[1][0] != '%') {
        err_printf("resume: invalid syntax");
        return 1;
    }
    number = parse_nonneg(argv[1] + 1);
    if (number < 0) {
        err_printf("resume: invalid syntax");
        return 1;
    }
    if (strcmp(argv[2], "fg") == 0) {
        want_fg = 1;
    } else if (strcmp(argv[2], "bg") == 0) {
        want_fg = 0;
    } else {
        err_printf("resume: invalid syntax");
        return 1;
    }
    if (argc > 3) {
        if (!want_fg || argc != 5 || strcmp(argv[3], "--timeout") != 0) {
            err_printf("resume: invalid syntax");
            return 1;
        }
        seconds = parse_nonneg(argv[4]);
        if (seconds < 0) {
            err_printf("resume: invalid syntax");
            return 1;
        }
        has_timeout = 1;
    }

    jobs_block(&saved);
    j = jobs_find_number(number);
    if (j == NULL || j->pgid <= 0) {
        jobs_unblock(&saved);
        err_printf("resume: no such job");
        return 1;
    }
    if (!want_fg) {
        status = resume_bg(j);
        jobs_unblock(&saved);
        return status;
    }
    jobs_unblock(&saved);

    return resume_fg(j, has_timeout, seconds);
}
