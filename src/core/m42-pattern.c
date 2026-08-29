/* m42-pattern.c - matching one expression against the shape of another
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A pattern is an expression with holes in it.  x_ is a hole that any
 * one thing fills, x_Integer a hole only a whole number fills, and x__
 * a hole that takes as many things as it can get.  Matching walks the
 * pattern and the subject side by side; every hole with a name writes
 * down what filled it, and a name that turns up twice has to be filled
 * the same way both times.
 *
 * Plus and Times are written here as trees of two branches, so a + b
 * is not quite the flat sum Mathematica matches against.  What we can
 * do cheaply is try the two branches the other way round when the
 * first way fails, which is what lets c_ x_ find the 2 in 2 x however
 * it was written.
 */

#include "m42-pattern.h"
#include "m42-lexer.h"

#include <math.h>
#include <string.h>

/* --- the names a match writes down --------------------------------------- */

GHashTable *
m42_pattern_names_new (void)
{
  return g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                (GDestroyNotify) m42_node_free);
}

static GHashTable *
names_copy (GHashTable *names)
{
  GHashTable *out = m42_pattern_names_new ();
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, names);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (out, g_strdup (key), m42_node_copy (value));
  return out;
}

/* Puts back what was written down before a guess that did not work. */
static void
names_restore (GHashTable *names, GHashTable *saved)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_remove_all (names);
  g_hash_table_iter_init (&iter, saved);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (names, g_strdup (key), m42_node_copy (value));
}

/* Two trees are the same when they are written the same way. */
gboolean
m42_node_same (const M42Node *a, const M42Node *b)
{
  if (a->kind != b->kind || a->op != b->op)
    return FALSE;
  if (a->kind == M42_NODE_NUMBER && a->number != b->number)
    return FALSE;
  if (g_strcmp0 (a->name, b->name) != 0)
    return FALSE;
  if (a->children->len != b->children->len)
    return FALSE;
  for (guint i = 0; i < a->children->len; i++)
    if (!m42_node_same (m42_node_child (a, i), m42_node_child (b, i)))
      return FALSE;
  return TRUE;
}

/* --- what a thing is called ---------------------------------------------- */

/* The head of an expression, which is what _Integer and its kind are
 * asking about. */
static const char *
head_of (const M42Node *n)
{
  switch (n->kind)
    {
    case M42_NODE_NUMBER:
      return isfinite (n->number) && n->number == floor (n->number) ? "Integer" : "Real";
    case M42_NODE_STRING:  return "String";
    case M42_NODE_IDENT:   return "Symbol";
    case M42_NODE_LIST:    return "List";
    case M42_NODE_MATRIX:  return "List";
    case M42_NODE_RULE:    return "Rule";
    case M42_NODE_CALL:    return n->name;
    case M42_NODE_UNARY:
      return n->op == M42_TOK_MINUS ? "Times" : "Symbol";
    case M42_NODE_BINARY:
      switch (n->op)
        {
        case M42_TOK_PLUS: case M42_TOK_MINUS:   return "Plus";
        case M42_TOK_STAR: case M42_TOK_SLASH:   return "Times";
        case M42_TOK_CARET:                      return "Power";
        default:                                 return "Expression";
        }
    default:
      return "Expression";
    }
}

/* A whole number answers to _Real and _Rational as well, the way a
 * first look at patterns is usually explained. */
static gboolean
head_allows (const char *wanted, const M42Node *subject)
{
  const char *actual = head_of (subject);

  if (strcmp (wanted, actual) == 0)
    return TRUE;
  if (strcmp (actual, "Integer") == 0 &&
      (strcmp (wanted, "Real") == 0 || strcmp (wanted, "Rational") == 0))
    return TRUE;
  return FALSE;
}

gboolean
m42_node_has_pattern (const M42Node *n)
{
  if (n == NULL)
    return FALSE;
  if (n->kind == M42_NODE_PATTERN || n->kind == M42_NODE_CONDITION)
    return TRUE;
  for (guint i = 0; i < n->children->len; i++)
    if (m42_node_has_pattern (m42_node_child (n, i)))
      return TRUE;
  return FALSE;
}

/* --- matching ------------------------------------------------------------ */

typedef struct {
  M42PatternTest test;
  gpointer       user_data;
} Match;

