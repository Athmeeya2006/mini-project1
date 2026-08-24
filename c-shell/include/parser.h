/* parser.h - validates the token stream against the assignment's right linear
 * grammar and builds the structure the executor walks.
 *
 *   LINE -> e | WORD ARG
 *   ARG  -> e | WORD ARG | OP_LT TGT | OP_GT TGT | OP_GTGT TGT
 *         | OP_PIPE CMD | OP_SEMI CMD | OP_AMP BG
 *   CMD  -> WORD ARG      TGT -> WORD ARG      BG -> e | WORD ARG
 */
#ifndef CSHELL_PARSER_H
#define CSHELL_PARSER_H

#include "lexer.h"

typedef enum {
    REDIR_IN,         /* <  */
    REDIR_OUT_TRUNC,  /* >  */
    REDIR_OUT_APPEND  /* >> */
} RedirType;

typedef struct Redir {
    RedirType     type;
    char         *file;
    struct Redir *next;
} Redir;

/* One stage of a pipeline: a command with its own redirections. */
typedef struct Command {
    char          **argv; /* NULL terminated */
    int             argc;
    Redir          *redirs;
    struct Command *next; /* next stage after a '|' */
} Command;

/* One command group: a whole pipeline, plus whether '&' followed it. */
typedef struct Pipeline {
    Command         *cmds;
    int              background;
    struct Pipeline *next; /* next group after ';' or '&' */
} Pipeline;

/* Returns 0 on success (*out is NULL for a blank line), -1 on a syntax error. */
int  parse_tokens(Token *toks, Pipeline **out);
void pipeline_list_free(Pipeline *p);

#endif /* CSHELL_PARSER_H */
