/* parser.c - a five state recogniser for the assignment's right linear grammar.
 *
 * Because the grammar is right linear the parser needs no stack: the current
 * non terminal is a single state, and each token either advances it or is a
 * syntax error. The structure built along the way is a list of pipelines
 * (command groups separated by ';' or '&'), each a list of commands.
 */
#include "parser.h"

#include <stdlib.h>

#include "utils.h"

typedef enum {
    P_START,    /* LINE: expect WORD, or end of line for a blank line */
    P_ARG,      /* ARG:  more words, an operator, or end of line */
    P_NEED_TGT, /* TGT:  the file name after < > >> */
    P_NEED_CMD, /* CMD:  the command after | or ; */
    P_BG        /* BG:   end of line, or the command after & */
} ParseState;

static Command *command_new(void)
{
    Command *c = xmalloc(sizeof *c);

    c->argv   = xmalloc(sizeof *c->argv);
    c->argv[0] = NULL;
    c->argc   = 0;
    c->redirs = NULL;
    c->next   = NULL;
    return c;
}

static void command_add_arg(Command *c, const char *arg)
{
    c->argv = xrealloc(c->argv, (size_t)(c->argc + 2) * sizeof *c->argv);
    c->argv[c->argc++] = xstrdup(arg);
    c->argv[c->argc]   = NULL;
}

static void command_add_redir(Command *c, RedirType type, const char *file)
{
    Redir *r = xmalloc(sizeof *r);
    Redir *it;

    r->type = type;
    r->file = xstrdup(file);
    r->next = NULL;

    if (c->redirs == NULL) {
        c->redirs = r;
        return;
    }
    for (it = c->redirs; it->next != NULL; it = it->next)
        ;
    it->next = r;
}

static Pipeline *pipeline_new(void)
{
    Pipeline *p = xmalloc(sizeof *p);

    p->cmds       = NULL;
    p->background = 0;
    p->next       = NULL;
    return p;
}

static void command_free(Command *c)
{
    while (c != NULL) {
        Command *next = c->next;
        Redir   *r    = c->redirs;

        while (r != NULL) {
            Redir *rn = r->next;
            free(r->file);
            free(r);
            r = rn;
        }
        for (int i = 0; i < c->argc; i++)
            free(c->argv[i]);
        free(c->argv);
        free(c);
        c = next;
    }
}

void pipeline_list_free(Pipeline *p)
{
    while (p != NULL) {
        Pipeline *next = p->next;
        command_free(p->cmds);
        free(p);
        p = next;
    }
}

int parse_tokens(Token *toks, Pipeline **out)
{
    ParseState state    = P_START;
    Pipeline  *head     = NULL, *cur_pipe = NULL;
    Command   *cur_cmd  = NULL;
    RedirType  pending_redir = REDIR_IN;
    int        start_new_group = 0; /* the WORD after ';'/'&' opens a group */
    Token     *t;

    *out = NULL;

    for (t = toks; t != NULL; t = t->next) {
        switch (state) {
        case P_START:
            if (t->type == TOK_EOF)
                goto done;              /* LINE -> e : a blank line is legal */
            if (t->type != TOK_WORD)
                goto syntax_error;
            head = cur_pipe = pipeline_new();
            cur_cmd = command_new();
            cur_pipe->cmds = cur_cmd;
            command_add_arg(cur_cmd, t->value);
            state = P_ARG;
            break;

        case P_ARG:
            switch (t->type) {
            case TOK_EOF:
                goto done;              /* ARG -> e */
            case TOK_WORD:
                command_add_arg(cur_cmd, t->value);
                break;
            case TOK_LT:
                pending_redir = REDIR_IN;
                state = P_NEED_TGT;
                break;
            case TOK_GT:
                pending_redir = REDIR_OUT_TRUNC;
                state = P_NEED_TGT;
                break;
            case TOK_GTGT:
                pending_redir = REDIR_OUT_APPEND;
                state = P_NEED_TGT;
                break;
            case TOK_PIPE:
                start_new_group = 0;
                state = P_NEED_CMD;
                break;
            case TOK_SEMI:
                start_new_group = 1;
                state = P_NEED_CMD;
                break;
            case TOK_AMP:
                cur_pipe->background = 1;
                state = P_BG;
                break;
            }
            break;

        case P_NEED_TGT:
            if (t->type != TOK_WORD)
                goto syntax_error;      /* TGT -> WORD ARG */
            command_add_redir(cur_cmd, pending_redir, t->value);
            state = P_ARG;
            break;

        case P_NEED_CMD:
            if (t->type != TOK_WORD)
                goto syntax_error;      /* CMD -> WORD ARG */
            if (start_new_group) {
                Pipeline *np = pipeline_new();
                cur_pipe->next = np;
                cur_pipe = np;
                cur_cmd = command_new();
                cur_pipe->cmds = cur_cmd;
            } else {
                Command *nc = command_new();
                cur_cmd->next = nc;
                cur_cmd = nc;
            }
            command_add_arg(cur_cmd, t->value);
            state = P_ARG;
            break;

        case P_BG:
            if (t->type == TOK_EOF)
                goto done;              /* BG -> e */
            if (t->type != TOK_WORD)
                goto syntax_error;      /* BG -> WORD ARG */
            {
                Pipeline *np = pipeline_new();
                cur_pipe->next = np;
                cur_pipe = np;
                cur_cmd = command_new();
                cur_pipe->cmds = cur_cmd;
            }
            command_add_arg(cur_cmd, t->value);
            state = P_ARG;
            break;
        }
    }

syntax_error:
    pipeline_list_free(head);
    return -1;

done:
    *out = head;
    return 0;
}
