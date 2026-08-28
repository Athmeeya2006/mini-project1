/* lexer.h - turns a raw input line into a token list.
 *
 * Implements the character classes and token grammar from the spec:
 *   special -> | & > < ;        quote -> " '        escape -> backslash
 * with maximal munch (so ">>" is one token) and full quote/escape removal.
 */
#ifndef CSHELL_LEXER_H
#define CSHELL_LEXER_H

typedef enum {
    TOK_WORD,
    TOK_PIPE,   /* |  */
    TOK_AMP,    /* &  */
    TOK_SEMI,   /* ;  */
    TOK_LT,     /* <  */
    TOK_GT,     /* >  */
    TOK_GTGT,   /* >> */
    TOK_EOF
} TokenType;

typedef struct Token {
    TokenType     type;
    char         *value; /* word only: quotes stripped, escapes applied */
    struct Token *next;
} Token;

/* Tokenise `line`. Returns 0 and stores a list ending in TOK_EOF in *out,
 * or -1 on a lexical error (unterminated quote / trailing backslash). */
int  lex_line(const char *line, Token **out);
void token_list_free(Token *t);

#endif /* CSHELL_LEXER_H */
