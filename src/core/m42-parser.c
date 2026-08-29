/* m42-parser.c - a recursive-descent parser for the math42 language
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The grammar, from loosest to tightest binding:
 *
 *   program := stmt (';' stmt)* [';']
 *   stmt    := IDENT ('=' | ':=') stmt
 *            | IDENT '[' params ']' ':=' stmt          f[x_] := ...
 *            | IDENT '(' params ')' '=' stmt           f(x) = ...
 *            | replace
 *   postfix := replace ('//' replace)*
 *   replace := rule ('/.' rule)*
 *   rule    := or ('->' or)?
 *   or      := and ('||' and)*
 *   and     := not ('&&' not)*
 *   not     := '!' not | cmp
 *   cmp     := range (('==' | '!=' | '<' | '<=' | '>' | '>=') range)?
 *   range   := sum (':' sum (':' sum)?)?
 *   sum     := product (('+' | '-') product)*
 *   product := apply (('*' | '/' | '%' | '.' | '\\') apply | juxtaposed)*
 *   apply   := unary (('/@' | '@@') unary)*
 *   prefix  := IDENT '@' prefix | power
 *   unary   := '-' unary | '+' unary | power
 *   power   := postfix ('^' unary)?           -- right associative
 *   postfix := primary ('!' | '\'')*
 *   primary := NUMBER | '%' | IDENT | IDENT '[' args ']' | IDENT '(' args ')'
 *            | IDENT '[[' args ']]' | '(' stmt ')' | '{' args '}'
 *            | '[' rows ']' | '@' '(' params ')' stmt
 *
 * Both Mathematica's Sin[x] and MATLAB's sin(x) call a function; a
 * number followed by an identifier or a parenthesis multiplies, as in
 * 2x or 2(x + 1) -- except inside [ ] where MATLAB separates elements
 * with spaces.
 */

#include "m42-parser.h"
#include "m42-lexer.h"
#include "m42-help.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  M42Lexer  lexer;
  M42Token  tok;      /* one token of lookahead */
  char     *error;
  int       in_matrix;   /* > 0 inside [ ]: whitespace separates */
} Parser;

static M42Node *parse_stmt (Parser *p);
static M42Node *parse_unary (Parser *p);
static M42Node *parse_sum (Parser *p);
static M42Node *parse_replace (Parser *p);
static M42Node *parse_function_and_postfix (Parser *p, M42Node *a);

/* --- nodes ----------------------------------------------------------- */

M42Node *
m42_node_new (M42NodeKind kind)
{
  M42Node *n = g_new0 (M42Node, 1);
  n->kind = kind;
  n->children = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_node_free);
  return n;
}

M42Node *
m42_node_number (double x)
{
  M42Node *n = m42_node_new (M42_NODE_NUMBER);
  n->number = x;
  return n;
}

M42Node *
m42_node_ident (const char *name)
{
  M42Node *n = m42_node_new (M42_NODE_IDENT);
  n->name = g_strdup (name);
  return n;
}

M42Node *
m42_node_binary (int op, M42Node *a, M42Node *b)
{
  M42Node *n = m42_node_new (M42_NODE_BINARY);
  n->op = op;
  g_ptr_array_add (n->children, a);
  g_ptr_array_add (n->children, b);
  return n;
}

M42Node *
m42_node_unary (int op, M42Node *a)
{
  M42Node *n = m42_node_new (M42_NODE_UNARY);
  n->op = op;
  g_ptr_array_add (n->children, a);
  return n;
}

M42Node *
m42_node_call1 (const char *name, M42Node *a)
{
  M42Node *n = m42_node_new (M42_NODE_CALL);
  n->name = g_strdup (name);
  g_ptr_array_add (n->children, a);
  return n;
}

M42Node *
m42_node_copy (const M42Node *node)
{
  M42Node *n;

  if (node == NULL)
    return NULL;
  n = m42_node_new (node->kind);
  n->op = node->op;
  n->number = node->number;
  n->name = g_strdup (node->name);
  for (guint i = 0; i < node->children->len; i++)
    g_ptr_array_add (n->children, m42_node_copy (m42_node_child (node, i)));
  return n;
}

void
m42_node_free (M42Node *node)
{
  if (node == NULL)
    return;
  g_free (node->name);
  g_ptr_array_unref (node->children);
  g_free (node);
}

/* --- the parser ------------------------------------------------------ */

static void
advance (Parser *p)
{
  m42_token_clear (&p->tok);
  p->tok = m42_lexer_next (&p->lexer);
  if (p->tok.kind == M42_TOK_ERROR && p->error == NULL)
    p->error = g_strdup_printf ("%s at position %d", p->tok.text, p->tok.offset + 1);
}

static void
fail (Parser *p, const char *what)
{
  if (p->error == NULL)
    p->error = g_strdup_printf ("%s at position %d", what, p->tok.offset + 1);
}

static gboolean
expect (Parser *p, M42TokenKind kind, const char *what)
{
  if (p->tok.kind != kind)
    {
      fail (p, what);
      return FALSE;
    }
  advance (p);
  return TRUE;
}

/* Peeks at the token after the current one. */
static M42TokenKind
peek (Parser *p)
{
  M42Lexer lx = p->lexer;
  M42Token next = m42_lexer_next (&lx);
  M42TokenKind kind = next.kind;
  m42_token_clear (&next);
  return kind;
}

/* At an f[[ ... : does the inner bracket hold a matrix rather than a
 * list of indices?  A semicolon says so, and so do two values with
 * nothing but space between them. */
static gboolean
looks_like_matrix (Parser *p)
{
  M42Lexer lx = p->lexer;
  M42Token t = m42_lexer_next (&lx);      /* the second '[' */
  int depth = 2;
  gboolean prev_value = FALSE, matrix = FALSE;

  m42_token_clear (&t);
  for (;;)
    {
      t = m42_lexer_next (&lx);
      if (t.kind == M42_TOK_END)
        break;
      if (t.kind == M42_TOK_SEMI)
        matrix = TRUE;
      if (t.kind == M42_TOK_LBRACKET)
        depth++;
      if (t.kind == M42_TOK_RBRACKET && --depth == 0)
        {
          m42_token_clear (&t);
          break;
        }
      if (depth == 2)
        {
          gboolean is_value = t.kind == M42_TOK_NUMBER || t.kind == M42_TOK_IDENT;
          if (is_value && prev_value && t.space_before)
            matrix = TRUE;
          prev_value = is_value;
        }
      m42_token_clear (&t);
      if (matrix)
        break;
    }
  return matrix;
}

