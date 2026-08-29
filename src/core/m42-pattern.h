/* m42-pattern.h - matching one expression against the shape of another
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>
#include "m42-parser.h"

G_BEGIN_DECLS

/* A pattern node's op says how much it stands for. */
enum {
  M42_BLANK          = 0,   /* _   one thing */
  M42_BLANK_SEQUENCE = 1,   /* __  one thing or more */
  M42_BLANK_NULL     = 2,   /* ___ nothing, or as much as you like */
};

/* A condition node's op says which kind it is: p /; test is a test
 * written out, and p ? f is a function to hand what matched to. */
enum {
  M42_PATTERN_CONDITION = 0,
  M42_PATTERN_TEST      = 1,
};

/* Several things matched by one name are kept in a list marked this
 * way, so that they can be put back where they came from side by side
 * rather than as a list. */
#define M42_LIST_IS_SEQUENCE 1

/* What a condition after /; needs: the test, and what the names in it
 * stand for.  It lives in the evaluator, which is the only part that
 * can work out whether the test is true. */
typedef gboolean (*M42PatternTest) (const M42Node *test, GHashTable *names,
                                    gpointer user_data);

/* TRUE when there is a pattern anywhere in the tree, which is what
 * tells a rule that it has a shape to look for rather than a name. */
gboolean m42_node_has_pattern (const M42Node *n);

/* Whether the subject answers to the pattern.  Names met along the way
 * are added to names, a table of char* to M42Node* the caller owns; a
 * name met twice must stand for the same thing both times.  On failure
 * the table is left as it was found. */
gboolean m42_node_match (const M42Node *pattern, const M42Node *subject,
                         GHashTable *names, M42PatternTest test, gpointer user_data);

/* A fresh table of the kind m42_node_match wants. */
GHashTable *m42_pattern_names_new (void);

/* A copy of body with every name replaced by what it stands for. */
M42Node *m42_node_bind (const M42Node *body, GHashTable *names);

/* Whether two trees are written the same way. */
gboolean m42_node_same (const M42Node *a, const M42Node *b);

/* Whether the pattern is found anywhere in the tree, the whole of it
 * included, which is what FreeQ asks the other way round. */
gboolean m42_node_contains (const M42Node *tree, const M42Node *pattern,
                            M42PatternTest test, gpointer user_data);

/* Every piece of the tree that answers to the pattern, outermost
 * first.  The caller owns the array and the trees in it. */
GPtrArray *m42_node_collect (const M42Node *tree, const M42Node *pattern,
                             M42PatternTest test, gpointer user_data);

/* Everywhere the pattern is found in the tree, from the outside in,
 * the rule's right side takes its place.  Returns a new tree, and sets
 * *changed when anything was replaced. */
M42Node *m42_node_replace_all (const M42Node *tree, const M42Node *pattern,
                               const M42Node *result, M42PatternTest test,
                               gpointer user_data, gboolean *changed);

G_END_DECLS
