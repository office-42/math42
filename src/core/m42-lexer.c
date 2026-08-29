/* m42-lexer.c - see m42-lexer.h
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m42-lexer.h"

#include <ctype.h>
#include <string.h>

void
m42_lexer_init (M42Lexer *lx, const char *src)
{
  lx->src = src;
  lx->pos = 0;
}

void
m42_token_clear (M42Token *tok)
{
  g_free (tok->text);
  tok->text = NULL;
}

static M42Token
simple (M42TokenKind kind, int offset)
{
  M42Token t = { kind, 0.0, NULL, offset, FALSE };
  return t;
}

M42Token
m42_lexer_next (M42Lexer *lx)
{
  const char *s = lx->src;
  int start;
  gboolean space = FALSE;
  M42Token t;

  /* Whitespace, and (* Mathematica comments *). */
  for (;;)
    {
      while (s[lx->pos] != '\0' && isspace ((unsigned char) s[lx->pos]))
        {
          lx->pos++;
          space = TRUE;
        }
      if (s[lx->pos] == '(' && s[lx->pos + 1] == '*')
        {
          /* Comments nest, as they do in Mathematica, so that a
           * comment about a comment closes where it means to. */
          int depth = 1;

          lx->pos += 2;
          while (s[lx->pos] != 0 && depth > 0)
            {
              if (s[lx->pos] == '(' && s[lx->pos + 1] == '*')
                {
                  depth++;
                  lx->pos += 2;
                }
              else if (s[lx->pos] == '*' && s[lx->pos + 1] == ')')
                {
                  depth--;
                  lx->pos += 2;
                }
              else
                lx->pos++;
            }
          space = TRUE;
          continue;
        }
      break;
    }

  start = lx->pos;
  if (s[start] == '\0')
    return simple (M42_TOK_END, start);

  /* Numbers: 12, 1.5, .5, 2e10, 1.5E-3.  A trailing letter that is not
   * an exponent belongs to the next token, so 2x is 2 times x. */
  if (isdigit ((unsigned char) s[start]) ||
      (s[start] == '.' && isdigit ((unsigned char) s[start + 1])))
    {
      char *end;

      t = simple (M42_TOK_NUMBER, start);
      t.number = g_ascii_strtod (s + start, &end);

      /* A long run of plain digits is kept as it was written, so that
       * it can be read exactly rather than through a double. */
      {
        const char *at = s + start;
        gsize digits = 0;

        while (at[digits] >= '0' && at[digits] <= '9')
          digits++;
        if (digits > 15 && (gsize) (end - (s + start)) == digits)
          t.text = g_strndup (s + start, digits);
      }
      /* g_ascii_strtod would eat "inf"/"nan" -- those are names here --
       * and a bare "e" after digits that is not an exponent. */
      lx->pos = (int) (end - s);
      t.space_before = space;
      return t;
    }

  /* "a string", with the usual backslash escapes. */
  if (s[start] == '"')
    {
      GString *text = g_string_new (NULL);

      lx->pos = start + 1;
      while (s[lx->pos] != '\0' && s[lx->pos] != '"')
        {
          if (s[lx->pos] == '\\' && s[lx->pos + 1] != '\0')
            {
              char c = s[++lx->pos];
              g_string_append_c (text, c == 'n' ? '\n' : c == 't' ? '\t' : c);
            }
          else
            g_string_append_c (text, s[lx->pos]);
          lx->pos++;
        }
      if (s[lx->pos] != '"')
        {
          t = simple (M42_TOK_ERROR, start);
          t.text = g_strdup ("a string was not closed");
          g_string_free (text, TRUE);
        }
      else
        {
          lx->pos++;
          t = simple (M42_TOK_STRING, start);
          t.text = g_string_free (text, FALSE);
        }
      t.space_before = space;
      return t;
    }

  if (isalpha ((unsigned char) s[start]) || s[start] == '_')
    {
      t = simple (M42_TOK_IDENT, start);
      while (isalnum ((unsigned char) s[lx->pos]) || s[lx->pos] == '_')
        lx->pos++;
      t.text = g_strndup (s + start, lx->pos - start);
      t.space_before = space;
      return t;
    }

  /* # is the first argument of a pure function, #2 the second. */
  if (s[start] == '#')
    {
      t = simple (M42_TOK_SLOT, start);
      lx->pos = start + 1;
      t.number = 1;
      if (isdigit ((unsigned char) s[lx->pos]))
        {
          char *end;
          t.number = g_ascii_strtod (s + lx->pos, &end);
          lx->pos = (int) (end - s);
        }
      t.space_before = space;
      return t;
    }

  lx->pos++;
