/* redirect.h - opens the files named by <, > and >> and wires them to a
 * command's standard input/output.
 *
 * Several inputs are concatenated into one continuous stream, and several
 * outputs each receive a full copy of the command's output. */
#ifndef CSHELL_REDIRECT_H
#define CSHELL_REDIRECT_H

#include "parser.h"

typedef struct {
    int  in_fd;        /* fd to install on stdin, or -1 */
    int  out_fd;       /* fd to install on stdout, or -1 */
    int  out_is_temp;  /* out_fd is a spool that must be fanned out afterwards */
    int *out_targets;  /* the real destination fds when out_is_temp */
    int  n_out_targets;
} RedirPlan;

/* Open every file this command redirects. Returns 0, or -1 after printing the
 * appropriate diagnostic (the command must then not be executed). */
int  redirect_plan(Command *cmd, RedirPlan *plan);

/* Install the plan's fds on stdin/stdout. Called in the child. */
void redirect_apply(RedirPlan *plan);

/* Copy the spooled output into every destination file (parent, after wait). */
void redirect_finish(RedirPlan *plan);

/* Close every fd the plan holds without freeing the plan itself. */
void redirect_close(RedirPlan *plan);

/* Release everything the plan owns. */
void redirect_release(RedirPlan *plan);

#endif /* CSHELL_REDIRECT_H */
