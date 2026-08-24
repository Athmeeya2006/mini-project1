/* exec.h - command resolution and pipeline execution. */
#ifndef CSHELL_EXEC_H
#define CSHELL_EXEC_H

#include "parser.h"

/* Resolve a command name to a runnable path following the Part C1 rules:
 * a name containing '/' is used literally, a bare name is looked up in the
 * current directory first and then in PATH, and a '%' prefix forces PATH.
 * Returns a malloc'd path or NULL when nothing executable was found. */
char *exec_resolve(const char *name);

/* Fork, wire up pipes and redirections, and wait for the whole pipeline. */
int exec_pipeline(Pipeline *p);

#endif /* CSHELL_EXEC_H */
