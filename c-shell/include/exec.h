/* exec.h - command resolution and pipeline execution. */
#ifndef CSHELL_EXEC_H
#define CSHELL_EXEC_H

#include "parser.h"

/* Resolve a command name to a runnable path following the Part C1 rules:
 * a name containing '/' is used literally, a bare name is looked up in the
 * current directory first and then in PATH, and a '%' prefix forces PATH.
 * Returns a malloc'd path or NULL when nothing executable was found. */
char *exec_resolve(const char *name);

/* Run one command group. A foreground group is waited for and its status
 * returned; a background group is launched, announced as "[n] pid" and left
 * running. *launch_failed is set when the group could not be started at all,
 * which is what stops the rest of a ';' or '&' sequence. */
int exec_pipeline(Pipeline *p, int *launch_failed);

/* Background children are held just before exec until the shell has printed
 * their "[n] pid" line, so that line always comes first. This lets them go:
 * the reader loop calls it before running a foreground group and at the end
 * of every input line. */
void exec_release_background(void);

#endif /* CSHELL_EXEC_H */
