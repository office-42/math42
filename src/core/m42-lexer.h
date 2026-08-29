/* m42-lexer.h - the tokens of the math42 language
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  M42_TOK_END,
  M42_TOK_NUMBER,
  M42_TOK_IDENT,
  M42_TOK_STRING,
  M42_TOK_PLUS, M42_TOK_MINUS, M42_TOK_STAR, M42_TOK_SLASH, M42_TOK_CARET,
  M42_TOK_PERCENT, M42_TOK_DOT, M42_TOK_BACKSLASH,
  M42_TOK_LPAREN, M42_TOK_RPAREN,
  M42_TOK_LBRACKET, M42_TOK_RBRACKET,
  M42_TOK_LBRACE, M42_TOK_RBRACE,
  M42_TOK_COMMA, M42_TOK_SEMI, M42_TOK_COLON,
  M42_TOK_ASSIGN, M42_TOK_SETDELAYED,      /* =  := */
  M42_TOK_ARROW, M42_TOK_REPLACE,          /* -> /. */
  M42_TOK_RULEDELAYED,                     /* :> keeps its right side as written */
  M42_TOK_REPLACEREP,                      /* //. over and over until nothing changes */
  M42_TOK_CONDITION,                       /* /; the test a match has to pass */
  M42_TOK_INCREMENT, M42_TOK_DECREMENT,    /* x++ and x-- */
  M42_TOK_PLUSEQ, M42_TOK_MINUSEQ,         /* x += 2, x -= 2 */
  M42_TOK_STAREQ, M42_TOK_SLASHEQ,         /* x *= 2, x /= 2 */
  M42_TOK_BANG, M42_TOK_AT, M42_TOK_QUOTE, /* ! @ ' */
  M42_TOK_QUESTION,                        /* ?Sin asks what Sin is */
  M42_TOK_SLOT,                            /* # and #2, in a pure function */
  M42_TOK_AMP,                             /* & closes one */
  M42_TOK_MAP, M42_TOK_APPLY, M42_TOK_POSTFIX,  /* /@  @@  // */
  M42_TOK_SPAN,                            /* ;; between the ends of a part */
  M42_TOK_EQ, M42_TOK_NE, M42_TOK_LT, M42_TOK_LE, M42_TOK_GT, M42_TOK_GE,
  M42_TOK_AND, M42_TOK_OR, M42_TOK_NOT,
  M42_TOK_ERROR,
} M42TokenKind;

typedef struct {
  M42TokenKind kind;
  double       number;   /* M42_TOK_NUMBER, and which slot for M42_TOK_SLOT */
  char        *text;     /* IDENT, STRING and ERROR; owned */
  int          offset;   /* where in the source it starts */
  gboolean     space_before;  /* whitespace preceded it: [1 -2] is two elements */
} M42Token;

typedef struct {
  const char *src;
  int         pos;
} M42Lexer;

void m42_lexer_init (M42Lexer *lx, const char *src);

/* Returns the next token; the caller frees its text with m42_token_clear. */
M42Token m42_lexer_next (M42Lexer *lx);
void     m42_token_clear (M42Token *tok);

G_END_DECLS
