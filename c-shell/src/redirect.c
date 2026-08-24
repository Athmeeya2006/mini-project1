/* redirect.c - opens the files named by <, > and >> for one command.
 *
 * Several '<' redirections are concatenated into a single spool file so the
 * command sees one continuous stream, and several '>' / '>>' redirections make
 * the command write to a spool that is copied into every destination once it
 * has finished (each destination keeping its own truncate/append mode).
 */
#include "redirect.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "shell.h"
#include "utils.h"

#define OUT_MODE 0644

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void plan_init(RedirPlan *plan)
{
    plan->in_fd         = -1;
    plan->out_fd        = -1;
    plan->out_is_temp   = 0;
    plan->out_targets   = NULL;
    plan->n_out_targets = 0;
}

int redirect_plan(Command *cmd, RedirPlan *plan)
{
    int   *in_fds = NULL, *out_fds = NULL;
    int    n_in = 0, n_out = 0;
    Redir *r;

    plan_init(plan);
    if (cmd->redirs == NULL)
        return 0;

    for (r = cmd->redirs; r != NULL; r = r->next) {
        if (r->type == REDIR_IN)
            n_in++;
        else
            n_out++;
    }
    if (n_in > 0)
        in_fds = xmalloc((size_t)n_in * sizeof *in_fds);
    if (n_out > 0)
        out_fds = xmalloc((size_t)n_out * sizeof *out_fds);
    n_in = n_out = 0;

    /* Inputs first: a missing input file must not create the output files. */
    for (r = cmd->redirs; r != NULL; r = r->next) {
        if (r->type != REDIR_IN)
            continue;
        int fd = open(r->file, O_RDONLY);
        if (fd < 0) {
            err_printf("%s: no such file or directory", CSHELL_NAME);
            goto fail;
        }
        in_fds[n_in++] = fd;
    }

    for (r = cmd->redirs; r != NULL; r = r->next) {
        int flags;
        int fd;

        if (r->type == REDIR_IN)
            continue;
        flags = O_WRONLY | O_CREAT |
                (r->type == REDIR_OUT_APPEND ? O_APPEND : O_TRUNC);
        fd = open(r->file, flags, OUT_MODE);
        if (fd < 0) {
            err_printf("%s: unable to create file for writing", CSHELL_NAME);
            goto fail;
        }
        out_fds[n_out++] = fd;
    }

    if (n_in == 1) {
        plan->in_fd = in_fds[0];
    } else if (n_in > 1) {
        /* Concatenate every input into one spool, in the order given. */
        int spool = make_temp_fd();
        if (spool < 0) {
            err_printf("%s: no such file or directory", CSHELL_NAME);
            goto fail;
        }
        for (int i = 0; i < n_in; i++) {
            copy_fd(in_fds[i], spool);
            close(in_fds[i]);
            in_fds[i] = -1;
        }
        lseek(spool, 0, SEEK_SET);
        plan->in_fd = spool;
    }
    free(in_fds);
    in_fds = NULL;

    if (n_out == 1) {
        plan->out_fd = out_fds[0];
        free(out_fds);
    } else if (n_out > 1) {
        int spool = make_temp_fd();
        if (spool < 0) {
            err_printf("%s: unable to create file for writing", CSHELL_NAME);
            goto fail;
        }
        plan->out_fd        = spool;
        plan->out_is_temp   = 1;
        plan->out_targets   = out_fds;
        plan->n_out_targets = n_out;
    } else {
        free(out_fds);
    }
    return 0;

fail:
    for (int i = 0; i < n_in; i++)
        if (in_fds != NULL && in_fds[i] >= 0)
            close(in_fds[i]);
    for (int i = 0; i < n_out; i++)
        if (out_fds != NULL && out_fds[i] >= 0)
            close(out_fds[i]);
    free(in_fds);
    free(out_fds);
    close_fd(&plan->in_fd);
    close_fd(&plan->out_fd);
    plan_init(plan);
    return -1;
}

void redirect_apply(RedirPlan *plan)
{
    if (plan->in_fd >= 0) {
        dup2(plan->in_fd, STDIN_FILENO);
        if (plan->in_fd != STDIN_FILENO)
            close(plan->in_fd);
        plan->in_fd = -1;
    }
    if (plan->out_fd >= 0) {
        dup2(plan->out_fd, STDOUT_FILENO);
        if (plan->out_fd != STDOUT_FILENO)
            close(plan->out_fd);
        plan->out_fd = -1;
    }
    /* The destination files themselves are only needed by the parent. */
    for (int i = 0; i < plan->n_out_targets; i++)
        close_fd(&plan->out_targets[i]);
}

void redirect_close(RedirPlan *plan)
{
    close_fd(&plan->in_fd);
    close_fd(&plan->out_fd);
    for (int i = 0; i < plan->n_out_targets; i++)
        close_fd(&plan->out_targets[i]);
}

void redirect_finish(RedirPlan *plan)
{
    if (!plan->out_is_temp || plan->out_fd < 0)
        return;
    for (int i = 0; i < plan->n_out_targets; i++) {
        if (plan->out_targets[i] < 0)
            continue;
        lseek(plan->out_fd, 0, SEEK_SET);
        copy_fd(plan->out_fd, plan->out_targets[i]);
    }
}

void redirect_release(RedirPlan *plan)
{
    redirect_close(plan);
    free(plan->out_targets);
    plan_init(plan);
}
