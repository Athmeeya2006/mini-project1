/* b_snoop.c - `snoop command [args...]` and `snoop -p pid`, a syscall tracer.
 *
 * The tracee is stopped twice per system call, once on the way in and once on
 * the way out, and the difference between the two timestamps is the time that
 * call took. A command is traced from its very first instruction by having the
 * child ask to be traced before it execs; an existing process is attached to
 * instead. When it is all over the calls are reported, busiest first.
 *
 * The syscall numbers are x86_64's. Anywhere else every call is reported by
 * number, since the numbering would be someone else's.
 */
#define _DEFAULT_SOURCE 1

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__)
#include <sys/user.h>
#include "syscalls.h"
#endif

#include "builtins.h"
#include "exec.h"
#include "jobs.h"
#include "shell.h"
#include "utils.h"

/* PTRACE_O_TRACESYSGOOD marks a syscall stop as SIGTRAP|0x80, which is what
 * tells one apart from a SIGTRAP the program raised itself. */
#define SYSCALL_STOP_SIG (SIGTRAP | 0x80)

#define SNOOP_MAX_CALLS 512

typedef struct {
    long   nr;
    long   calls;
    double seconds;
    int    order; /* first occurrence, which is how ties are broken */
} Call;

static Call g_calls[SNOOP_MAX_CALLS];
static int  g_ncalls;

static Call *call_for(long nr)
{
    for (int i = 0; i < g_ncalls; i++)
        if (g_calls[i].nr == nr)
            return &g_calls[i];
    if (g_ncalls == SNOOP_MAX_CALLS)
        return NULL;
    g_calls[g_ncalls].nr      = nr;
    g_calls[g_ncalls].calls   = 0;
    g_calls[g_ncalls].seconds = 0.0;
    g_calls[g_ncalls].order   = g_ncalls;
    return &g_calls[g_ncalls++];
}

static int call_cmp(const void *a, const void *b)
{
    const Call *x = a, *y = b;

    if (x->calls != y->calls)
        return x->calls > y->calls ? -1 : 1;
    return x->order - y->order;
}

static const char *call_name(long nr, char *buf, size_t cap)
{
#if defined(__x86_64__)
    if (nr >= 0 && nr <= SYSCALL_NAME_MAX && syscall_names[nr] != NULL)
        return syscall_names[nr];
#endif
    snprintf(buf, cap, "syscall_%ld", nr);
    return buf;
}

static double now_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Whether a syscall stop is the way in rather than the way out. */
static int at_syscall_entry(pid_t pid)
{
#if defined(__x86_64__)
    struct user_regs_struct regs;

    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) != 0)
        return 1;
    return (long)regs.rax == -ENOSYS;
#else
    (void)pid;
    return 1;
#endif
}

/* The number of the call the tracee is stopped on. */
static long syscall_number(pid_t pid)
{
#if defined(__x86_64__)
    struct user_regs_struct regs;

    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) != 0)
        return -1;
    return (long)regs.orig_rax;
#else
    (void)pid;
    return -1;
#endif
}

static void report(void)
{
    char buf[32];

    qsort(g_calls, (size_t)g_ncalls, sizeof *g_calls, call_cmp);
    printf("%-16s %-7s %s\n", "syscall", "calls", "time");
    for (int i = 0; i < g_ncalls; i++)
        printf("%-16s %-7ld %.3fs\n",
               call_name(g_calls[i].nr, buf, sizeof buf),
               g_calls[i].calls, g_calls[i].seconds);
    fflush(stdout);
}

/* Run the tracee until it is gone, counting and timing every call on the way.
 * The tracee is already stopped when this is entered. */
