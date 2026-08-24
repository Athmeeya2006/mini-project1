/* lexer.c - character level scanner.
 *
 * A six state DFA walks the line once.  Quote and escape processing happens
 * while scanning, so a WORD token already carries its final value:
 *
 *   S_START     between tokens, whitespace is skipped
 *   S_WORD      inside an unquoted fragment
 *   S_WORD_ESC  just consumed a backslash outside quotes
 *   S_DQ        inside "..."      S_DQ_ESC  backslash inside "..."
 *   S_SQ        inside '...'      (verbatim, no escapes)
 */
#include "lexer.h"

#include <stdlib.h>
#include <string.h>

#include "utils.h"

typedef enum {
    S_START,
    S_WORD,
    S_WORD_ESC,
    S_DQ,
    S_DQ_ESC,
    S_SQ
} LexState;

static int is_space_ch(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_special_ch(char c)
{
    return c == '|' || c == '&' || c == '>' || c == '<' || c == ';';
}

static Token *token_new(TokenType type, char *value)
{
    Token *t = xmalloc(sizeof *t);

    t->type  = type;
    t->value = value;
    t->next  = NULL;
    return t;
}

static void token_append(Token **head, Token **tail, Token *t)
{
    if (*tail == NULL)
        *head = *tail = t;
    else {
        (*tail)->next = t;
        *tail = t;
    }
}

void token_list_free(Token *t)
{
    while (t != NULL) {
        Token *next = t->next;
        free(t->value);
        free(t);
        t = next;
    }
}

int lex_line(const char *line, Token **out)
{
    Token   *head = NULL, *tail = NULL;
    LexState st = S_START;
    Buf      word;
    int      in_word = 0; /* a word has begun, even if its value is empty */
    size_t   i = 0;

    buf_init(&word);
    *out = NULL;

    while (line[i] != '\0') {
        char c = line[i];

        switch (st) {
        case S_START:
            if (is_space_ch(c)) {
                i++;
            } else if (is_special_ch(c)) {
                TokenType tt;
                if (c == '>' && line[i + 1] == '>') { /* maximal munch */
                    tt = TOK_GTGT;
                    i += 2;
                } else {
                    switch (c) {
                    case '|': tt = TOK_PIPE; break;
                    case '&': tt = TOK_AMP;  break;
                    case ';': tt = TOK_SEMI; break;
                    case '<': tt = TOK_LT;   break;
                    default:  tt = TOK_GT;   break;
                    }
                    i++;
                }
                token_append(&head, &tail, token_new(tt, NULL));
            } else {
                /* Start a word and re-examine this character in S_WORD. */
                st      = S_WORD;
                in_word = 1;
            }
            break;

        case S_WORD:
            if (is_space_ch(c) || is_special_ch(c)) {
                token_append(&head, &tail, token_new(TOK_WORD, buf_release(&word)));
                buf_init(&word);
                in_word = 0;
                st      = S_START; /* the character is handled again there */
            } else if (c == '\\') {
                st = S_WORD_ESC;
                i++;
            } else if (c == '"') {
                st = S_DQ;
                i++;
            } else if (c == '\'') {
                st = S_SQ;
                i++;
            } else {
                buf_push(&word, c);
                i++;
            }
            break;

        case S_WORD_ESC:
            /* Outside quotes a backslash contributes the next char literally. */
            buf_push(&word, c);
            i++;
            st = S_WORD;
            break;

        case S_DQ:
            if (c == '"') {
                st = S_WORD;
                i++;
            } else if (c == '\\') {
                st = S_DQ_ESC;
                i++;
            } else {
                buf_push(&word, c);
                i++;
            }
            break;

        case S_DQ_ESC:
            /* Only \" and \\ are escapes here; anything else keeps both bytes. */
            if (c == '"' || c == '\\') {
                buf_push(&word, c);
            } else {
                buf_push(&word, '\\');
                buf_push(&word, c);
            }
            i++;
            st = S_DQ;
            break;

        case S_SQ:
            if (c == '\'') {
                st = S_WORD;
                i++;
            } else {
                buf_push(&word, c);
                i++;
            }
            break;
        }
    }

    /* End of line.  There is no line continuation, so any state that still
     * expects more input is a lexical error. */
    if (st == S_WORD_ESC || st == S_DQ || st == S_DQ_ESC || st == S_SQ) {
        buf_free(&word);
        token_list_free(head);
        return -1;
    }

    if (in_word) {
        token_append(&head, &tail, token_new(TOK_WORD, buf_release(&word)));
        buf_init(&word);
    }
    buf_free(&word);

    token_append(&head, &tail, token_new(TOK_EOF, NULL));
    *out = head;
    return 0;
}