/* Matching does not stop at the first way a pattern can be filled: a
 * test after /; may throw that way out, and then the sequence that
 * came before it has to try the next.  So every step hands what it
 * found to whatever comes after, and takes back its guess when that
 * says no. */
typedef gboolean (*Cont) (gpointer data);

static gboolean match_one (const Match *m, const M42Node *pattern,
                           const M42Node *subject, GHashTable *names,
                           Cont k, gpointer kd);
static gboolean match_row (const Match *m, GPtrArray *patterns, guint pi,
                           GPtrArray *subjects, guint si, GHashTable *names,
                           Cont k, gpointer kd);

/* Nothing comes after: the match is done and it stands. */
static gboolean
accept (gpointer data)
{
  (void) data;
  return TRUE;
}

/* The rest of a row of arguments. */
typedef struct {
  const Match *m;
  GPtrArray   *patterns;
  guint        pi;
  GPtrArray   *subjects;
  guint        si;
  GHashTable  *names;
  Cont         k;
  gpointer     kd;
} RowNext;

static gboolean
row_next (gpointer data)
{
  RowNext *r = data;

  return match_row (r->m, r->patterns, r->pi, r->subjects, r->si, r->names, r->k, r->kd);
}

/* One more thing to match after this one -- the other branch of a sum
 * read the other way round. */
typedef struct {
  const Match   *m;
  const M42Node *pattern;
  const M42Node *subject;
  GHashTable    *names;
  Cont           k;
  gpointer       kd;
} OneNext;

static gboolean
one_next (gpointer data)
{
  OneNext *o = data;

  return match_one (o->m, o->pattern, o->subject, o->names, o->k, o->kd);
}

/* The test a match still has to pass. */
typedef struct {
  const Match   *m;
  const M42Node *test;
  GHashTable    *names;
  Cont           k;
  gpointer       kd;
} TestNext;

static gboolean
test_next (gpointer data)
{
  TestNext *t = data;

  if (t->m->test != NULL && !t->m->test (t->test, t->names, t->m->user_data))
    return FALSE;
  return t->k (t->kd);
}

/* Writes down that a name stands for this, or checks that it already
 * stood for the same.  It takes the node either way. */
static gboolean
remember (GHashTable *names, const char *name, M42Node *what)
{
  M42Node *before;

  if (name == NULL || name[0] == 0)
    {
      m42_node_free (what);
      return TRUE;
    }
  before = g_hash_table_lookup (names, name);
  if (before != NULL)
    {
      gboolean agrees = m42_node_same (before, what);

      m42_node_free (what);
      return agrees;
    }
  g_hash_table_insert (names, g_strdup (name), what);
  return TRUE;
}

/* The things a sequence swallowed, kept as a list that knows it is a
 * sequence. */
static M42Node *
sequence_of (GPtrArray *subjects, guint from, guint count)
{
  M42Node *list = m42_node_new (M42_NODE_LIST);

  list->op = M42_LIST_IS_SEQUENCE;
  for (guint i = 0; i < count; i++)
    g_ptr_array_add (list->children,
                     m42_node_copy (g_ptr_array_index (subjects, from + i)));
  return list;
}

/* Arguments side by side, with room for a sequence to take several. */
static gboolean
match_row (const Match *m, GPtrArray *patterns, guint pi,
           GPtrArray *subjects, guint si, GHashTable *names, Cont k, gpointer kd)
{
  const M42Node *pattern;

  if (pi == patterns->len)
    return si == subjects->len && k (kd);
  pattern = g_ptr_array_index (patterns, pi);

  if (pattern->kind == M42_NODE_PATTERN && pattern->op != M42_BLANK)
    {
      guint least = pattern->op == M42_BLANK_SEQUENCE ? 1 : 0;
      guint most = subjects->len - si;

      for (guint take = least; take <= most; take++)
        {
          g_autoptr (GHashTable) saved = names_copy (names);
          RowNext rest = { m, patterns, pi + 1, subjects, si + take, names, k, kd };
          gboolean fits = TRUE;

          /* Every one of them has to answer to the head, when one was
           * asked for. */
          if (pattern->children->len > 0)
            for (guint i = 0; i < take && fits; i++)
              fits = head_allows (m42_node_child (pattern, 0)->name,
                                  g_ptr_array_index (subjects, si + i));
          if (fits && remember (names, pattern->name, sequence_of (subjects, si, take)) &&
              row_next (&rest))
            return TRUE;
          names_restore (names, saved);
        }
      return FALSE;
    }

  if (si == subjects->len)
    return FALSE;
  {
    g_autoptr (GHashTable) saved = names_copy (names);
    RowNext rest = { m, patterns, pi + 1, subjects, si + 1, names, k, kd };

    if (match_one (m, pattern, g_ptr_array_index (subjects, si), names, row_next, &rest))
      return TRUE;
    names_restore (names, saved);
  }
  return FALSE;
}

