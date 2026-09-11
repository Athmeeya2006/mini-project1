/* jobs.h - the table of everything the shell has spawned, and the SIGCHLD
 * reaper that keeps it up to date.
 *
 * Every pipeline the shell launches gets an entry, foreground or background.
 * Job numbers come from a counter that only ever goes up, so a number is never
 * reused within a session. The table is shared with the SIGCHLD handler, so
 * every access made outside the handler has to run with SIGCHLD blocked.
 */
#ifndef CSHELL_JOBS_H
#define CSHELL_JOBS_H

#include <signal.h>
#include <sys/types.h>

#include "redirect.h"

#define JOBS_MAX      64
#define JOB_PROCS_MAX 16
#define JOB_NAME_MAX  64
#define JOB_CMD_MAX   512

typedef enum { PROC_RUNNING, PROC_STOPPED, PROC_DONE } ProcState;

typedef struct {
    pid_t     pid;
    char      name[JOB_NAME_MAX];
    ProcState state;
} JobProc;

typedef struct {
    int        used;
    int        number;        /* session wide and never reused; 0 = none yet */
    pid_t      pgid;
    int        background;
    int        notify;        /* announce the job when it finishes */
    int        nprocs;
    int        ndone;
    int        leader_status; /* wait status of the first process */
    JobProc    procs[JOB_PROCS_MAX];
    char       cmd[JOB_CMD_MAX]; /* the command line, for reporting */
    RedirPlan *plans;         /* redirections still owed a fan out, or NULL */
    int        nplans;
} Job;

/* Install the SIGCHLD handler and clear the table. */
void jobs_init(void);

/* SIGCHLD must be blocked around every access made outside the handler. */
void jobs_block(sigset_t *saved);
void jobs_unblock(const sigset_t *saved);

/* Claim a slot for a group the shell has just launched. A background group
 * takes a job number straight away; a foreground one only gets a number if it
 * is later stopped, which is what keeps the numbering in step with what the
 * user has actually been told about. NULL when the table is full. */
Job *jobs_new(const char *cmd, int background);

/* Give a job that has no number yet the next one. */
int  jobs_assign_number(Job *j);
void jobs_add_proc(Job *j, pid_t pid, const char *name);
void jobs_remove(Job *j);

/* The job with this number, or the one holding this pid, or NULL. Only
 * processes that have not exited count. Must run with SIGCHLD blocked. */
Job *jobs_find_number(int number);
Job *jobs_find_pid(pid_t pid);

/* Mark every process of a job that has not exited as stopped, or as running
 * again after a SIGCONT. */
void jobs_mark_stopped(Job *j);
void jobs_mark_running(Job *j);

/* Mark a job stopped, give it a number if it has none, and announce it as
 * "[n] + Stopped    <command>". The leading newline puts the line under the
 * "^Z" the terminal has just echoed. */
void jobs_report_stopped(Job *j);

/* 1 if any tracked job has a stopped process. */
int jobs_any_stopped(void);

/* Send SIGHUP to the process group of every job still on the books, on the
 * way out. Does not wait for any of them. */
void jobs_hangup(void);

/* The live jobs that have a number, oldest first, for `activities`. Returns
 * how many were written to `out`. Must run with SIGCHLD blocked. */
int jobs_snapshot(Job **out, int max);

/* Retire the jobs whose processes have all exited, fanning out whatever
 * redirections they still owe. Blocks SIGCHLD itself. */
void jobs_sweep(void);

/* While the shell waits at the prompt a completion message has to start on a
 * line of its own, so the handler is told when that is the case. */
void jobs_set_at_prompt(int at_prompt);

/* 1 if the handler has printed something since this was last called. The
 * reader loop uses it to decide whether an interrupted read needs a fresh
 * prompt or should simply carry on. */
int jobs_take_notified(void);

#endif /* CSHELL_JOBS_H */
