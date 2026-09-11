/* b_ping.c - `ping <target> <signal_number>`, sending a signal to one process
 * or to a whole process group.
 *
 * Only what the shell itself started and still tracks can be a target: a pid
 * that exists on the system but did not come from this shell is as unknown
 * here as one that never existed.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include "builtins.h"
#include "jobs.h"
#include "shell.h"
#include "utils.h"

/* Signals wrap round at 64. */
#define PING_SIGNAL_MODULO 64

int builtin_ping(int argc, char **argv)
{
    sigset_t saved;
    long     typed, sig;
    int      sent = 0;

    /* The signal is checked first, so a bad one is reported whether or not the
     * target would have resolved. */
    if (argc != 3) {
        err_printf("ping: invalid syntax");
        return 1;
    }
    typed = parse_nonneg(argv[2]);
    if (typed < 0) {
        err_printf("ping: invalid syntax");
        return 1;
    }
    sig = typed % PING_SIGNAL_MODULO;

    jobs_block(&saved);
    if (argv[1][0] == '%') {
        /* A job number: the signal goes to every process in its group. */
        long  number = parse_nonneg(argv[1] + 1);
        Job  *j      = jobs_find_number((int)number);

        if (j != NULL && j->pgid > 0 && kill(-j->pgid, (int)sig) == 0)
            sent = 1;
    } else {
        long pid = parse_nonneg(argv[1]);

        if (jobs_find_pid((pid_t)pid) != NULL && kill((pid_t)pid, (int)sig) == 0)
            sent = 1;
    }
    jobs_unblock(&saved);

    if (!sent) {
        err_printf("ping: no such process found");
        return 1;
    }
    /* The number that was typed, not the one that was actually sent. */
    printf("Sent signal %s to %s\n", argv[2], argv[1]);
    fflush(stdout);
    return 0;
}