/* Comma-separated expressions up to close, appended to into->children. */
/* One argument, or several statements with semicolons between them:
 * If[c, a; b] and (a; b) both hold a little program.  A semicolon
 * inside a matrix is a new row and never gets here, since a matrix is
 * read by parse_matrix and not by this. */
static M42Node *
parse_block (Parser *p)
{
  M42Node *first = parse_stmt (p);
  M42Node *seq;

  if (first == NULL || p->tok.kind != M42_TOK_SEMI)
    return first;

  seq = m42_node_new (M42_NODE_SEQ);
  g_ptr_array_add (seq->children, first);
  while (p->tok.kind == M42_TOK_SEMI)
    {
      M42Node *next;

      advance (p);
      seq->op = 1;                 /* a trailing ; hides the answer */
      if (p->tok.kind == M42_TOK_COMMA || p->tok.kind == M42_TOK_RPAREN ||
          p->tok.kind == M42_TOK_RBRACKET || p->tok.kind == M42_TOK_RBRACE ||
          p->tok.kind == M42_TOK_END)
        break;
      next = parse_stmt (p);
      if (next == NULL)
        {
          m42_node_free (seq);
          return NULL;
        }
      g_ptr_array_add (seq->children, next);
      seq->op = 0;
    }
  return seq;
}

static gboolean
parse_args (Parser *p, M42Node *into, M42TokenKind close, const char *what)
{
  if (p->tok.kind == close)
    {
      advance (p);
      return TRUE;
    }
  for (;;)
    {
      M42Node *arg;

      /* A colon on its own is every index at that level: A(2, :). */
      if (p->tok.kind == M42_TOK_COLON)
        {
          M42TokenKind after = peek (p);

          if (after == M42_TOK_COMMA || after == close)
            {
              g_ptr_array_add (into->children, m42_node_ident ("All"));
              advance (p);
              if (p->tok.kind == M42_TOK_COMMA)
                {
                  advance (p);
                  continue;
                }
              return expect (p, close, what);
            }
        }

      arg = parse_block (p);
      if (arg == NULL)
        return FALSE;
      g_ptr_array_add (into->children, arg);
      if (p->tok.kind == M42_TOK_COMMA)
        {
          advance (p);
          continue;
        }
      return expect (p, close, what);
    }
}

static gboolean
starts_primary (Parser *p)
{
  switch (p->tok.kind)
    {
    case M42_TOK_NUMBER: case M42_TOK_IDENT: case M42_TOK_LPAREN: case M42_TOK_STRING:
    case M42_TOK_LBRACE: case M42_TOK_LBRACKET: case M42_TOK_AT:
    case M42_TOK_PERCENT: case M42_TOK_SLOT:
      return TRUE;
    default:
      return FALSE;
    }
}

/* [1 2 3; 4 5 6]: rows of elements separated by commas or spaces. */
static M42Node *
parse_matrix (Parser *p)
{
  M42Node *m = m42_node_new (M42_NODE_MATRIX);
  M42Node *row = m42_node_new (M42_NODE_LIST);

  p->in_matrix++;
  advance (p);   /* [ */
  while (p->tok.kind != M42_TOK_RBRACKET)
    {
      M42Node *el;

      if (p->tok.kind == M42_TOK_SEMI)
        {
          advance (p);
          g_ptr_array_add (m->children, row);
          row = m42_node_new (M42_NODE_LIST);
          continue;
        }
      if (p->tok.kind == M42_TOK_COMMA)
        {
          advance (p);
          continue;
        }
      if (p->tok.kind == M42_TOK_END)
        {
          fail (p, "expected ']'");
          break;
        }
      el = parse_stmt (p);
      if (el == NULL)
        break;
      g_ptr_array_add (row->children, el);
    }
  p->in_matrix--;
  if (p->error != NULL)
    {
      m42_node_free (row);
      m42_node_free (m);
      return NULL;
    }
  advance (p);   /* ] */
  g_ptr_array_add (m->children, row);
  return m;
}

/* --- holes in a shape -----------------------------------------------------
 *
 * x_ stands for anything and calls it x; _Integer stands for any whole
 * number; x__ takes one thing or more, x___ nothing or more.  A name
 * with an underscore in the middle of it is a pattern only when what
 * follows the underscore looks like a head, which is to say it begins
 * with a capital -- so my_var stays the ordinary MATLAB name it looks
 * like, while x_Integer is the pattern Mathematica means by it.
 */
static M42Node *
pattern_node (const char *text)
{
  const char *underscore = strchr (text, '_');
  size_t before, bars = 0;
  const char *head;
  M42Node *n;

  if (underscore == NULL)
    return NULL;
  before = (size_t) (underscore - text);
  while (underscore[bars] == '_')
    bars++;
  head = underscore + bars;
  if (bars > 3 || strchr (head, '_') != NULL)
    return NULL;
  /* A head has to look like one, or this is just a name with an
   * underscore in it. */
  if (before > 0 && head[0] != 0 && !g_ascii_isupper (head[0]))
    return NULL;

  n = m42_node_new (M42_NODE_PATTERN);
  n->op = (int) bars - 1;
  if (before > 0)
    n->name = g_strndup (text, before);
  if (head[0] != 0)
    g_ptr_array_add (n->children, m42_node_ident (head));
  return n;
}

/* A parameter is plain when it is a name, or a name with one bare hole
 * after it: x and x_ are handled by making a function of x, while
 * x_Integer and x__ are shapes, and are kept for the matcher. */
static gboolean
plain_param (const char *text)
{
  const char *hole = text == NULL ? NULL : strchr (text, '_');

  if (text == NULL)
    return FALSE;
  return hole == NULL || (hole != text && hole[1] == 0);
}

/* '(' IDENT, IDENT ')' as IDENT nodes into->children. */
static gboolean
parse_params (Parser *p, M42Node *into, M42TokenKind close)
{
  while (p->tok.kind == M42_TOK_IDENT)
    {
      char *name = g_strdup (p->tok.text);
      char *hole = strchr (name, '_');

      /* f[x_] and f[x_Integer] both name their argument x. */
      if (hole != NULL && hole != name)
        *hole = 0;
      g_ptr_array_add (into->children, m42_node_ident (name));
      g_free (name);
      advance (p);
      if (p->tok.kind == M42_TOK_COMMA)
        advance (p);
    }
  return expect (p, close, "expected parameter list");
}