/* Plus and Times may be read in either order. */
static gboolean
turns_around (const M42Node *n)
{
  return n->kind == M42_NODE_BINARY && n->children->len == 2 &&
         (n->op == M42_TOK_PLUS || n->op == M42_TOK_STAR);
}

static gboolean
match_one (const Match *m, const M42Node *pattern, const M42Node *subject,
           GHashTable *names, Cont k, gpointer kd)
{
  if (pattern->kind == M42_NODE_PATTERN)
    {
      g_autoptr (GHashTable) saved = names_copy (names);

      if (pattern->children->len > 0 &&
          !head_allows (m42_node_child (pattern, 0)->name, subject))
        return FALSE;
      if (remember (names, pattern->name, m42_node_copy (subject)) && k (kd))
        return TRUE;
      names_restore (names, saved);
      return FALSE;
    }

  if (pattern->kind == M42_NODE_CONDITION)
    {
      /* p ? f hands what matched to f, so the test to run is built
       * here, around the subject; p /; test is a test as written, and
       * the names it uses are filled in when it runs. */
      g_autoptr (M42Node) applied =
        pattern->op == M42_PATTERN_TEST
          ? m42_node_call1 (m42_node_child (pattern, 1)->name, m42_node_copy (subject))
          : NULL;
      TestNext then = { m, applied != NULL ? applied : m42_node_child (pattern, 1),
                        names, k, kd };

      /* The test comes after the shape, and a shape that can be filled
       * more than one way gets to try again when the test says no. */
      return match_one (m, m42_node_child (pattern, 0), subject, names, test_next, &then);
    }

  /* a | b: one shape or another.  It is written with the same token as
   * MATLAB's or, and means this only where a pattern is wanted, which
   * is the only place the matcher is ever asked. */
  if (pattern->kind == M42_NODE_BINARY && pattern->op == M42_TOK_OR &&
      pattern->children->len == 2)
    {
      g_autoptr (GHashTable) saved = names_copy (names);

      if (match_one (m, m42_node_child (pattern, 0), subject, names, k, kd))
        return TRUE;
      names_restore (names, saved);
      if (match_one (m, m42_node_child (pattern, 1), subject, names, k, kd))
        return TRUE;
      names_restore (names, saved);
      return FALSE;
    }

  if (pattern->kind != subject->kind || pattern->op != subject->op)
    return FALSE;
  if (pattern->kind == M42_NODE_NUMBER)
    return pattern->number == subject->number && k (kd);
  if (g_strcmp0 (pattern->name, subject->name) != 0)
    return FALSE;
  if (pattern->kind == M42_NODE_IDENT || pattern->kind == M42_NODE_STRING)
    return k (kd);

  {
    g_autoptr (GHashTable) saved = names_copy (names);

    if (match_row (m, pattern->children, 0, subject->children, 0, names, k, kd))
      return TRUE;
    names_restore (names, saved);
  }

  /* a + b is the same sum as b + a, and a b the same product. */
  if (turns_around (pattern) && turns_around (subject))
    {
      g_autoptr (GHashTable) saved = names_copy (names);
      OneNext then = { m, m42_node_child (pattern, 1), m42_node_child (subject, 0),
                       names, k, kd };

      if (match_one (m, m42_node_child (pattern, 0), m42_node_child (subject, 1), names,
                     one_next, &then))
        return TRUE;
      names_restore (names, saved);
    }
  return FALSE;
}

gboolean
m42_node_match (const M42Node *pattern, const M42Node *subject, GHashTable *names,
                M42PatternTest test, gpointer user_data)
{
  Match m = { test, user_data };

  if (pattern == NULL || subject == NULL)
    return FALSE;
  return match_one (&m, pattern, subject, names, accept, NULL);
}

