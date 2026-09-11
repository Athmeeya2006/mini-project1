/* builtins.c - dispatch table for the shell intrinsics. */
#include "builtins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "utils.h"

static const char *const builtin_names[] = {
    "hop", "reveal", "peek", "locate", "activities", "exit", NULL
};

int builtin_is(const char *name)
{
    for (int i = 0; builtin_names[i] != NULL; i++) {
        if (strcmp(name, builtin_names[i]) == 0)
            return 1;
    }
    return 0;
}

int builtin_run(Command *cmd)
{
    const char *name = cmd->argv[0];
    int         status;

    if (strcmp(name, "hop") == 0)
        status = builtin_hop(cmd->argc, cmd->argv);
    else if (strcmp(name, "reveal") == 0)
        status = builtin_reveal(cmd->argc, cmd->argv);
    else if (strcmp(name, "peek") == 0)
        status = builtin_peek(cmd->argc, cmd->argv);
    else if (strcmp(name, "locate") == 0)
        status = builtin_locate(cmd->argc, cmd->argv);
    else if (strcmp(name, "activities") == 0)
        status = builtin_activities(cmd->argc, cmd->argv);
    else if (strcmp(name, "exit") == 0) {
        g_shell.should_exit = 1;
        status = 0;
    } else
        status = 1;

    /* stdout may currently point at a redirection target, so make sure
     * everything has left the stdio buffer before it is restored. */
    fflush(stdout);
    return status;
}
