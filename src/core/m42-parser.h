/* m42-parser.h - source text to syntax tree
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  M42_NODE_NUMBER,    /* 3.5 */
  M42_NODE_STRING,    /* "a string": the text is in name */
  M42_NODE_IDENT,     /* x, Pi */
  M42_NODE_UNARY,     /* -a, a!, !a, a' (op says which) */
  M42_NODE_BINARY,    /* a + b, a ^ b, a . b, a == b, a && b */
  M42_NODE_CALL,      /* Sin[x], sin(x): name and arguments */
  M42_NODE_LIST,      /* {a, b, c} */
  M42_NODE_MATRIX,    /* [1 2; 3 4]: children are LIST rows */
  M42_NODE_RANGE,     /* a:b or a:step:b -- children (a, b[, step]) */
  M42_NODE_PART,      /* x[[i, j]]: children (target, i, j...) */
  M42_NODE_APPLYFN,   /* (expr)[args]: children (function, argument...) */
  M42_NODE_ASSIGN,    /* x = expr: name and one child.  op is 0 for a
                         plain =, or the arithmetic token of x += 2 and
                         its kind; number is 1 when the answer wanted is
                         what the name held before, as x++ gives */
  M42_NODE_FUNCDEF,   /* f[x_, y_] := body: name, IDENT params..., body */
  M42_NODE_DEFINE,    /* f[0] = 1, f[x_Integer] := x, v[[2]] = 9: the left
                         side as it was written, and the right side */
  M42_NODE_LAMBDA,    /* @(x, y) body: IDENT params..., body */
  M42_NODE_RULE,      /* x -> 2, and x :> 2 when op is 1 */
  M42_NODE_PATTERN,   /* x_, _, _Integer, x__: the name is the one bound,
                         op says how many things it stands for, and the
                         head it is held to, if any, is the one child */
  M42_NODE_CONDITION, /* pattern /; test: children (pattern, test) */
  M42_NODE_REPLACE,   /* expr /. rules -- op is 1 for //., which goes on
                         until nothing changes: children (expr, rule...) */
  M42_NODE_SEQ,       /* a; b; c -- op is 1 when a trailing ';' hides the result */
  M42_NODE_LAST,      /* %, the previous output */
} M42NodeKind;

typedef struct _M42Node M42Node;

struct _M42Node {
  M42NodeKind kind;
  int         op;         /* an M42TokenKind for UNARY/BINARY; a flag for SEQ */
  double      number;
  char       *name;       /* IDENT, CALL (function), ASSIGN/FUNCDEF (target) */
  GPtrArray  *children;   /* of M42Node*: operands, arguments, items */
};

/* Parses one line: an expression, an assignment, or several separated
 * by semicolons.  On failure returns NULL and sets *error to a message
 * the user can read. */
M42Node *m42_parse (const char *src, char **error);

/* TRUE when there is nothing in the text but whitespace and comments,
 * which is a line to pass over rather than to complain about. */
gboolean m42_source_is_blank (const char *src);

M42Node *m42_node_new (M42NodeKind kind);
M42Node *m42_node_number (double x);
M42Node *m42_node_ident (const char *name);
M42Node *m42_node_unary (int op, M42Node *a);
M42Node *m42_node_binary (int op, M42Node *a, M42Node *b);
M42Node *m42_node_call1 (const char *name, M42Node *a);
M42Node *m42_node_copy (const M42Node *node);
void     m42_node_free (M42Node *node);

/* Shorthand for children[i]. */
static inline M42Node *
m42_node_child (const M42Node *n, guint i)
{
  return g_ptr_array_index (n->children, i);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (M42Node, m42_node_free)

G_END_DECLS