/* --- putting the answer back together ------------------------------------ */

/* What a name stands for, when it stands for several things. */
static const M42Node *
sequence_for (GHashTable *names, const M42Node *n)
{
  const M42Node *bound;

  if (n->kind != M42_NODE_IDENT)
    return NULL;
  bound = g_hash_table_lookup (names, n->name);
  if (bound != NULL && bound->kind == M42_NODE_LIST && bound->op == M42_LIST_IS_SEQUENCE)
    return bound;
  return NULL;
}

M42Node *
m42_node_bind (const M42Node *body, GHashTable *names)
{
  M42Node *out;

  if (body == NULL)
    return NULL;
  if (body->kind == M42_NODE_IDENT)
    {
      M42Node *bound = g_hash_table_lookup (names, body->name);

      if (bound != NULL)
        {
          M42Node *copy = m42_node_copy (bound);

          /* On its own, with nowhere to spread into, a sequence is
           * just a list again.  Nothing else has its op touched: for a
           * sum or a product that is which operator it is. */
          if (copy->kind == M42_NODE_LIST && copy->op == M42_LIST_IS_SEQUENCE)
            copy->op = 0;
          return copy;
        }
    }

  out = m42_node_new (body->kind);
  out->op = body->op;
  out->number = body->number;
  out->name = g_strdup (body->name);
  for (guint i = 0; i < body->children->len; i++)
    {
      const M42Node *child = m42_node_child (body, i);
      const M42Node *spread = sequence_for (names, child);

      /* Several things put back side by side, not as one list. */
      if (spread != NULL)
        for (guint k = 0; k < spread->children->len; k++)
          g_ptr_array_add (out->children, m42_node_copy (m42_node_child (spread, k)));
      else
        g_ptr_array_add (out->children, m42_node_bind (child, names));
    }
  return out;
}

/* --- replacing ------------------------------------------------------------ */

gboolean
m42_node_contains (const M42Node *tree, const M42Node *pattern,
                   M42PatternTest test, gpointer user_data)
{
  g_autoptr (GHashTable) names = m42_pattern_names_new ();

  if (tree == NULL)
    return FALSE;
  if (m42_node_match (pattern, tree, names, test, user_data))
    return TRUE;
  for (guint i = 0; i < tree->children->len; i++)
    if (m42_node_contains (m42_node_child (tree, i), pattern, test, user_data))
      return TRUE;
  return FALSE;
}

static void
collect_into (GPtrArray *found, const M42Node *tree, const M42Node *pattern,
              M42PatternTest test, gpointer user_data)
{
  g_autoptr (GHashTable) names = m42_pattern_names_new ();

  if (tree == NULL)
    return;
  if (m42_node_match (pattern, tree, names, test, user_data))
    {
      g_ptr_array_add (found, m42_node_copy (tree));
      return;                  /* what was found is not looked inside */
    }
  for (guint i = 0; i < tree->children->len; i++)
    collect_into (found, m42_node_child (tree, i), pattern, test, user_data);
}

GPtrArray *
m42_node_collect (const M42Node *tree, const M42Node *pattern,
                  M42PatternTest test, gpointer user_data)
{
  GPtrArray *found = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_node_free);

  collect_into (found, tree, pattern, test, user_data);
  return found;
}

M42Node *
m42_node_replace_all (const M42Node *tree, const M42Node *pattern,
                      const M42Node *result, M42PatternTest test,
                      gpointer user_data, gboolean *changed)
{
  g_autoptr (GHashTable) names = m42_pattern_names_new ();
  M42Node *out;

  if (tree == NULL)
    return NULL;

  /* From the outside in: the largest piece that answers to the pattern
   * is the one replaced, and what took its place is left alone. */
  if (m42_node_match (pattern, tree, names, test, user_data))
    {
      if (changed != NULL)
        *changed = TRUE;
      return m42_node_bind (result, names);
    }

  out = m42_node_new (tree->kind);
  out->op = tree->op;
  out->number = tree->number;
  out->name = g_strdup (tree->name);
  for (guint i = 0; i < tree->children->len; i++)
    g_ptr_array_add (out->children,
                     m42_node_replace_all (m42_node_child (tree, i), pattern, result,
                                           test, user_data, changed));
  return out;
}
