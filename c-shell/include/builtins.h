/* builtins.h - the shell intrinsics (Part B) plus `exit`. */
#ifndef CSHELL_BUILTINS_H
#define CSHELL_BUILTINS_H

#include "parser.h"

/* 1 if argv[0] names a builtin. */
int builtin_is(const char *name);

/* Run a builtin; returns the status the command should report. */
int builtin_run(Command *cmd);

int builtin_hop(int argc, char **argv);
int builtin_reveal(int argc, char **argv);
int builtin_peek(int argc, char **argv);
int builtin_locate(int argc, char **argv);
int builtin_activities(int argc, char **argv);
int builtin_resume(int argc, char **argv);
int builtin_ping(int argc, char **argv);

#endif /* CSHELL_BUILTINS_H */