static void trace_loop(pid_t pid, int attached)
{
    int    in_syscall = 0;
    int    first_stop = attached;
    int    deliver = 0;
    long   pending_nr = -1;
    double entered_at = 0.0;

    ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);

    for (;;) {
        int status;

        if (ptrace(PTRACE_SYSCALL, pid, 0, deliver) != 0)
            return;
        deliver = 0;

        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR)
                return;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            return;
        if (!WIFSTOPPED(status))
            return;

        if (WSTOPSIG(status) == SYSCALL_STOP_SIG) {
            if (first_stop) {
                first_stop = 0;
                /* A process attached to in the middle of a call (sleeping
                 * in one, most likely) stops first on that call's way out.
                 * On entry the kernel has already put -ENOSYS in the return
                 * register, so anything else there is an exit, whose entry
                 * was never seen: it is not counted, and the next stop is an
                 * entry again. */
                if (!at_syscall_entry(pid))
                    continue;
            }
            if (!in_syscall) {
                Call *c = call_for(syscall_number(pid));

                /* Counted on the way in, so a call that never comes back -
                 * exit_group, most of all - is still counted. */
                pending_nr = c != NULL ? c->nr : -1;
                if (c != NULL)
                    c->calls++;
                entered_at = now_seconds();
                in_syscall = 1;
            } else {
                Call *c = pending_nr >= 0 ? call_for(pending_nr) : NULL;

                if (c != NULL)
                    c->seconds += now_seconds() - entered_at;
                in_syscall = 0;
            }
        } else {
            int sig = WSTOPSIG(status);

            /* Not our stop: the tracee was signalled, so pass it on. A stop
             * signal is the exception. It would put the tracee in a group
             * stop that only its tracer could get it out of, and the shell is
             * in no position to manage that while it is waiting here, so
             * Ctrl-Z does nothing to something being traced. */
            deliver = (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU)
                          ? 0
                          : sig;
        }
    }
}

static int snoop_attach(pid_t pid)
{
    char        proc_dir[64];
    struct stat st;
    int         status;

    snprintf(proc_dir, sizeof proc_dir, "/proc/%d", (int)pid);
    if (stat(proc_dir, &st) != 0) {
        err_printf("snoop: no such process");
        return 1;
    }
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) != 0) {
        if (errno == ESRCH)
            err_printf("snoop: no such process");
        else
            err_printf("snoop: unable to trace process");
        return 1;
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            err_printf("snoop: unable to trace process");
            return 1;
        }
    }
    trace_loop(pid, 1);
    /* The shell was not the one to reap it, so its job entry is put right. */
    jobs_mark_exited(pid);
    report();
    return 0;
}

static int snoop_command(int argc, char **argv)
{
    char *path = exec_resolve(argv[0]);
    pid_t pid;
    int   status;

    (void)argc;
    if (path == NULL) {
        err_printf("snoop: command not found");
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        err_printf("snoop: unable to fork");
        free(path);
        return 1;
    }
    if (pid == 0) {
        sigset_t empty;

        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        /* Ask to be traced, then exec: the first stop is the exec itself, so
         * nothing the command does is missed. */
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(127);
        execv(path, argv);
        err_printf("snoop: command not found");
        _exit(127);
    }
    free(path);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            err_printf("snoop: unable to trace process");
            return 1;
        }
    }
    if (WIFEXITED(status)) /* it never got as far as the exec */
        return WEXITSTATUS(status);

    trace_loop(pid, 0);
    report();
    return 0;
}

int builtin_snoop(int argc, char **argv)
{
    sigset_t saved;
    int      status;

    if (argc < 2) {
        err_printf("snoop: invalid syntax");
        return 1;
    }

    g_ncalls = 0;
    /* The shell's reaper must not take the stops this is waiting for. */
    jobs_block(&saved);
    if (strcmp(argv[1], "-p") == 0) {
        long pid = argc == 3 ? parse_nonneg(argv[2]) : -1;

        if (argc != 3) {
            err_printf("snoop: invalid syntax");
            status = 1;
        } else if (pid <= 0) {
            err_printf("snoop: no such process");
            status = 1;
        } else {
            status = snoop_attach((pid_t)pid);
        }
    } else {
        status = snoop_command(argc - 1, argv + 1);
    }
    jobs_unblock(&saved);
    return status;
}
