/* jobs.c - the job table and the SIGCHLD reaper.
 *
 * The handler runs at arbitrary points in the shell's execution, so it only
 * uses async signal safe calls: it reaps with waitpid(WNOHANG) and reports a
 * finished job with a single write(), formatting the message by hand rather
 * than through stdio. Everything that needs more than that - closing spooled
 * redirections, freeing the slot - is left to jobs_sweep(), which the reader
 * loop calls with the signal blocked.
 */
#include "jobs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static Job          g_jobs[JOBS_MAX];
static int          g_next_number = 1;
static volatile sig_atomic_t g_at_prompt;
static volatile sig_atomic_t g_notified;

void jobs_block(sigset_t *saved)
{
    sigset_t block;

    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, saved);
}

void jobs_unblock(const sigset_t *saved)
{
    sigprocmask(SIG_SETMASK, saved, NULL);
}

void jobs_set_at_prompt(int at_prompt)
{
    g_at_prompt = at_prompt ? 1 : 0;
}

int jobs_take_notified(void)
{
    int was = g_notified;

    g_notified = 0;
    return was;
}

/* --- message formatting, safe to call from the handler --------------------- */

static void append_str(char *buf, size_t cap, size_t *len, const char *s)
{
    while (*s != '\0' && *len + 1 < cap)
        buf[(*len)++] = *s++;
}

static void append_num(char *buf, size_t cap, size_t *len, long v)
{
    char tmp[32];
    int  i = 0;

    if (v < 0) {
        append_str(buf, cap, len, "-");
        v = -v;
    }
    do {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    while (i > 0 && *len + 1 < cap)
        buf[(*len)++] = tmp[--i];
}

/* "<name> with pid <pid> exited normally", per D2. A job that was killed by a
 * signal exited abnormally; any exit status at all still counts as normal. */
static void report_finished(const Job *j)
{
    char   buf[JOB_NAME_MAX + 64];
    size_t len = 0;

    /* The prompt has already been printed and is waiting for input, so the
     * message needs a line of its own. */
    if (g_at_prompt)
        append_str(buf, sizeof buf, &len, "\n");
    append_str(buf, sizeof buf, &len, j->procs[0].name);
    append_str(buf, sizeof buf, &len, " with pid ");
    append_num(buf, sizeof buf, &len, (long)j->procs[0].pid);
    append_str(buf, sizeof buf, &len,
               WIFSIGNALED(j->leader_status) ? " exited abnormally\n"
                                             : " exited normally\n");
    (void)!write(STDOUT_FILENO, buf, len);
    g_notified = 1;
    /* The line has been broken now; a second message in the same burst goes
     * straight underneath this one. */
    g_at_prompt = 0;
}

/* --- the table ------------------------------------------------------------ */

static Job *find_by_pid(pid_t pid)
{
    for (int i = 0; i < JOBS_MAX; i++) {
        if (!g_jobs[i].used)
            continue;
        for (int k = 0; k < g_jobs[i].nprocs; k++)
            if (g_jobs[i].procs[k].pid == pid)
                return &g_jobs[i];
    }
    return NULL;
}

Job *jobs_new(const char *cmd, int background)
{
    for (int i = 0; i < JOBS_MAX; i++) {
        Job *j = &g_jobs[i];

        if (j->used)
            continue;
        memset(j, 0, sizeof *j);
        j->used       = 1;
        j->number     = background ? g_next_number++ : 0;
        j->background = background;
        j->notify     = background;
        snprintf(j->cmd, sizeof j->cmd, "%s", cmd);
        return j;
    }
    return NULL;
}

int jobs_assign_number(Job *j)
{
    if (j == NULL)
        return 0;
    if (j->number == 0)
        j->number = g_next_number++;
    return j->number;
}

void jobs_add_proc(Job *j, pid_t pid, const char *name)
{
    JobProc *pr;

    if (j == NULL || j->nprocs >= JOB_PROCS_MAX)
        return;
    pr = &j->procs[j->nprocs++];
    pr->pid   = pid;
    pr->state = PROC_RUNNING;
    snprintf(pr->name, sizeof pr->name, "%s", name);
}

/* Give back whatever the slot still owns. */
static void job_release(Job *j)
{
    for (int i = 0; i < j->nplans; i++) {
        redirect_finish(&j->plans[i]);
        redirect_release(&j->plans[i]);
    }
    free(j->plans);
    j->plans  = NULL;
    j->nplans = 0;
    j->used   = 0;
}

void jobs_remove(Job *j)
{
    if (j != NULL && j->used)
        job_release(j);
}

void jobs_sweep(void)
{
    sigset_t saved;

    jobs_block(&saved);
    for (int i = 0; i < JOBS_MAX; i++) {
        Job *j = &g_jobs[i];

        if (j->used && j->nprocs > 0 && j->ndone == j->nprocs)
            job_release(j);
    }
    jobs_unblock(&saved);
}

/* --- the reaper ----------------------------------------------------------- */

static void sigchld_handler(int sig)
{
    int   saved_errno = errno;
    pid_t pid;
    int   status;

    (void)sig;
    /* WNOHANG so the shell never blocks here; WUNTRACED so that a background
     * job stopped by the terminal (SIGTTIN on a read) is noticed too. */
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        Job *j = find_by_pid(pid);

        if (j == NULL)
            continue;
        for (int k = 0; k < j->nprocs; k++) {
            if (j->procs[k].pid != pid)
                continue;
            if (WIFSTOPPED(status)) {
                j->procs[k].state = PROC_STOPPED;
            } else if (j->procs[k].state != PROC_DONE) {
                j->procs[k].state = PROC_DONE;
                j->ndone++;
            }
        }
        if (WIFSTOPPED(status))
            continue;
        if (pid == j->procs[0].pid)
            j->leader_status = status;
        /* A pipeline is reported once, under the pid of its first stage. */
        if (j->ndone == j->nprocs && j->notify) {
            j->notify = 0;
            report_finished(j);
        }
    }
    errno = saved_errno;
}

void jobs_init(void)
{
    struct sigaction sa;

    memset(g_jobs, 0, sizeof g_jobs);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: the read at the prompt has to come back with EINTR so the
     * loop can redraw the prompt under the message the handler just printed. */
    sa.sa_flags = 0;
    sigaction(SIGCHLD, &sa, NULL);
}