/* The slots of a pure function are held as names no one can type, so
 * that #^2& becomes Function[{$1}, $1^2] with nothing to collide. */
static char *
slot_name (int n)
{
  return g_strdup_printf ("$%d", n);
}

/* The highest slot used anywhere in a tree, so that #1 + #2 & knows it
 * takes two arguments. */
static int
highest_slot (const M42Node *n)
{
  int best = 0;

  if (n->kind == M42_NODE_IDENT && n->name != NULL && n->name[0] == '$')
    best = atoi (n->name + 1);
  for (guint i = 0; i < n->children->len; i++)
    best = MAX (best, highest_slot (m42_node_child (n, i)));
  return best;
}

static M42Node *
parse_primary (Parser *p)
{
  M42Node *n;

  switch (p->tok.kind)
    {
    case M42_TOK_SLOT:
      {
        g_autofree char *name = slot_name ((int) p->tok.number);
        n = m42_node_ident (name);
        advance (p);
        return n;
      }

    case M42_TOK_SPAN:
      /* ;;3 inside a part: everything up to the third. */
      {
        M42Node *span = m42_node_new (M42_NODE_CALL);
        M42Node *upper;

        span->name = g_strdup ("Span");
        advance (p);
        g_ptr_array_add (span->children, m42_node_ident ("All"));
        upper = parse_sum (p);
        if (upper == NULL)
          {
            m42_node_free (span);
            return NULL;
          }
        g_ptr_array_add (span->children, upper);
        return span;
      }

    case M42_TOK_NUMBER:
      n = m42_node_number (p->tok.number);
      /* The digits as written, when there were too many for a double. */
      if (p->tok.text != NULL)
        n->name = g_strdup (p->tok.text);
      advance (p);
      return n;

    case M42_TOK_QUESTION:
      /* ?Sin is Information["Sin"]. */
      advance (p);
      if (p->tok.kind != M42_TOK_IDENT)
        {
          fail (p, "expected a name after ?");
          return NULL;
        }
      n = m42_node_new (M42_NODE_CALL);
      n->name = g_strdup ("Information");
      {
        M42Node *arg = m42_node_new (M42_NODE_STRING);
        arg->name = g_strdup (p->tok.text);
        g_ptr_array_add (n->children, arg);
      }
      advance (p);
      return n;

    case M42_TOK_STRING:
      n = m42_node_new (M42_NODE_STRING);
      n->name = g_strdup (p->tok.text);
      advance (p);
      return n;

    case M42_TOK_PERCENT:
      advance (p);
      return m42_node_new (M42_NODE_LAST);

    case M42_TOK_IDENT:
      {
        char *name = g_strdup (p->tok.text);
        M42Node *hole;

        advance (p);

        /* x_, _Integer, x__: a shape to fill rather than a name. */
        hole = pattern_node (name);
        if (hole != NULL)
          {
            g_free (name);

            /* x_?OddQ: the shape, and then a question asked of what
             * fills it. */
            if (p->tok.kind == M42_TOK_QUESTION)
              {
                M42Token look = p->tok;
                M42Lexer after = p->lexer;
                M42Token who = m42_lexer_next (&after);

                (void) look;
                if (who.kind == M42_TOK_IDENT)
                  {
                    M42Node *asked = m42_node_new (M42_NODE_CONDITION);

                    asked->op = 1;                  /* a function, not a test */
                    g_ptr_array_add (asked->children, hole);
                    g_ptr_array_add (asked->children, m42_node_ident (who.text));
                    m42_token_clear (&who);
                    advance (p);                    /* the ? */
                    advance (p);                    /* the name */
                    return asked;
                  }
                m42_token_clear (&who);
              }
            return hole;
          }

        /* Inside [ ], "a (b)" is two elements, not a call.
         *
         * And a space before a round bracket means multiplication, as
         * it does in Mathematica: 2 x (x^2 + 1)^3 is a product of
         * three things and not x called with an argument.  Without the
         * space, x(x + 1) is still read as a call and turned into a
         * product later if x turns out not to be a function -- that
         * rule cannot help here, because the call would take the whole
         * of (x^2 + 1)^3 with it and square the product by mistake.
         * A name the reference table knows is a function whatever the
         * spacing: Sin (x) is Sin[x]. */
        if ((p->tok.kind == M42_TOK_LBRACKET ||
             (p->tok.kind == M42_TOK_LPAREN &&
              !(p->tok.space_before && m42_function_find (name) == NULL))) &&
            !(p->in_matrix > 0 && p->in_matrix < 100 && p->tok.space_before))
          {
            M42TokenKind close;
            const char *what;

            /* f[[...]] is Part of f -- unless what follows is a matrix
             * written the MATLAB way, as in Eigenvalues[[2 1; 1 2]],
             * which a semicolon or two values side by side gives away. */
            if (p->tok.kind == M42_TOK_LBRACKET && peek (p) == M42_TOK_LBRACKET &&
                !looks_like_matrix (p))
              {
                /* x[[i, j]] */
                advance (p);
                advance (p);
                n = m42_node_new (M42_NODE_PART);
                g_ptr_array_add (n->children, m42_node_ident (name));
                g_free (name);
                if (!parse_args (p, n, M42_TOK_RBRACKET, "expected ']]'") ||
                    !expect (p, M42_TOK_RBRACKET, "expected ']]'"))
                  {
                    m42_node_free (n);
                    return NULL;
                  }
                return n;
              }

            close = p->tok.kind == M42_TOK_LBRACKET ? M42_TOK_RBRACKET : M42_TOK_RPAREN;
            what = close == M42_TOK_RBRACKET ? "expected ']'" : "expected ')'";
            n = m42_node_new (M42_NODE_CALL);
            n->name = name;
            /* Round brackets are remembered: s(s + 1) is a call in
             * MATLAB and a multiplication in Mathematica, and only the
             * evaluator knows whether s is a function. */
            n->op = close == M42_TOK_RPAREN ? 1 : 0;
            advance (p);
            p->in_matrix += 100;   /* commas separate, spaces multiply */
            if (!parse_args (p, n, close, what))
              {
                p->in_matrix -= 100;
                m42_node_free (n);
                return NULL;
              }
            p->in_matrix -= 100;
            return n;
          }
        n = m42_node_ident (name);
        g_free (name);
        return n;
      }

    case M42_TOK_LPAREN:
      advance (p);
      p->in_matrix += 100;
      n = parse_block (p);
      p->in_matrix -= 100;
      if (n == NULL)
        return NULL;
      if (!expect (p, M42_TOK_RPAREN, "expected ')'"))
        {
          m42_node_free (n);
          return NULL;
        }
      return n;

    case M42_TOK_LBRACE:
      advance (p);
      n = m42_node_new (M42_NODE_LIST);
      p->in_matrix += 100;
      if (!parse_args (p, n, M42_TOK_RBRACE, "expected '}'"))
        {
          p->in_matrix -= 100;
          m42_node_free (n);
          return NULL;
        }
      p->in_matrix -= 100;
      return n;

    case M42_TOK_LBRACKET:
      {
        int saved = p->in_matrix;
        p->in_matrix = 0;
        n = parse_matrix (p);
        p->in_matrix = saved;
        return n;
      }

    case M42_TOK_AT:
      /* @(x, y) body */
      advance (p);
      n = m42_node_new (M42_NODE_LAMBDA);
      if (!expect (p, M42_TOK_LPAREN, "expected '(' after @") ||
          !parse_params (p, n, M42_TOK_RPAREN))
        {
          m42_node_free (n);
          return NULL;
        }
      {
        M42Node *body = parse_stmt (p);
        if (body == NULL)
          {
            m42_node_free (n);
            return NULL;
          }
        g_ptr_array_add (n->children, body);
      }
      return n;

    case M42_TOK_END:
      fail (p, "unexpected end of input");
      return NULL;

    default:
      fail (p, "unexpected token");
      return NULL;
    }
}