#define TWO(c2, kind2) if (s[lx->pos] == (c2)) { lx->pos++; t = simple (kind2, start); goto done; }
  switch (s[start])
    {
    case '+':
      TWO ('+', M42_TOK_INCREMENT);
      TWO ('=', M42_TOK_PLUSEQ);
      t = simple (M42_TOK_PLUS, start);
      break;
    case '-':
      TWO ('>', M42_TOK_ARROW);
      /* x-- is one token, so a--b is (a--) b, as Mathematica has it;
       * a - -b with a space between is two, as it should be. */
      TWO ('-', M42_TOK_DECREMENT);
      TWO ('=', M42_TOK_MINUSEQ);
      t = simple (M42_TOK_MINUS, start);
      break;
    case '*': TWO ('=', M42_TOK_STAREQ); t = simple (M42_TOK_STAR, start); break;
    case '/':
      /* /. is ReplaceAll unless a digit follows the dot, as in 1/.5 */
      if (s[lx->pos] == '.' && !isdigit ((unsigned char) s[lx->pos + 1]))
        {
          lx->pos++;
          t = simple (M42_TOK_REPLACE, start);
          break;
        }
      /* //. is ReplaceRepeated, and has to be looked for before //. */
      if (s[lx->pos] == '/' && s[lx->pos + 1] == '.')
        {
          lx->pos += 2;
          t = simple (M42_TOK_REPLACEREP, start);
          break;
        }
      TWO (';', M42_TOK_CONDITION);  /* pattern /; test */
      TWO ('=', M42_TOK_SLASHEQ);    /* x /= 2 */
      TWO ('@', M42_TOK_MAP);        /* f /@ list */
      TWO ('/', M42_TOK_POSTFIX);    /* list // f */
      t = simple (M42_TOK_SLASH, start);
      break;
    case '^': t = simple (M42_TOK_CARET, start); break;
    case '%': t = simple (M42_TOK_PERCENT, start); break;
    case '.':
      /* MATLAB's element-wise .* ./ .^ are what * / ^ already do. */
      TWO ('*', M42_TOK_STAR);
      TWO ('/', M42_TOK_SLASH);
      TWO ('^', M42_TOK_CARET);
      t = simple (M42_TOK_DOT, start);
      break;
    case '(': t = simple (M42_TOK_LPAREN, start); break;
    case ')': t = simple (M42_TOK_RPAREN, start); break;
    case '[': t = simple (M42_TOK_LBRACKET, start); break;
    case ']': t = simple (M42_TOK_RBRACKET, start); break;
    case '{': t = simple (M42_TOK_LBRACE, start); break;
    case '}': t = simple (M42_TOK_RBRACE, start); break;
    case ',': t = simple (M42_TOK_COMMA, start); break;
    case ';': TWO (';', M42_TOK_SPAN); t = simple (M42_TOK_SEMI, start); break;
    case ':':
      TWO ('=', M42_TOK_SETDELAYED);
      TWO ('>', M42_TOK_RULEDELAYED);
      t = simple (M42_TOK_COLON, start);
      break;
    case '=': TWO ('=', M42_TOK_EQ); t = simple (M42_TOK_ASSIGN, start); break;
    case '!': TWO ('=', M42_TOK_NE); t = simple (M42_TOK_BANG, start); break;
    case '~': TWO ('=', M42_TOK_NE); t = simple (M42_TOK_NOT, start); break;
    case '<': TWO ('=', M42_TOK_LE); t = simple (M42_TOK_LT, start); break;
    case '>': TWO ('=', M42_TOK_GE); t = simple (M42_TOK_GT, start); break;
    case '&': TWO ('&', M42_TOK_AND); t = simple (M42_TOK_AMP, start); break;
    case '|': TWO ('|', M42_TOK_OR); t = simple (M42_TOK_OR, start); break;
    case '@': TWO ('@', M42_TOK_APPLY); t = simple (M42_TOK_AT, start); break;
    case '?': t = simple (M42_TOK_QUESTION, start); break;
    case '\\': t = simple (M42_TOK_BACKSLASH, start); break;
    case '\'': t = simple (M42_TOK_QUOTE, start); break;
    default:
      t = simple (M42_TOK_ERROR, start);
      t.text = g_strdup_printf ("unexpected character '%c'", s[start]);
      break;
    }
#undef TWO
done:
  t.space_before = space;
  return t;
}
