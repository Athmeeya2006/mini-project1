/* b_activities.c - `activities`, the list of everything the shell has spawned
 * and not yet buried.
 *
 * One line per process group, oldest first, with its processes indented
 * underneath it. Processes that have already exited are left out, and a group
 * is only listed while it still has one.
 */
#include <signal.h>
#include <stdio.h>

#include "builtins.h"
#include "jobs.h"
#include "shell.h"

int builtin_activities(int argc, char **argv)
{
    Job     *jobs[JOBS_MAX];
    sigset_t saved;
    int      n;

    (void)argc;
    (void)argv;

    /* The reaper writes to the same table, so it has to be held off while the
     * listing is taken and printed. */
    jobs_block(&saved);
    n = jobs_snapshot(jobs, JOBS_MAX);

    for (int i = 0; i < n; i++) {
        Job *j     = jobs[i];
        int  shown = 0;

        for (int k = 0; k < j->nprocs; k++)
            if (j->procs[k].state != PROC_DONE)
                shown++;
        if (shown == 0)
            continue;

        printf("[%d] pgid %d\n", j->number, (int)j->pgid);
        for (int k = 0; k < j->nprocs; k++) {
            JobProc *pr = &j->procs[k];

            if (pr->state == PROC_DONE)
                continue;
            printf("  %d %-10s %s\n", (int)pr->pid, pr->name,
                   pr->state == PROC_STOPPED ? "Stopped" : "Running");
        }
    }

    fflush(stdout);
    jobs_unblock(&saved);
    return 0;
}