static M42Node *
parse_postfix (Parser *p)
{
  M42Node *n = parse_primary (p);

  for (;;)
    {
      if (n == NULL)
        return NULL;

      if (p->tok.kind == M42_TOK_BANG || p->tok.kind == M42_TOK_QUOTE)
        {
          int op = p->tok.kind;
          advance (p);
          n = m42_node_unary (op, n);
          continue;
        }

      /* [args] after something that is not a name: a function value
       * being called, as in (#^2 &)[4] or f[2][3]. */
      if (p->tok.kind == M42_TOK_LBRACKET && peek (p) != M42_TOK_LBRACKET &&
          !p->tok.space_before && n->kind != M42_NODE_IDENT &&
          (n->kind == M42_NODE_LAMBDA || n->kind == M42_NODE_CALL ||
           n->kind == M42_NODE_APPLYFN || n->kind == M42_NODE_PART ||
           (n->kind == M42_NODE_UNARY && n->op == M42_TOK_QUOTE)))
        {
          M42Node *call = m42_node_new (M42_NODE_APPLYFN);

          g_ptr_array_add (call->children, n);
          advance (p);
          p->in_matrix += 100;
          if (!parse_args (p, call, M42_TOK_RBRACKET, "expected ']'"))
            {
              p->in_matrix -= 100;
              m42_node_free (call);
              return NULL;
            }
          p->in_matrix -= 100;
          n = call;
          continue;
        }

      /* [[i]] after anything at all, so that NDSolve[...][[3]] takes
       * the third point rather than multiplying by a matrix. */
      if (p->tok.kind == M42_TOK_LBRACKET && peek (p) == M42_TOK_LBRACKET &&
          !looks_like_matrix (p))
        {
          M42Node *part = m42_node_new (M42_NODE_PART);

          g_ptr_array_add (part->children, n);
          advance (p);
          advance (p);
          if (!parse_args (p, part, M42_TOK_RBRACKET, "expected ']]'") ||
              !expect (p, M42_TOK_RBRACKET, "expected ']]'"))
            {
              m42_node_free (part);
              return NULL;
            }
          n = part;
          continue;
        }
      return n;
    }
}

static M42Node *
parse_power (Parser *p)
{
  M42Node *base = parse_postfix (p);

  if (base != NULL && p->tok.kind == M42_TOK_CARET)
    {
      M42Node *exp;
      advance (p);
      exp = parse_unary (p);
      if (exp == NULL)
        {
          m42_node_free (base);
          return NULL;
        }
      return m42_node_binary (M42_TOK_CARET, base, exp);
    }
  return base;
}

static M42Node *
parse_unary (Parser *p)
{
  if (p->tok.kind == M42_TOK_MINUS || p->tok.kind == M42_TOK_PLUS)
    {
      int op = p->tok.kind;
      M42Node *a;
      advance (p);
      a = parse_unary (p);
      if (a == NULL)
        return NULL;
      return op == M42_TOK_MINUS ? m42_node_unary (op, a) : a;
    }
  return parse_power (p);
}

/* f @ x is f[x], and it leans to the right: f @ g @ x is f[g[x]]. */
static M42Node *
parse_prefix_apply (Parser *p, M42Node *head)
{
  M42Node *arg, *call;

  advance (p);   /* @ */
  arg = parse_unary (p);
  if (arg == NULL)
    {
      m42_node_free (head);
      return NULL;
    }
  if (head->kind != M42_NODE_IDENT)
    {
      fail (p, "@ wants the name of a function on its left");
      m42_node_free (head);
      m42_node_free (arg);
      return NULL;
    }
  call = m42_node_new (M42_NODE_CALL);
  call->name = g_strdup (head->name);
  g_ptr_array_add (call->children, arg);
  m42_node_free (head);
  return call;
}

/* f /@ list maps, f @@ list applies, and f @ x calls. */
static M42Node *
parse_apply (Parser *p)
{
  M42Node *a = parse_unary (p);

  while (a != NULL)
    {
      M42Node *b, *call;
      const char *fname;

      if (p->tok.kind == M42_TOK_AT)
        {
          a = parse_prefix_apply (p, a);
          continue;
        }
      if (p->tok.kind != M42_TOK_MAP && p->tok.kind != M42_TOK_APPLY)
        break;

      fname = p->tok.kind == M42_TOK_MAP ? "Map" : "Apply";
      advance (p);
      b = parse_unary (p);
      if (b == NULL)
        {
          m42_node_free (a);
          return NULL;
        }
      call = m42_node_new (M42_NODE_CALL);
      call->name = g_strdup (fname);
      g_ptr_array_add (call->children, a);
      g_ptr_array_add (call->children, b);
      a = call;
    }
  return a;
}

static M42Node *
parse_product (Parser *p)
{
  M42Node *a = parse_apply (p);

  while (a != NULL)
    {
      int op;
      M42Node *b;

      if (p->tok.kind == M42_TOK_STAR || p->tok.kind == M42_TOK_SLASH ||
          p->tok.kind == M42_TOK_DOT || p->tok.kind == M42_TOK_BACKSLASH ||
          (p->tok.kind == M42_TOK_PERCENT && peek (p) != M42_TOK_END &&
           p->in_matrix == 0))
        {
          op = p->tok.kind;
          advance (p);
        }
      else if (starts_primary (p) && p->tok.kind != M42_TOK_PERCENT &&
               !(p->in_matrix > 0 && p->in_matrix < 100 && p->tok.space_before))
        /* 2x, 2(x+1), x y -- and a number on the right as well, so
         * that (x^2 + 1)^3 2x means what it says.  Inside a matrix
         * written the MATLAB way a space still separates one element
         * from the next, which the guard above keeps. */
        op = M42_TOK_STAR;
      else
        break;

      b = parse_apply (p);
      if (b == NULL)
        {
          m42_node_free (a);
          return NULL;
        }
      a = m42_node_binary (op, a, b);
    }
  return a;
}

static M42Node *
parse_sum (Parser *p)
{
  M42Node *a = parse_product (p);

  while (a != NULL && (p->tok.kind == M42_TOK_PLUS || p->tok.kind == M42_TOK_MINUS))
    {
      int op = p->tok.kind;
      M42Node *b;

      /* [1 -2] is two elements; [1 - 2] and [1-2] are one. */
      if (p->in_matrix > 0 && p->in_matrix < 100 && p->tok.space_before)
        {
          M42Lexer lx = p->lexer;
          M42Token next = m42_lexer_next (&lx);
          gboolean tight = !next.space_before;
          m42_token_clear (&next);
          if (tight)
            break;
        }
      advance (p);
      b = parse_product (p);
      if (b == NULL)
        {
          m42_node_free (a);
          return NULL;
        }
      a = m42_node_binary (op, a, b);
    }
  return a;
}

/* a ;; b, the span of a part.  An end that is left out is All, which
 * the part works out against the length of what it is indexing. */
static M42Node *
parse_span (Parser *p)
{
  M42Node *a = parse_sum (p);
  M42Node *span, *upper;

  if (a == NULL || p->tok.kind != M42_TOK_SPAN)
    return a;

  span = m42_node_new (M42_NODE_CALL);
  span->name = g_strdup ("Span");
  g_ptr_array_add (span->children, a);
  advance (p);
  if (p->tok.kind == M42_TOK_RBRACKET || p->tok.kind == M42_TOK_COMMA ||
      p->tok.kind == M42_TOK_RPAREN || p->tok.kind == M42_TOK_END)
    upper = m42_node_ident ("All");
  else
    upper = parse_sum (p);
  if (upper == NULL)
    {
      m42_node_free (span);
      return NULL;
    }
  g_ptr_array_add (span->children, upper);
  return span;
}

static M42Node *
parse_range (Parser *p)
{
  M42Node *a = parse_span (p);

  if (a != NULL && p->tok.kind == M42_TOK_COLON)
    {
      M42Node *n = m42_node_new (M42_NODE_RANGE);
      M42Node *b;

      g_ptr_array_add (n->children, a);
      advance (p);
      b = parse_span (p);
      if (b == NULL)
        {
          m42_node_free (n);
          return NULL;
        }
      g_ptr_array_add (n->children, b);
      if (p->tok.kind == M42_TOK_COLON)
        {
          /* a:step:b -- MATLAB puts the step in the middle. */
          M42Node *c;
          advance (p);
          c = parse_span (p);
          if (c == NULL)
            {
              m42_node_free (n);
              return NULL;
            }
          g_ptr_array_add (n->children, c);
        }
      return n;
    }
  return a;
}

static M42Node *
parse_cmp (Parser *p)
{
  M42Node *a = parse_range (p);

  if (a != NULL)
    switch (p->tok.kind)
      {
      case M42_TOK_EQ: case M42_TOK_NE: case M42_TOK_LT:
      case M42_TOK_LE: case M42_TOK_GT: case M42_TOK_GE:
        {
          int op = p->tok.kind;
          M42Node *b;
          advance (p);
          b = parse_range (p);
          if (b == NULL)
            {
              m42_node_free (a);
              return NULL;
            }
          return m42_node_binary (op, a, b);
        }
      default:
        break;
      }
  return a;
}

static M42Node *
parse_not (Parser *p)
{
  if (p->tok.kind == M42_TOK_BANG || p->tok.kind == M42_TOK_NOT)
    {
      M42Node *a;
      advance (p);
      a = parse_not (p);
      return a != NULL ? m42_node_unary (M42_TOK_NOT, a) : NULL;
    }
  return parse_cmp (p);
}

static M42Node *
parse_and (Parser *p)
{
  M42Node *a = parse_not (p);

  while (a != NULL && p->tok.kind == M42_TOK_AND)
    {
      M42Node *b;
      advance (p);
      b = parse_not (p);
      if (b == NULL)
        {
          m42_node_free (a);
          return NULL;
        }
      a = m42_node_binary (M42_TOK_AND, a, b);
    }
  return a;
}

static M42Node *
parse_or (Parser *p)
{
  M42Node *a = parse_and (p);

  while (a != NULL && p->tok.kind == M42_TOK_OR)
    {
      M42Node *b;
      advance (p);
      b = parse_and (p);
      if (b == NULL)
        {
          m42_node_free (a);
          return NULL;
        }
      a = m42_node_binary (M42_TOK_OR, a, b);
    }
  return a;
}

/* pattern /; test -- the test a match still has to pass.  It is read
 * more tightly than -> is, so that x_ /; x > 0 -> x^2 puts the test on
 * the pattern, which is where it is wanted. */
static M42Node *
parse_condition (Parser *p)
{
  M42Node *a = parse_or (p);

  if (a != NULL && p->tok.kind == M42_TOK_CONDITION)
    {
      M42Node *n = m42_node_new (M42_NODE_CONDITION);
      M42Node *test;

      g_ptr_array_add (n->children, a);
      advance (p);
      test = parse_or (p);
      if (test == NULL)
        {
          m42_node_free (n);
          return NULL;
        }
      g_ptr_array_add (n->children, test);
      return n;
    }
  return a;
}

static M42Node *
parse_rule (Parser *p)
{
  M42Node *a = parse_condition (p);

  if (a != NULL && (p->tok.kind == M42_TOK_ARROW || p->tok.kind == M42_TOK_RULEDELAYED))
    {
      M42Node *n = m42_node_new (M42_NODE_RULE);
      M42Node *b;

      n->op = p->tok.kind == M42_TOK_RULEDELAYED;
      g_ptr_array_add (n->children, a);
      advance (p);
      b = parse_condition (p);
      if (b == NULL)
        {
          m42_node_free (n);
          return NULL;
        }
      g_ptr_array_add (n->children, b);
      return n;
    }
  return a;
}

static M42Node *
parse_replace (Parser *p)
{
  M42Node *a = parse_rule (p);

  if (a != NULL && (p->tok.kind == M42_TOK_REPLACE || p->tok.kind == M42_TOK_REPLACEREP))
    {
      M42Node *n = m42_node_new (M42_NODE_REPLACE);

      n->op = p->tok.kind == M42_TOK_REPLACEREP;
      g_ptr_array_add (n->children, a);
      while (p->tok.kind == M42_TOK_REPLACE || p->tok.kind == M42_TOK_REPLACEREP)
        {
          M42Node *r;
          advance (p);
          r = parse_rule (p);
          if (r == NULL)
            {
              m42_node_free (n);
              return NULL;
            }
          g_ptr_array_add (n->children, r);
        }
      return n;
    }
  return a;
}

/* --- MATLAB's blocks ------------------------------------------------------
 *
 * for i = 1:3, body; end -- and while and if, with elseif and else --
 * written the way a MATLAB script writes them.  They are read into the
 * same For, While and If the rest of math42 already knows, so nothing
 * below the parser has to learn a second shape.  A one-line block wants
 * a comma or a semicolon after its header, as MATLAB does.
 */

static gboolean
keyword_is (Parser *p, const char *word)
{
  return p->tok.kind == M42_TOK_IDENT && p->tok.text != NULL &&
         strcmp (p->tok.text, word) == 0;
}

static gboolean
closes_block (Parser *p)
{
  return p->tok.kind == M42_TOK_END || keyword_is (p, "end") ||
         keyword_is (p, "endfor") || keyword_is (p, "endwhile") ||
         keyword_is (p, "endif") || keyword_is (p, "else") || keyword_is (p, "elseif");
}

/* The statements of a block, up to whatever closes it. */
static M42Node *
parse_block_body (Parser *p)
{
  M42Node *seq = m42_node_new (M42_NODE_SEQ);

  for (;;)
    {
      M42Node *one;

      while (p->tok.kind == M42_TOK_SEMI || p->tok.kind == M42_TOK_COMMA)
        {
          seq->op = 1;              /* a trailing separator hides the answer */
          advance (p);
        }
      if (closes_block (p) || p->error != NULL)
        break;

      one = parse_stmt (p);
      if (one == NULL)
        {
          m42_node_free (seq);
          return NULL;
        }
      g_ptr_array_add (seq->children, one);
      seq->op = 0;
    }
  return seq;
}

/* A comma or semicolon between a block's header and its body. */
static gboolean
expect_block_separator (Parser *p, const char *which)
{
  if (p->tok.kind == M42_TOK_SEMI || p->tok.kind == M42_TOK_COMMA)
    {
      advance (p);
      return TRUE;
    }
  if (closes_block (p))
    return TRUE;
  {
    g_autofree char *what = g_strdup_printf ("%s wants a comma before its body", which);
    fail (p, what);
  }
  return FALSE;
}

static gboolean
expect_end (Parser *p)
{
  if (!closes_block (p) || keyword_is (p, "else") || keyword_is (p, "elseif"))
    {
      fail (p, "expected end");
      return FALSE;
    }
  advance (p);
  return TRUE;
}

static M42Node *
call_of (const char *name, M42Node *a, M42Node *b, M42Node *c)
{
  M42Node *n = m42_node_new (M42_NODE_CALL);

  n->name = g_strdup (name);
  if (a != NULL) g_ptr_array_add (n->children, a);
  if (b != NULL) g_ptr_array_add (n->children, b);
  if (c != NULL) g_ptr_array_add (n->children, c);
  return n;
}

/* if cond, body; elseif cond, body; else body; end */
static M42Node *
parse_matlab_if (Parser *p)
{
  M42Node *cond, *body, *otherwise = NULL;

  advance (p);                     /* if, or elseif */
  cond = parse_stmt (p);
  if (cond == NULL || !expect_block_separator (p, "if"))
    {
      m42_node_free (cond);
      return NULL;
    }
  body = parse_block_body (p);
  if (body == NULL)
    {
      m42_node_free (cond);
      return NULL;
    }

  if (keyword_is (p, "elseif"))
    {
      otherwise = parse_matlab_if (p);      /* which eats the end for us */
      if (otherwise == NULL)
        {
          m42_node_free (cond);
          m42_node_free (body);
          return NULL;
        }
      return call_of ("If", cond, body, otherwise);
    }

  if (keyword_is (p, "else"))
    {
      advance (p);
      if (p->tok.kind == M42_TOK_SEMI || p->tok.kind == M42_TOK_COMMA)
        advance (p);
      otherwise = parse_block_body (p);
      if (otherwise == NULL)
        {
          m42_node_free (cond);
          m42_node_free (body);
          return NULL;
        }
    }

  if (!expect_end (p))
    {
      m42_node_free (cond);
      m42_node_free (body);
      m42_node_free (otherwise);
      return NULL;
    }
  return call_of ("If", cond, body, otherwise);
}

/* for i = list, body; end -- Do over the list, with a single value
 * wrapped so that for i = 5 runs once, as MATLAB has it. */
static M42Node *
parse_matlab_for (Parser *p)
{
  g_autofree char *var = NULL;
  M42Node *over, *body, *iterator, *wrapped;

  advance (p);                     /* for */
  var = g_strdup (p->tok.text);
  advance (p);                     /* the name */
  advance (p);                     /* = */

  over = parse_stmt (p);
  if (over == NULL || !expect_block_separator (p, "for"))
    {
      m42_node_free (over);
      return NULL;
    }
  body = parse_block_body (p);
  if (body == NULL || !expect_end (p))
    {
      m42_node_free (over);
      m42_node_free (body);
      return NULL;
    }

  {
    M42Node *one = m42_node_new (M42_NODE_LIST);
    g_ptr_array_add (one->children, over);
    wrapped = call_of ("Flatten", one, NULL, NULL);
  }
  iterator = m42_node_new (M42_NODE_LIST);
  g_ptr_array_add (iterator->children, m42_node_ident (var));
  g_ptr_array_add (iterator->children, wrapped);
  return call_of ("Do", body, iterator, NULL);
}

static M42Node *
parse_matlab_while (Parser *p)
{
  M42Node *cond, *body;

  advance (p);                     /* while */
  cond = parse_stmt (p);
  if (cond == NULL || !expect_block_separator (p, "while"))
    {
      m42_node_free (cond);
      return NULL;
    }
  body = parse_block_body (p);
  if (body == NULL || !expect_end (p))
    {
      m42_node_free (cond);
      m42_node_free (body);
      return NULL;
    }
  return call_of ("While", cond, body, NULL);
}

/* {a, b} = {1, 2} and MATLAB's [q, r] = ... : a row of names on the
 * left of one =, which hands out what the right side is made of.  It
 * is told apart from an ordinary list by looking ahead for the bracket
 * that closes it and seeing what follows. */
static gboolean
looks_like_unpacking (Parser *p)
{
  M42Lexer lx = p->lexer;
  M42TokenKind open = p->tok.kind;
  M42TokenKind close = open == M42_TOK_LBRACE ? M42_TOK_RBRACE : M42_TOK_RBRACKET;
  int depth = 1;
  gboolean names_only = TRUE, assigns = FALSE;

  for (;;)
    {
      M42Token t = m42_lexer_next (&lx);
      M42TokenKind kind = t.kind;

      m42_token_clear (&t);
      if (kind == M42_TOK_END)
        break;
      if (kind == open)
        depth++;
      else if (kind == close && --depth == 0)
        {
          M42Token after = m42_lexer_next (&lx);

          assigns = after.kind == M42_TOK_ASSIGN;
          m42_token_clear (&after);
          break;
        }
      if (kind != M42_TOK_IDENT && kind != M42_TOK_COMMA)
        names_only = FALSE;
    }
  return names_only && assigns;
}

static M42Node *
parse_stmt (Parser *p)
{
  /* A row of names being handed the pieces of what is on the right. */
  if ((p->tok.kind == M42_TOK_LBRACE || p->tok.kind == M42_TOK_LBRACKET) &&
      looks_like_unpacking (p))
    {
      M42Node *def = m42_node_new (M42_NODE_DEFINE);
      M42Node *names = parse_replace (p);
      M42Node *body;

      if (names == NULL)
        {
          m42_node_free (def);
          return NULL;
        }
      g_ptr_array_add (def->children, names);
      if (!expect (p, M42_TOK_ASSIGN, "expected = after the names"))
        {
          m42_node_free (def);
          return NULL;
        }
      body = parse_stmt (p);
      if (body == NULL)
        {
          m42_node_free (def);
          return NULL;
        }
      g_ptr_array_add (def->children, body);
      return def;
    }

  /* MATLAB's block words, when they are used as such: for and while
   * followed by something, if followed by a condition.  A name that
   * happens to be one of these words is left alone. */
  if (p->tok.kind == M42_TOK_IDENT && p->tok.text != NULL)
    {
      if (strcmp (p->tok.text, "for") == 0)
        {
          M42Lexer lx = p->lexer;
          M42Token name_tok = m42_lexer_next (&lx);
          M42Token eq = m42_lexer_next (&lx);
          gboolean is_block = name_tok.kind == M42_TOK_IDENT && eq.kind == M42_TOK_ASSIGN;

          m42_token_clear (&name_tok);
          m42_token_clear (&eq);
          if (is_block)
            return parse_matlab_for (p);
        }
      else if (strcmp (p->tok.text, "while") == 0 || strcmp (p->tok.text, "if") == 0)
        {
          M42Lexer lx = p->lexer;
          M42Token next = m42_lexer_next (&lx);
          gboolean is_block = next.kind != M42_TOK_LBRACKET && next.kind != M42_TOK_LPAREN &&
                              next.kind != M42_TOK_ASSIGN && next.kind != M42_TOK_END;

          m42_token_clear (&next);
          if (is_block)
            return p->tok.text[0] == 'w' ? parse_matlab_while (p) : parse_matlab_if (p);
        }
    }

  {
  if (p->tok.kind == M42_TOK_IDENT)
    {
      M42TokenKind next = peek (p);

      /* x++, x--, x += 2 and the rest: one name, one operator, and
       * what it works on. */
      if (next == M42_TOK_INCREMENT || next == M42_TOK_DECREMENT ||
          next == M42_TOK_PLUSEQ || next == M42_TOK_MINUSEQ ||
          next == M42_TOK_STAREQ || next == M42_TOK_SLASHEQ)
        {
          M42Node *n = m42_node_new (M42_NODE_ASSIGN);
          gboolean by_one = next == M42_TOK_INCREMENT || next == M42_TOK_DECREMENT;

          n->name = g_strdup (p->tok.text);
          n->op = (next == M42_TOK_INCREMENT || next == M42_TOK_PLUSEQ) ? M42_TOK_PLUS
                : (next == M42_TOK_DECREMENT || next == M42_TOK_MINUSEQ) ? M42_TOK_MINUS
                : next == M42_TOK_STAREQ ? M42_TOK_STAR : M42_TOK_SLASH;
          n->number = by_one;      /* x++ answers with what x was */
          advance (p);
          advance (p);
          if (by_one)
            g_ptr_array_add (n->children, m42_node_number (1));
          else
            {
              M42Node *by = parse_stmt (p);

              if (by == NULL)
                {
                  m42_node_free (n);
                  return NULL;
                }
              g_ptr_array_add (n->children, by);
            }
          return n;
        }

      /* x = ..., x := ... */
      if (next == M42_TOK_ASSIGN || next == M42_TOK_SETDELAYED)
        {
          M42Node *n = m42_node_new (M42_NODE_ASSIGN);
          M42Node *value;

          n->name = g_strdup (p->tok.text);
          advance (p);
          advance (p);
          value = parse_stmt (p);
          if (value == NULL)
            {
              m42_node_free (n);
              return NULL;
            }
          g_ptr_array_add (n->children, value);
          return n;
        }

      /* f[x_] := ..., f(x) = ...: a definition when the argument list is
       * names only and a definition operator follows. */
      if (next == M42_TOK_LBRACKET || next == M42_TOK_LPAREN)
        {
          M42Lexer lx = p->lexer;
          M42Token t = m42_lexer_next (&lx);   /* the bracket */
          M42TokenKind open = t.kind;
          M42TokenKind close = open == M42_TOK_LBRACKET ? M42_TOK_RBRACKET : M42_TOK_RPAREN;
          gboolean names_only = TRUE, is_def = FALSE;
          int depth = 1;

          /* The whole of the left side is looked over, brackets within
           * brackets included, to see whether a definition follows it
           * and whether it is a plain row of names. */
          m42_token_clear (&t);
          for (;;)
            {
              t = m42_lexer_next (&lx);
              if (t.kind == M42_TOK_END)
                {
                  m42_token_clear (&t);
                  break;
                }
              if (t.kind == open)
                depth++;
              else if (t.kind == close && --depth == 0)
                {
                  m42_token_clear (&t);
                  t = m42_lexer_next (&lx);
                  is_def = t.kind == M42_TOK_SETDELAYED || t.kind == M42_TOK_ASSIGN;
                  m42_token_clear (&t);
                  break;
                }
              if (depth > 1 || (t.kind != M42_TOK_IDENT && t.kind != M42_TOK_COMMA) ||
                  (t.kind == M42_TOK_IDENT && !plain_param (t.text)))
                names_only = FALSE;
              m42_token_clear (&t);
            }

          if (names_only && is_def)
            {
              M42Node *n = m42_node_new (M42_NODE_FUNCDEF);
              M42Node *body;

              n->name = g_strdup (p->tok.text);
              advance (p);
              advance (p);
              if (!parse_params (p, n, close))
                {
                  m42_node_free (n);
                  return NULL;
                }
              advance (p);   /* := or = */
              body = parse_stmt (p);
              if (body == NULL)
                {
                  m42_node_free (n);
                  return NULL;
                }
              g_ptr_array_add (n->children, body);
              return n;
            }

          /* f[0] = 1, f[x_Integer] := x^2, v[[2]] = 9: the left side is
           * more than a row of names, so it is kept as it was written
           * and made sense of when it is used. */
          if (is_def)
            {
              M42Node *def = m42_node_new (M42_NODE_DEFINE);
              M42Node *lhs = parse_rule (p);
              M42Node *body;

              if (lhs == NULL)
                {
                  m42_node_free (def);
                  return NULL;
                }
              g_ptr_array_add (def->children, lhs);
              if (p->tok.kind != M42_TOK_ASSIGN && p->tok.kind != M42_TOK_SETDELAYED)
                {
                  m42_node_free (def);
                  if (p->error == NULL)
                    p->error = g_strdup ("expected = or := in a definition");
                  return NULL;
                }
              advance (p);
              body = parse_stmt (p);
              if (body == NULL)
                {
                  m42_node_free (def);
                  return NULL;
                }
              g_ptr_array_add (def->children, body);
              return def;
            }
        }
    }
  }

  return parse_function_and_postfix (p, parse_replace (p));
}

/* What is left when a statement has been read: & makes what came
 * before into a pure function, and // hands it to what comes after. */
static M42Node *
parse_function_and_postfix (Parser *p, M42Node *a)
{
  while (a != NULL)
    {
      if (p->tok.kind == M42_TOK_AMP)
        {
          M42Node *fn = m42_node_new (M42_NODE_LAMBDA);
          int slots = highest_slot (a);

          advance (p);
          for (int i = 1; i <= MAX (slots, 1); i++)
            {
              g_autofree char *name = slot_name (i);
              g_ptr_array_add (fn->children, m42_node_ident (name));
            }
          g_ptr_array_add (fn->children, a);
          a = fn;
          continue;
        }
      if (p->tok.kind == M42_TOK_POSTFIX)
        {
          M42Node *f, *call;

          advance (p);
          f = parse_replace (p);
          if (f == NULL)
            {
              m42_node_free (a);
              return NULL;
            }
          if (f->kind != M42_NODE_IDENT)
            {
              fail (p, "// wants the name of a function on its right");
              m42_node_free (a);
              m42_node_free (f);
              return NULL;
            }
          call = m42_node_new (M42_NODE_CALL);
          call->name = g_strdup (f->name);
          g_ptr_array_add (call->children, a);
          m42_node_free (f);
          a = call;
          continue;
        }
      break;
    }
  return a;
}

gboolean
m42_source_is_blank (const char *src)
{
  M42Lexer lx;
  M42Token t;
  gboolean blank;

  if (src == NULL)
    return TRUE;
  m42_lexer_init (&lx, src);
  t = m42_lexer_next (&lx);
  blank = t.kind == M42_TOK_END;
  m42_token_clear (&t);
  return blank;
}

M42Node *
m42_parse (const char *src, char **error)
{
  Parser p = { 0 };
  M42Node *seq = m42_node_new (M42_NODE_SEQ);

  m42_lexer_init (&p.lexer, src);
  advance (&p);

  while (p.error == NULL)
    {
      M42Node *n;

      if (p.tok.kind == M42_TOK_END)
        break;
      n = parse_stmt (&p);
      if (n == NULL)
        break;
      g_ptr_array_add (seq->children, n);
      seq->op = 0;
      if (p.tok.kind == M42_TOK_SEMI)
        {
          advance (&p);
          seq->op = 1;
          continue;
        }
      if (p.tok.kind != M42_TOK_END)
        fail (&p, keyword_is (&p, "end") ? "an end with no block to close"
                                         : "unexpected input");
      break;
    }

  if (p.error == NULL && seq->children->len == 0)
    p.error = g_strdup ("nothing to evaluate");

  m42_token_clear (&p.tok);
  if (p.error != NULL)
    {
      m42_node_free (seq);
      *error = p.error;
      return NULL;
    }
  *error = NULL;
  return seq;
}
