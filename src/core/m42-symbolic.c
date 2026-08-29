/* m42-symbolic.c - see m42-symbolic.h
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m42-symbolic.h"
#include "m42-lexer.h"

#include <math.h>
#include <string.h>

/* --- printing --------------------------------------------------------- */

/* How tightly a node binds, so that a child that binds less tightly
 * than its parent is parenthesised. */
static int
precedence (const M42Node *n)
{
  switch (n->kind)
    {
    case M42_NODE_SEQ:     return 0;
    case M42_NODE_ASSIGN:
    case M42_NODE_DEFINE:
    case M42_NODE_FUNCDEF: return 1;
    case M42_NODE_REPLACE:   return 2;
    case M42_NODE_RULE:      return 3;
    case M42_NODE_CONDITION: return 3;
    case M42_NODE_PATTERN:   return 12;
    case M42_NODE_RANGE:   return 6;
    case M42_NODE_BINARY:
      switch (n->op)
        {
        case M42_TOK_OR:    return 4;
        case M42_TOK_AND:   return 5;
        case M42_TOK_EQ: case M42_TOK_NE: case M42_TOK_LT:
        case M42_TOK_LE: case M42_TOK_GT: case M42_TOK_GE:
          return 6;
        case M42_TOK_PLUS: case M42_TOK_MINUS:
          return 7;
        case M42_TOK_STAR: case M42_TOK_SLASH: case M42_TOK_PERCENT:
        case M42_TOK_DOT: case M42_TOK_BACKSLASH:
          return 8;
        case M42_TOK_CARET: return 10;
        default:            return 8;
        }
    case M42_NODE_UNARY:
      return n->op == M42_TOK_MINUS || n->op == M42_TOK_NOT ? 9 : 11;
    case M42_NODE_LAMBDA:  return 1;
    default:               return 12;
    }
}

static const char *
op_text (int op)
{
  switch (op)
    {
    case M42_TOK_PLUS:    return " + ";
    case M42_TOK_MINUS:   return " - ";
    case M42_TOK_STAR:    return " ";
    case M42_TOK_SLASH:   return "/";
    case M42_TOK_PERCENT: return " % ";
    case M42_TOK_CARET:   return "^";
    case M42_TOK_DOT:     return " . ";
    case M42_TOK_BACKSLASH: return " \\ ";
    case M42_TOK_EQ:      return " == ";
    case M42_TOK_NE:      return " != ";
    case M42_TOK_LT:      return " < ";
    case M42_TOK_LE:      return " <= ";
    case M42_TOK_GT:      return " > ";
    case M42_TOK_GE:      return " >= ";
    case M42_TOK_AND:     return " && ";
    case M42_TOK_OR:      return " || ";
    default:              return " ? ";
    }
}

static void
print_child (GString *out, const M42Node *parent, const M42Node *child, gboolean right)
{
  int pp = precedence (parent), cp = precedence (child);
  /* Subtraction and division need parentheses on an equal-precedence
   * right child -- a - (b - c) -- while + and * do not, since the way
   * they group makes no difference; ^ needs them on the left instead. */
  gboolean right_matters = parent->kind == M42_NODE_BINARY &&
                           (parent->op == M42_TOK_MINUS || parent->op == M42_TOK_SLASH ||
                            parent->op == M42_TOK_PERCENT);
  /* -3 Exp[x] reads perfectly well at the front of a product; it is
   * only on the right of an operator, or under a power, that a
   * negative number needs its brackets. */
  gboolean negative_needs_it = child->kind == M42_NODE_NUMBER && child->number < 0 &&
                               (right || pp >= 10);
  gboolean paren = cp < pp ||
                   (cp == pp && right && right_matters) ||
                   (cp == pp && !right && parent->kind == M42_NODE_BINARY && parent->op == M42_TOK_CARET) ||
                   negative_needs_it;

  if (paren)
    g_string_append_c (out, '(');
  m42_node_to_string (out, child);
  if (paren)
    g_string_append_c (out, ')');
}

static void
print_args (GString *out, const M42Node *n, guint from, const char *open, const char *close)
{
  g_string_append (out, open);
  for (guint i = from; i < n->children->len; i++)
    {
      if (i > from)
        g_string_append (out, ", ");
      m42_node_to_string (out, m42_node_child (n, i));
    }
  g_string_append (out, close);
}

void
m42_node_to_string (GString *out, const M42Node *n)
{
  switch (n->kind)
    {
    case M42_NODE_NUMBER:
      m42_number_to_string (out, n->number);
      break;
    case M42_NODE_IDENT:
      g_string_append (out, n->name);
      break;
    case M42_NODE_STRING:
      g_string_append_c (out, '"');
      g_string_append (out, n->name);
      g_string_append_c (out, '"');
      break;
    case M42_NODE_LAST:
      g_string_append_c (out, '%');
      break;
    case M42_NODE_UNARY:
      if (n->op == M42_TOK_MINUS || n->op == M42_TOK_NOT)
        {
          g_string_append_c (out, n->op == M42_TOK_MINUS ? '-' : '!');
          print_child (out, n, m42_node_child (n, 0), TRUE);
        }
      else
        {
          print_child (out, n, m42_node_child (n, 0), FALSE);
          g_string_append_c (out, n->op == M42_TOK_BANG ? '!' : '\'');
        }
      break;
    case M42_NODE_BINARY:
      {
        const M42Node *a = m42_node_child (n, 0), *b = m42_node_child (n, 1);
        print_child (out, n, a, FALSE);
        /* x 2 would read back as x*2 only with the star; 2 x is fine. */
        if (n->op == M42_TOK_STAR && (b->kind == M42_NODE_NUMBER ||
                                      (b->kind == M42_NODE_UNARY && b->op == M42_TOK_MINUS)))
          g_string_append (out, "*");
        else
          g_string_append (out, op_text (n->op));
        print_child (out, n, b, TRUE);
      }
      break;
    case M42_NODE_CALL:
      g_string_append (out, n->name);
      print_args (out, n, 0, "[", "]");
      break;
    case M42_NODE_LIST:
      print_args (out, n, 0, "{", "}");
      break;
    case M42_NODE_MATRIX:
      g_string_append_c (out, '[');
      for (guint i = 0; i < n->children->len; i++)
        {
          const M42Node *row = m42_node_child (n, i);
          if (i > 0)
            g_string_append (out, "; ");
          for (guint j = 0; j < row->children->len; j++)
            {
              if (j > 0)
                g_string_append (out, ", ");
              m42_node_to_string (out, m42_node_child (row, j));
            }
        }
      g_string_append_c (out, ']');
      break;
    case M42_NODE_RANGE:
      print_child (out, n, m42_node_child (n, 0), FALSE);
      if (n->children->len == 3)
        {
          g_string_append_c (out, ':');
          print_child (out, n, m42_node_child (n, 2), TRUE);
        }
      g_string_append_c (out, ':');
      print_child (out, n, m42_node_child (n, 1), TRUE);
      break;
    case M42_NODE_PART:
      m42_node_to_string (out, m42_node_child (n, 0));
      print_args (out, n, 1, "[[", "]]");
      break;
    case M42_NODE_APPLYFN:
      m42_node_to_string (out, m42_node_child (n, 0));
      print_args (out, n, 1, "[", "]");
      break;
    case M42_NODE_ASSIGN:
      g_string_append_printf (out, "%s = ", n->name);
      m42_node_to_string (out, m42_node_child (n, 0));
      break;
    case M42_NODE_FUNCDEF:
      g_string_append (out, n->name);
      g_string_append_c (out, '[');
      for (guint i = 0; i + 1 < n->children->len; i++)
        g_string_append_printf (out, "%s%s_", i > 0 ? ", " : "", m42_node_child (n, i)->name);
      g_string_append (out, "] := ");
      m42_node_to_string (out, m42_node_child (n, n->children->len - 1));
      break;
    case M42_NODE_LAMBDA:
      g_string_append (out, "Function[{");
      for (guint i = 0; i + 1 < n->children->len; i++)
        g_string_append_printf (out, "%s%s", i > 0 ? ", " : "", m42_node_child (n, i)->name);
      g_string_append (out, "}, ");
      m42_node_to_string (out, m42_node_child (n, n->children->len - 1));
      g_string_append_c (out, ']');
      break;
    case M42_NODE_DEFINE:
      m42_node_to_string (out, m42_node_child (n, 0));
      g_string_append (out, " := ");
      m42_node_to_string (out, m42_node_child (n, 1));
      break;
    case M42_NODE_RULE:
      m42_node_to_string (out, m42_node_child (n, 0));
      g_string_append (out, n->op ? " :> " : " -> ");
      m42_node_to_string (out, m42_node_child (n, 1));
      break;
    case M42_NODE_PATTERN:
      /* x, then as many bars as it stands for things, then the head it
       * is held to: x_, __, x___Integer. */
      if (n->name != NULL)
        g_string_append (out, n->name);
      for (int i = 0; i <= n->op; i++)
        g_string_append_c (out, '_');
      if (n->children->len > 0)
        g_string_append (out, m42_node_child (n, 0)->name);
      break;
    case M42_NODE_CONDITION:
      m42_node_to_string (out, m42_node_child (n, 0));
      g_string_append (out, n->op ? "?" : " /; ");
      m42_node_to_string (out, m42_node_child (n, 1));
      break;
    case M42_NODE_REPLACE:
      m42_node_to_string (out, m42_node_child (n, 0));
      for (guint i = 1; i < n->children->len; i++)
        {
          g_string_append (out, n->op ? " //. " : " /. ");
          m42_node_to_string (out, m42_node_child (n, i));
        }
      break;
    case M42_NODE_SEQ:
      for (guint i = 0; i < n->children->len; i++)
        {
          if (i > 0)
            g_string_append (out, "; ");
          m42_node_to_string (out, m42_node_child (n, i));
        }
      if (n->op)
        g_string_append_c (out, ';');
      break;
    }
}

/* --- simplification ---------------------------------------------------- */

static gboolean
is_num (const M42Node *n, double x)
{
  return n->kind == M42_NODE_NUMBER && n->number == x;
}

static gboolean
is_number (const M42Node *n)
{
  return n->kind == M42_NODE_NUMBER;
}

static double
fold (int op, double a, double b)
{
  switch (op)
    {
    case M42_TOK_PLUS:  return a + b;
    case M42_TOK_MINUS: return a - b;
    case M42_TOK_STAR:  return a * b;
    case M42_TOK_SLASH: return a / b;
    case M42_TOK_CARET: return pow (a, b);
    default:            return NAN;
    }
}

/* The number at the front of a product, however deeply the product is
 * nested to the left: 3 (2 x y) is 6 x y.  Returns what is left, or
 * NULL when the whole thing was a number. */
static M42Node *
fold_leading_number (M42Node *n, double *factor)
{
  if (n->kind == M42_NODE_NUMBER)
    {
      *factor *= n->number;
      m42_node_free (n);
      return NULL;
    }
  if (n->kind == M42_NODE_BINARY && n->op == M42_TOK_STAR)
    {
      M42Node *left = g_ptr_array_steal_index (n->children, 0);
      M42Node *right = g_ptr_array_steal_index (n->children, 0);
      M42Node *rest = fold_leading_number (left, factor);

      m42_node_free (n);
      if (rest == NULL)
        return right;
      return m42_node_binary (M42_TOK_STAR, rest, right);
    }
  return n;
}

/* A term that carries a minus sign, with the sign taken off: -x, -2,
 * -2 x and (-x)/3 all come back as what they are without it, so that a
 * sum can be written as a subtraction. */
static M42Node *
without_minus (const M42Node *n)
{
  if (n->kind == M42_NODE_UNARY && n->op == M42_TOK_MINUS)
    return m42_node_copy (m42_node_child (n, 0));
  if (n->kind == M42_NODE_NUMBER && n->number < 0)
    return m42_node_number (-n->number);
  if (n->kind == M42_NODE_BINARY && n->op == M42_TOK_STAR)
    {
      M42Node *first = without_minus (m42_node_child (n, 0));

      if (first != NULL)
        return m42_node_binary (M42_TOK_STAR, first,
                                m42_node_copy (m42_node_child (n, 1)));
      return NULL;
    }
  if (n->kind == M42_NODE_BINARY && n->op == M42_TOK_SLASH)
    {
      M42Node *top = without_minus (m42_node_child (n, 0));

      if (top != NULL)
        return m42_node_binary (M42_TOK_SLASH, top,
                                m42_node_copy (m42_node_child (n, 1)));
      return NULL;
    }
  return NULL;
}

/* Simplifies a tree that was built for the purpose, and frees it. */
static M42Node *
simplify_and_free (M42Node *n)
{
  M42Node *r = m42_node_simplify (n);
  m42_node_free (n);
  return r;
}

M42Node *
m42_node_simplify (const M42Node *n)
{
  M42Node *r;

  /* A number that is a simple fraction is written as one, which lets
   * the rules below gather it with the other fractions: 0.5 x^2/2
   * becomes x^2/4 rather than staying as it is. */
  if (n->kind == M42_NODE_NUMBER && n->number != floor (n->number) &&
      isfinite (n->number))
    {
      for (int den = 2; den <= 16; den++)
        {
          double scaled = n->number * den;

          if (fabs (scaled - round (scaled)) < 1e-12 * MAX (1.0, fabs (scaled)))
            return m42_node_binary (M42_TOK_SLASH, m42_node_number (round (scaled)),
                                    m42_node_number (den));
        }
    }

  if (n->kind == M42_NODE_UNARY && n->op == M42_TOK_MINUS)
    {
      M42Node *a = m42_node_simplify (m42_node_child (n, 0));

      if (is_number (a))
        {
          a->number = -a->number;
          return a;
        }
      if (a->kind == M42_NODE_UNARY && a->op == M42_TOK_MINUS)
        {
          M42Node *inner = g_ptr_array_steal_index (a->children, 0);
          m42_node_free (a);
          return inner;
        }
      /* Minus a difference is the difference the other way round, which
       * is how -(c - y) comes out as y - c. */
      if (a->kind == M42_NODE_BINARY && a->op == M42_TOK_MINUS)
        {
          M42Node *left = g_ptr_array_steal_index (a->children, 0);
          M42Node *right = g_ptr_array_steal_index (a->children, 0);

          m42_node_free (a);
          return simplify_and_free (m42_node_binary (M42_TOK_MINUS, right, left));
        }
      return m42_node_unary (M42_TOK_MINUS, a);
    }

  if (n->kind == M42_NODE_BINARY)
    {
      M42Node *a = m42_node_simplify (m42_node_child (n, 0));
      M42Node *b = m42_node_simplify (m42_node_child (n, 1));
      int op = n->op;

      if (is_number (a) && is_number (b) &&
          (op == M42_TOK_PLUS || op == M42_TOK_MINUS || op == M42_TOK_STAR ||
           op == M42_TOK_SLASH || op == M42_TOK_CARET))
        {
          double x = fold (op, a->number, b->number);
          /* Keep 1/3 as a fraction rather than a decimal. */
          if (op != M42_TOK_SLASH || x == floor (x) || !isfinite (x))
            {
              m42_node_free (a);
              m42_node_free (b);
              return m42_node_number (x);
            }
          /* A fraction of two whole numbers is put in lowest terms, so
           * that the derivative of x/3 is 1/3 and not 3/9. */
          if (a->number == floor (a->number) && b->number == floor (b->number) &&
              fabs (a->number) < 1e15 && fabs (b->number) < 1e15)
            {
              gint64 p = (gint64) a->number, q = (gint64) b->number, g;
              gint64 u = ABS (p), v = ABS (q);

              while (v != 0)
                {
                  gint64 t = u % v;
                  u = v;
                  v = t;
                }
              g = u == 0 ? 1 : u;
              if (q < 0)
                {
                  p = -p;
                  q = -q;
                }
              a->number = (double) (p / g);
              b->number = (double) (q / g);
            }
        }

#define DROP_A() do { m42_node_free (a); return b; } while (0)
#define DROP_B() do { m42_node_free (b); return a; } while (0)
      switch (op)
        {
        case M42_TOK_PLUS:
          if (is_num (a, 0)) DROP_A ();
          if (is_num (b, 0)) DROP_B ();
          /* a + -b -> a - b, whether the minus is an operator or part
           * of the number: a sum should never read "+ (-6)". */
          if (b->kind == M42_NODE_UNARY && b->op == M42_TOK_MINUS)
            {
              M42Node *inner = g_ptr_array_steal_index (b->children, 0);
              m42_node_free (b);
              return m42_node_binary (M42_TOK_MINUS, a, inner);
            }
          {
            M42Node *positive = without_minus (b);

            if (positive != NULL)
              {
                m42_node_free (b);
                return m42_node_binary (M42_TOK_MINUS, a, positive);
              }
          }
          /* a + (-c + d) is (a - c) + d: without this a sum built from
           * the right keeps a "+ -" in the middle of it. */
          if (b->kind == M42_NODE_BINARY && (b->op == M42_TOK_PLUS || b->op == M42_TOK_MINUS))
            {
              M42Node *positive = without_minus (m42_node_child (b, 0));

              if (positive != NULL)
                {
                  M42Node *tail = g_ptr_array_steal_index (b->children, 1);
                  int tail_op = b->op;

                  m42_node_free (b);
                  return simplify_and_free (
                    m42_node_binary (tail_op,
                                     m42_node_binary (M42_TOK_MINUS, a, positive), tail));
                }
            }
          break;
        case M42_TOK_MINUS:
          if (is_num (b, 0)) DROP_B ();
          /* a - (-c) is a + c. */
          if (is_number (b) && b->number < 0)
            {
              b->number = -b->number;
              return m42_node_binary (M42_TOK_PLUS, a, b);
            }
          if (b->kind == M42_NODE_UNARY && b->op == M42_TOK_MINUS)
            {
              M42Node *inner = g_ptr_array_steal_index (b->children, 0);

              m42_node_free (b);
              return simplify_and_free (m42_node_binary (M42_TOK_PLUS, a, inner));
            }
          if (is_num (a, 0))
            {
              m42_node_free (a);
              return m42_node_unary (M42_TOK_MINUS, b);
            }
          break;
        case M42_TOK_STAR:
          if (is_num (a, 0) || is_num (b, 0))
            {
              m42_node_free (a);
              m42_node_free (b);
              return m42_node_number (0);
            }
          if (is_num (a, 1)) DROP_A ();
          if (is_num (b, 1)) DROP_B ();
          /* Numbers go first: x 2 -> 2 x, and a (2 q) -> 2 (a q), so
           * that a derivative reads 2 a q rather than a 2 q. */
          if (is_number (b) && !is_number (a))
            {
              M42Node *t = a; a = b; b = t;
            }
          if (!is_number (a) && b->kind == M42_NODE_BINARY && b->op == M42_TOK_STAR &&
              is_number (m42_node_child (b, 0)))
            {
              M42Node *coefficient = g_ptr_array_steal_index (b->children, 0);
              M42Node *rest = g_ptr_array_steal_index (b->children, 0);

              m42_node_free (b);
              return simplify_and_free (
                m42_node_binary (M42_TOK_STAR, coefficient,
                                 m42_node_binary (M42_TOK_STAR, a, rest)));
            }
          /* 2 (3 x) is 6 x, and 3 (2 x y) is 6 x y. */
          if (is_number (a) && b->kind == M42_NODE_BINARY && b->op == M42_TOK_STAR)
            {
              double factor = a->number;
              M42Node *rest;

              m42_node_free (a);
              rest = fold_leading_number (b, &factor);
              if (rest == NULL)
                return m42_node_number (factor);
              if (factor == 1)
                return rest;
              return m42_node_binary (M42_TOK_STAR, m42_node_number (factor), rest);
            }
          /* -1 x -> -x */
          if (is_num (a, -1))
            {
              m42_node_free (a);
              return m42_node_unary (M42_TOK_MINUS, b);
            }
          /* a (-b) is -(a b), which reads better and prints without a
           * star between them. */
          for (int side = 0; side < 2; side++)
            {
              M42Node *negated = side ? a : b;
              M42Node *other = side ? b : a;

              if (negated->kind != M42_NODE_UNARY || negated->op != M42_TOK_MINUS)
                continue;
              {
                M42Node *inner = g_ptr_array_steal_index (negated->children, 0);

                m42_node_free (negated);
                return m42_node_unary (M42_TOK_MINUS,
                                       simplify_and_free (
                                         side ? m42_node_binary (M42_TOK_STAR, inner, other)
                                              : m42_node_binary (M42_TOK_STAR, other, inner)));
              }
            }
          /* A fraction anywhere in a product comes to the outside:
           * 2 (x^2/2) is x^2, and (1/2) ArcTan[x] is ArcTan[x]/2. */
          for (int side = 0; side < 2; side++)
            {
              M42Node *fraction = side ? a : b;
              M42Node *other = side ? b : a;

              if (fraction->kind != M42_NODE_BINARY || fraction->op != M42_TOK_SLASH)
                continue;
              {
                M42Node *top = g_ptr_array_steal_index (fraction->children, 0);
                M42Node *bottom = g_ptr_array_steal_index (fraction->children, 0);

                m42_node_free (fraction);
                return simplify_and_free (
                  m42_node_binary (M42_TOK_SLASH,
                                   side ? m42_node_binary (M42_TOK_STAR, top, other)
                                        : m42_node_binary (M42_TOK_STAR, other, top),
                                   bottom));
              }
            }
          /* Powers of the same thing add their exponents: x x is x^2,
           * and x^2 x is x^3. */
          {
            const M42Node *ba = a, *bb = b;
            double ea = 1, eb = 1;
            g_autoptr (GString) sa = g_string_new (NULL), sb = g_string_new (NULL);

            if (a->kind == M42_NODE_BINARY && a->op == M42_TOK_CARET &&
                m42_node_child (a, 1)->kind == M42_NODE_NUMBER)
              {
                ba = m42_node_child (a, 0);
                ea = m42_node_child (a, 1)->number;
              }
            if (b->kind == M42_NODE_BINARY && b->op == M42_TOK_CARET &&
                m42_node_child (b, 1)->kind == M42_NODE_NUMBER)
              {
                bb = m42_node_child (b, 0);
                eb = m42_node_child (b, 1)->number;
              }
            m42_node_to_string (sa, ba);
            m42_node_to_string (sb, bb);
            if (strcmp (sa->str, sb->str) == 0 && !is_number (ba))
              {
                M42Node *base = m42_node_copy (ba);
                m42_node_free (a);
                m42_node_free (b);
                return simplify_and_free (m42_node_binary (M42_TOK_CARET, base,
                                                           m42_node_number (ea + eb)));
              }
          }
          break;
        case M42_TOK_SLASH:
          if (is_num (b, 1)) DROP_B ();
          if (is_num (a, 0))
            {
              m42_node_free (a);
              m42_node_free (b);
              return m42_node_number (0);
            }
          /* A minus sign on top is written in front of the whole
           * fraction, so that what is left can cancel: -(2 Sqrt[a])/2
           * becomes -Sqrt[a]. */
          if (a->kind == M42_NODE_UNARY && a->op == M42_TOK_MINUS)
            {
              M42Node *inner = g_ptr_array_steal_index (a->children, 0);

              m42_node_free (a);
              return simplify_and_free (
                m42_node_unary (M42_TOK_MINUS,
                                m42_node_binary (M42_TOK_SLASH, inner, b)));
            }
          /* One fraction, not a tower of them: (p/q)/b is p/(q b), and
           * a/(p/q) is a q/p. */
          if (a->kind == M42_NODE_BINARY && a->op == M42_TOK_SLASH)
            {
              M42Node *p = g_ptr_array_steal_index (a->children, 0);
              M42Node *q = g_ptr_array_steal_index (a->children, 0);

              m42_node_free (a);
              return simplify_and_free (m42_node_binary (M42_TOK_SLASH, p,
                                                         m42_node_binary (M42_TOK_STAR, q, b)));
            }
          if (b->kind == M42_NODE_BINARY && b->op == M42_TOK_SLASH)
            {
              M42Node *p = g_ptr_array_steal_index (b->children, 0);
              M42Node *q = g_ptr_array_steal_index (b->children, 0);

              m42_node_free (b);
              return simplify_and_free (m42_node_binary (M42_TOK_SLASH,
                                                         m42_node_binary (M42_TOK_STAR, a, q), p));
            }
          /* A number in the bottom divides a number on top: 6/(2 a) is
           * 3/a.  It goes no further than that.  Pulling the number out
           * of any other fraction -- a/(c f) as (a/f)/c -- would only
           * push it back, since the rule above puts a tower of two
           * fractions back into one, and the two rules would hand
           * 1/(2 a) to each other for ever. */
          if (b->kind == M42_NODE_BINARY && b->op == M42_TOK_STAR &&
              is_number (a) && is_number (m42_node_child (b, 0)))
            {
              double top = a->number, bottom = m42_node_child (b, 0)->number;

              if (bottom != 0 && top == floor (top) && bottom == floor (bottom) &&
                  fmod (top, bottom) == 0)
                {
                  M42Node *rest = g_ptr_array_steal_index (b->children, 1);

                  m42_node_free (a);
                  m42_node_free (b);
                  return simplify_and_free (
                    m42_node_binary (M42_TOK_SLASH, m42_node_number (top / bottom), rest));
                }
            }
          /* Powers of the same thing divide by taking one exponent from
           * the other: x^2/x is x. */
          {
            const M42Node *ba = a, *bb = b;
            double ea = 1, eb = 1;
            g_autoptr (GString) sa = g_string_new (NULL), sb = g_string_new (NULL);

            if (a->kind == M42_NODE_BINARY && a->op == M42_TOK_CARET &&
                m42_node_child (a, 1)->kind == M42_NODE_NUMBER)
              {
                ba = m42_node_child (a, 0);
                ea = m42_node_child (a, 1)->number;
              }
            if (b->kind == M42_NODE_BINARY && b->op == M42_TOK_CARET &&
                m42_node_child (b, 1)->kind == M42_NODE_NUMBER)
              {
                bb = m42_node_child (b, 0);
                eb = m42_node_child (b, 1)->number;
              }
            m42_node_to_string (sa, ba);
            m42_node_to_string (sb, bb);
            if (strcmp (sa->str, sb->str) == 0 && !is_number (ba))
              {
                M42Node *base = m42_node_copy (ba);

                m42_node_free (a);
                m42_node_free (b);
                return simplify_and_free (m42_node_binary (M42_TOK_CARET, base,
                                                           m42_node_number (ea - eb)));
              }
          }
          /* A sum over a number is shared out, which is what turns
           * (2 x + 2)/2 into x + 1. */
          if (is_number (b) && b->number != 0 && a->kind == M42_NODE_BINARY &&
              (a->op == M42_TOK_PLUS || a->op == M42_TOK_MINUS))
            {
              M42Node *first = g_ptr_array_steal_index (a->children, 0);
              M42Node *second = g_ptr_array_steal_index (a->children, 0);
              int sum_op = a->op;

              m42_node_free (a);
              return simplify_and_free (
                m42_node_binary (sum_op,
                                 m42_node_binary (M42_TOK_SLASH, first, m42_node_copy (b)),
                                 m42_node_binary (M42_TOK_SLASH, second, b)));
            }
          /* (c f)/d with c and d numbers: the numbers are put in lowest
           * terms, so 8 x/16 comes out as x/2. */
          if (is_number (b) && a->kind == M42_NODE_BINARY && a->op == M42_TOK_STAR &&
              is_number (m42_node_child (a, 0)))
            {
              double c = m42_node_child (a, 0)->number, d = b->number;

              if (c == floor (c) && d == floor (d) && d != 0 &&
                  fabs (c) < 1e15 && fabs (d) < 1e15)
                {
                  gint64 p = (gint64) c, q = (gint64) d;
                  gint64 u = ABS (p), w = ABS (q), g;
                  M42Node *rest = g_ptr_array_steal_index (a->children, 1);

                  while (w != 0)
                    {
                      gint64 t = u % w;
                      u = w;
                      w = t;
                    }
                  g = u == 0 ? 1 : u;
                  if (q < 0)
                    {
                      p = -p;
                      q = -q;
                    }
                  p /= g;
                  q /= g;
                  m42_node_free (a);
                  m42_node_free (b);
                  if (p != 1)
                    rest = m42_node_binary (M42_TOK_STAR, m42_node_number ((double) p), rest);
                  if (q == 1)
                    return rest;
                  return m42_node_binary (M42_TOK_SLASH, rest, m42_node_number ((double) q));
                }
            }
          break;
        case M42_TOK_CARET:
          if (is_num (b, 1)) DROP_B ();
          if (is_num (b, 0) || is_num (a, 1))
            {
              m42_node_free (a);
              m42_node_free (b);
              return m42_node_number (1);
            }
          /* A power of a fraction is the powers of its two halves,
           * which is what lets (Sqrt[2]/2)^2 come out as 1/2. */
          if (is_number (b) && a->kind == M42_NODE_BINARY && a->op == M42_TOK_SLASH)
            {
              M42Node *p_top = g_ptr_array_steal_index (a->children, 0);
              M42Node *p_bottom = g_ptr_array_steal_index (a->children, 0);
              M42Node *e2 = m42_node_copy (b);

              m42_node_free (a);
              return simplify_and_free (
                m42_node_binary (M42_TOK_SLASH,
                                 m42_node_binary (M42_TOK_CARET, p_top, b),
                                 m42_node_binary (M42_TOK_CARET, p_bottom, e2)));
            }
          /* A negative power is written as one over a positive one. */
          if (is_number (b) && b->number < 0 && !is_number (a))
            {
              double e = -b->number;

              m42_node_free (b);
              return simplify_and_free (
                m42_node_binary (M42_TOK_SLASH, m42_node_number (1),
                                 e == 1 ? a : m42_node_binary (M42_TOK_CARET, a,
                                                               m42_node_number (e))));
            }
          /* A square root squared is what was under it. */
          if (is_num (b, 2) && a->kind == M42_NODE_CALL && a->children->len == 1 &&
              (strcmp (a->name, "Sqrt") == 0 || strcmp (a->name, "sqrt") == 0))
            {
              M42Node *under = g_ptr_array_steal_index (a->children, 0);

              m42_node_free (a);
              m42_node_free (b);
              return under;
            }
          break;
        default:
          break;
        }
#undef DROP_A
#undef DROP_B
      return m42_node_binary (op, a, b);
    }

  r = m42_node_new (n->kind);
  r->op = n->op;
  r->number = n->number;
  r->name = g_strdup (n->name);
  for (guint i = 0; i < n->children->len; i++)
    g_ptr_array_add (r->children, m42_node_simplify (m42_node_child (n, i)));

  /* A square inside a square root comes outside it: Sqrt[4 a] is
   * 2 Sqrt[a], which is what turns the roots of x^2 == a into
   * Sqrt[a] rather than Sqrt[4 a]/2. */
  if (r->kind == M42_NODE_CALL && r->children->len == 1 &&
      (strcmp (r->name, "Sqrt") == 0 || strcmp (r->name, "sqrt") == 0))
    {
      const M42Node *under = m42_node_child (r, 0);

      if (under->kind == M42_NODE_BINARY && under->op == M42_TOK_STAR &&
          m42_node_child (under, 0)->kind == M42_NODE_NUMBER)
        {
          double c = m42_node_child (under, 0)->number;
          double root = sqrt (c);

          if (c > 1 && root == floor (root))
            {
              M42Node *rest = m42_node_copy (m42_node_child (under, 1));
              M42Node *out = m42_node_binary (M42_TOK_STAR, m42_node_number (root),
                                              m42_node_call1 (r->name, rest));

              m42_node_free (r);
              return simplify_and_free (out);
            }
        }
    }

  /* The values everyone knows by heart: Exp[0] is 1, Log[1] is 0.  Only
   * these, so that Sin[2] stays as it is. */
  if (r->kind == M42_NODE_CALL && r->children->len == 1 &&
      m42_node_child (r, 0)->kind == M42_NODE_NUMBER)
    {
      static const struct { const char *name; double at, value; } KNOWN[] = {
        { "Exp", 0, 1 },   { "exp", 0, 1 },
        { "Log", 1, 0 },   { "log", 1, 0 },
        { "Sin", 0, 0 },   { "sin", 0, 0 },
        { "Cos", 0, 1 },   { "cos", 0, 1 },
        { "Tan", 0, 0 },   { "tan", 0, 0 },
        { "Sinh", 0, 0 },  { "Cosh", 0, 1 },
        { "ArcTan", 0, 0 }, { "ArcSin", 0, 0 },
        { "Sqrt", 0, 0 },  { "Sqrt", 1, 1 },
        { "sqrt", 0, 0 },  { "sqrt", 1, 1 },
        { "Abs", 0, 0 },   { "abs", 0, 0 },
      };
      double at = m42_node_child (r, 0)->number;

      for (guint i = 0; i < G_N_ELEMENTS (KNOWN); i++)
        if (strcmp (KNOWN[i].name, r->name) == 0 && KNOWN[i].at == at)
          {
            double value = KNOWN[i].value;

            m42_node_free (r);
            return m42_node_number (value);
          }
    }
  return r;
}

/* --- substitution ------------------------------------------------------- */

gboolean
m42_node_depends_on (const M42Node *n, const char *name)
{
  if (n->kind == M42_NODE_IDENT && strcmp (n->name, name) == 0)
    return TRUE;
  for (guint i = 0; i < n->children->len; i++)
    if (m42_node_depends_on (m42_node_child (n, i), name))
      return TRUE;
  return FALSE;
}

M42Node *
m42_node_substitute (const M42Node *n, const char *name, const M42Node *with)
{
  M42Node *r;

  if (n->kind == M42_NODE_IDENT && strcmp (n->name, name) == 0)
    return m42_node_copy (with);
  r = m42_node_new (n->kind);
  r->op = n->op;
  r->number = n->number;
  r->name = g_strdup (n->name);
  for (guint i = 0; i < n->children->len; i++)
    g_ptr_array_add (r->children, m42_node_substitute (m42_node_child (n, i), name, with));
  return r;
}

/* --- differentiation ---------------------------------------------------- */

#define NUM(x)      m42_node_number (x)
#define CP(n)       m42_node_copy (n)
#define ADD(a, b)   m42_node_binary (M42_TOK_PLUS, a, b)
#define SUB(a, b)   m42_node_binary (M42_TOK_MINUS, a, b)
#define MUL(a, b)   m42_node_binary (M42_TOK_STAR, a, b)
#define DIV(a, b)   m42_node_binary (M42_TOK_SLASH, a, b)
#define POW(a, b)   m42_node_binary (M42_TOK_CARET, a, b)
#define NEG(a)      m42_node_unary (M42_TOK_MINUS, a)
#define CALL(f, a)  m42_node_call1 (f, a)

/* d f(u) / du, for the functions with a known derivative. */
static M42Node *
derivative_of_function (const char *f, const M42Node *u)
{
  if (!strcmp (f, "Sin") || !strcmp (f, "sin"))     return CALL ("Cos", CP (u));
  if (!strcmp (f, "Cos") || !strcmp (f, "cos"))     return NEG (CALL ("Sin", CP (u)));
  if (!strcmp (f, "Tan") || !strcmp (f, "tan"))     return DIV (NUM (1), POW (CALL ("Cos", CP (u)), NUM (2)));
  if (!strcmp (f, "Cot") || !strcmp (f, "cot"))     return NEG (DIV (NUM (1), POW (CALL ("Sin", CP (u)), NUM (2))));
  if (!strcmp (f, "Exp") || !strcmp (f, "exp"))     return CALL ("Exp", CP (u));
  if (!strcmp (f, "Log") || !strcmp (f, "log"))     return DIV (NUM (1), CP (u));
  if (!strcmp (f, "Sqrt") || !strcmp (f, "sqrt"))   return DIV (NUM (1), MUL (NUM (2), CALL ("Sqrt", CP (u))));
  if (!strcmp (f, "Sinh") || !strcmp (f, "sinh"))   return CALL ("Cosh", CP (u));
  if (!strcmp (f, "Cosh") || !strcmp (f, "cosh"))   return CALL ("Sinh", CP (u));
  if (!strcmp (f, "Tanh") || !strcmp (f, "tanh"))   return SUB (NUM (1), POW (CALL ("Tanh", CP (u)), NUM (2)));
  if (!strcmp (f, "ArcTan") || !strcmp (f, "atan")) return DIV (NUM (1), ADD (NUM (1), POW (CP (u), NUM (2))));
  if (!strcmp (f, "ArcSin") || !strcmp (f, "asin")) return DIV (NUM (1), CALL ("Sqrt", SUB (NUM (1), POW (CP (u), NUM (2)))));
  if (!strcmp (f, "ArcCos") || !strcmp (f, "acos")) return NEG (DIV (NUM (1), CALL ("Sqrt", SUB (NUM (1), POW (CP (u), NUM (2))))));
  if (!strcmp (f, "Abs") || !strcmp (f, "abs"))     return CALL ("Sign", CP (u));
  if (!strcmp (f, "Erf") || !strcmp (f, "erf"))
    return DIV (MUL (NUM (2), CALL ("Exp", NEG (POW (CP (u), NUM (2))))),
                CALL ("Sqrt", m42_node_ident ("Pi")));
  if (!strcmp (f, "Erfc") || !strcmp (f, "erfc"))
    return NEG (DIV (MUL (NUM (2), CALL ("Exp", NEG (POW (CP (u), NUM (2))))),
                     CALL ("Sqrt", m42_node_ident ("Pi"))));
  return NULL;
}

M42Node *
m42_node_differentiate (const M42Node *n, const char *var)
{
  switch (n->kind)
    {
    case M42_NODE_NUMBER:
      return NUM (0);

    case M42_NODE_IDENT:
      return NUM (strcmp (n->name, var) == 0 ? 1 : 0);

    case M42_NODE_UNARY:
      if (n->op == M42_TOK_MINUS)
        {
          M42Node *d = m42_node_differentiate (m42_node_child (n, 0), var);
          return d != NULL ? NEG (d) : NULL;
        }
      return NULL;

    case M42_NODE_LIST:
      {
        M42Node *r = m42_node_new (M42_NODE_LIST);
        for (guint i = 0; i < n->children->len; i++)
          {
            M42Node *d = m42_node_differentiate (m42_node_child (n, i), var);
            if (d == NULL)
              {
                m42_node_free (r);
                return NULL;
              }
            g_ptr_array_add (r->children, d);
          }
        return r;
      }

    case M42_NODE_BINARY:
      {
        const M42Node *a = m42_node_child (n, 0), *b = m42_node_child (n, 1);
        M42Node *da, *db;

        if (n->op != M42_TOK_PLUS && n->op != M42_TOK_MINUS && n->op != M42_TOK_STAR &&
            n->op != M42_TOK_SLASH && n->op != M42_TOK_CARET)
          return NULL;

        if (n->op == M42_TOK_CARET)
          {
            if (!m42_node_depends_on (b, var))
              {
                /* b a^(b-1) a' */
                da = m42_node_differentiate (a, var);
                if (da == NULL)
                  return NULL;
                return MUL (MUL (CP (b), POW (CP (a), SUB (CP (b), NUM (1)))), da);
              }
            if (!m42_node_depends_on (a, var))
              {
                /* a^b Log[a] b' */
                db = m42_node_differentiate (b, var);
                if (db == NULL)
                  return NULL;
                return MUL (MUL (POW (CP (a), CP (b)), CALL ("Log", CP (a))), db);
              }
            /* a^b (b' Log[a] + b a'/a) */
            da = m42_node_differentiate (a, var);
            db = m42_node_differentiate (b, var);
            if (da == NULL || db == NULL)
              {
                m42_node_free (da);
                m42_node_free (db);
                return NULL;
              }
            return MUL (POW (CP (a), CP (b)),
                        ADD (MUL (db, CALL ("Log", CP (a))), DIV (MUL (CP (b), da), CP (a))));
          }

        da = m42_node_differentiate (a, var);
        db = m42_node_differentiate (b, var);
        if (da == NULL || db == NULL)
          {
            m42_node_free (da);
            m42_node_free (db);
            return NULL;
          }
        switch (n->op)
          {
          case M42_TOK_PLUS:  return ADD (da, db);
          case M42_TOK_MINUS: return SUB (da, db);
          case M42_TOK_STAR:  return ADD (MUL (da, CP (b)), MUL (CP (a), db));
          case M42_TOK_SLASH: return DIV (SUB (MUL (da, CP (b)), MUL (CP (a), db)),
                                          POW (CP (b), NUM (2)));
          default: break;
          }
        return NULL;
      }

    case M42_NODE_CALL:
      if (n->children->len == 1)
        {
          const M42Node *u = m42_node_child (n, 0);
          M42Node *outer = derivative_of_function (n->name, u);
          M42Node *du;

          if (outer == NULL)
            return NULL;
          du = m42_node_differentiate (u, var);
          if (du == NULL)
            {
              m42_node_free (outer);
              return NULL;
            }
          return MUL (outer, du);
        }
      return NULL;

    default:
      return NULL;
    }
}

/* --- integration --------------------------------------------------------
 *
 * The rules a first course teaches, and no more: the power rule, sums,
 * constant multiples, 1/(a x + b), the standard functions of a linear
 * argument, and a polynomial times one of those, by parts.  Anything
 * else returns NULL, and the caller falls back on Simpson.
 */

/* a x + b with numeric a and b, or FALSE. */
static gboolean
linear_coeffs (const M42Node *n, const char *var, double *a, double *b)
{
  double a1, b1, a2, b2;

  switch (n->kind)
    {
    case M42_NODE_NUMBER:
      *a = 0; *b = n->number;
      return TRUE;
    case M42_NODE_IDENT:
      if (strcmp (n->name, var) == 0)
        {
          *a = 1; *b = 0;
          return TRUE;
        }
      return FALSE;
    case M42_NODE_UNARY:
      if (n->op != M42_TOK_MINUS || !linear_coeffs (m42_node_child (n, 0), var, &a1, &b1))
        return FALSE;
      *a = -a1; *b = -b1;
      return TRUE;
    case M42_NODE_BINARY:
      if (!linear_coeffs (m42_node_child (n, 0), var, &a1, &b1) ||
          !linear_coeffs (m42_node_child (n, 1), var, &a2, &b2))
        return FALSE;
      switch (n->op)
        {
        case M42_TOK_PLUS:  *a = a1 + a2; *b = b1 + b2; return TRUE;
        case M42_TOK_MINUS: *a = a1 - a2; *b = b1 - b2; return TRUE;
        case M42_TOK_STAR:
          if (a1 != 0 && a2 != 0)
            return FALSE;
          *a = a1 * b2 + a2 * b1;
          *b = b1 * b2;
          return TRUE;
        case M42_TOK_SLASH:
          if (a2 != 0 || b2 == 0)
            return FALSE;
          *a = a1 / b2; *b = b1 / b2;
          return TRUE;
        default:
          return FALSE;
        }
    default:
      return FALSE;
    }
}

/* A polynomial in var with numeric coefficients: used to decide
 * whether integration by parts will terminate. */
static gboolean
is_polynomial (const M42Node *n, const char *var, int *degree)
{
  int d1, d2;

  if (!m42_node_depends_on (n, var))
    {
      /* A constant subtree counts, but only a numeric one -- another
       * symbol would make the "polynomial" a product. */
      *degree = 0;
      return n->kind == M42_NODE_NUMBER;
    }
  switch (n->kind)
    {
    case M42_NODE_IDENT:
      *degree = 1;
      return TRUE;
    case M42_NODE_UNARY:
      return n->op == M42_TOK_MINUS && is_polynomial (m42_node_child (n, 0), var, degree);
    case M42_NODE_BINARY:
      switch (n->op)
        {
        case M42_TOK_PLUS: case M42_TOK_MINUS:
          if (!is_polynomial (m42_node_child (n, 0), var, &d1) ||
              !is_polynomial (m42_node_child (n, 1), var, &d2))
            return FALSE;
          *degree = MAX (d1, d2);
          return TRUE;
        case M42_TOK_STAR:
          if (!is_polynomial (m42_node_child (n, 0), var, &d1) ||
              !is_polynomial (m42_node_child (n, 1), var, &d2))
            return FALSE;
          *degree = d1 + d2;
          return TRUE;
        case M42_TOK_SLASH:
          if (m42_node_depends_on (m42_node_child (n, 1), var) ||
              !is_polynomial (m42_node_child (n, 0), var, degree))
            return FALSE;
          return TRUE;
        case M42_TOK_CARET:
          {
            const M42Node *e = m42_node_child (n, 1);
            if (e->kind != M42_NODE_NUMBER || e->number < 0 || e->number != floor (e->number) ||
                !is_polynomial (m42_node_child (n, 0), var, &d1))
              return FALSE;
            *degree = d1 * (int) e->number;
            return TRUE;
          }
        default:
          return FALSE;
        }
    default:
      return FALSE;
    }
}

static M42Node *integrate_node (const M42Node *n, const char *var, int depth);
static gboolean poly_of (const M42Node *n, const char *var, GArray *out, int depth);
static M42Node *number_node (double x);

/* F(a x + b) divided by a, the chain rule undone for a linear inside. */
static M42Node *
over_a (M42Node *big_f, double a)
{
  if (a == 1)
    return big_f;
  return DIV (big_f, NUM (a));
}

/* Integrates a call of one argument that is linear in var. */
static M42Node *
integrate_call (const M42Node *n, const char *var)
{
  const M42Node *u;
  const char *f = n->name;
  double a, b;

  if (n->children->len != 1)
    return NULL;
  u = m42_node_child (n, 0);
  if (!m42_node_depends_on (u, var))
    return MUL (CP (n), m42_node_ident (var));

  /* The square root of a quadratic with no linear term, which is the
   * pair of forms every table has:
   *
   *   INT sqrt(c + k x^2) dx = x sqrt(c + k x^2)/2 + (c/2) INT dx/sqrt(c + k x^2)
   *
   * and what is left is an inverse sine when k is negative and a
   * logarithm when it is positive.
   */
  if (!strcmp (f, "Sqrt") || !strcmp (f, "sqrt"))
    {
      g_autoptr (GArray) coefficients = g_array_new (FALSE, TRUE, sizeof (double));

      if (poly_of (u, var, coefficients, 0) && coefficients->len == 3 &&
          fabs (g_array_index (coefficients, double, 1)) < 1e-14)
        {
          double c = g_array_index (coefficients, double, 0);
          double k = g_array_index (coefficients, double, 2);
          M42Node *root = CALL ("Sqrt", CP (u));
          M42Node *first = DIV (MUL (m42_node_ident (var), CP (root)), NUM (2));
          M42Node *rest = NULL;

          if (k < 0 && c > 0)
            rest = MUL (number_node (c / (2 * sqrt (-k))),
                        CALL ("ArcSin", MUL (number_node (sqrt (-k / c)),
                                             m42_node_ident (var))));
          else if (k > 0)
            rest = MUL (number_node (c / (2 * sqrt (k))),
                        CALL ("Log", CALL ("Abs",
                                           ADD (MUL (number_node (sqrt (k)),
                                                     m42_node_ident (var)),
                                                CP (root)))));
          m42_node_free (root);
          if (c == 0)
            {
              m42_node_free (rest);
              return first;
            }
          if (rest == NULL)
            {
              m42_node_free (first);
              return NULL;
            }
          return ADD (first, rest);
        }
    }

  if (!linear_coeffs (u, var, &a, &b) || a == 0)
    return NULL;

  if (!strcmp (f, "Sin") || !strcmp (f, "sin"))   return over_a (NEG (CALL ("Cos", CP (u))), a);
  if (!strcmp (f, "Cos") || !strcmp (f, "cos"))   return over_a (CALL ("Sin", CP (u)), a);
  if (!strcmp (f, "Tan") || !strcmp (f, "tan"))   return over_a (NEG (CALL ("Log", CALL ("Abs", CALL ("Cos", CP (u))))), a);
  if (!strcmp (f, "Cot") || !strcmp (f, "cot"))   return over_a (CALL ("Log", CALL ("Abs", CALL ("Sin", CP (u)))), a);
  if (!strcmp (f, "Exp") || !strcmp (f, "exp"))   return over_a (CALL ("Exp", CP (u)), a);
  if (!strcmp (f, "Sinh") || !strcmp (f, "sinh")) return over_a (CALL ("Cosh", CP (u)), a);
  if (!strcmp (f, "Cosh") || !strcmp (f, "cosh")) return over_a (CALL ("Sinh", CP (u)), a);
  if (!strcmp (f, "Tanh") || !strcmp (f, "tanh")) return over_a (CALL ("Log", CALL ("Cosh", CP (u))), a);
  if (!strcmp (f, "Log") || !strcmp (f, "log"))   return over_a (SUB (MUL (CP (u), CALL ("Log", CP (u))), CP (u)), a);
  if (!strcmp (f, "Sqrt") || !strcmp (f, "sqrt")) return over_a (DIV (MUL (NUM (2), POW (CP (u), DIV (NUM (3), NUM (2)))), NUM (3)), a);
  /* |u| climbs one way and falls the other, and u |u| / 2 does both. */
  if (!strcmp (f, "Abs") || !strcmp (f, "abs"))
    return over_a (DIV (MUL (CP (u), CALL ("Abs", CP (u))), NUM (2)), a);
  if (!strcmp (f, "Sign") || !strcmp (f, "sign"))
    return over_a (CALL ("Abs", CP (u)), a);
  if (!strcmp (f, "ArcTan") || !strcmp (f, "atan"))
    return over_a (SUB (MUL (CP (u), CALL ("ArcTan", CP (u))),
                        DIV (CALL ("Log", ADD (NUM (1), POW (CP (u), NUM (2)))), NUM (2))), a);
  if (!strcmp (f, "ArcSin") || !strcmp (f, "asin"))
    return over_a (ADD (MUL (CP (u), CALL ("ArcSin", CP (u))),
                        CALL ("Sqrt", SUB (NUM (1), POW (CP (u), NUM (2))))), a);
  if (!strcmp (f, "ArcCos") || !strcmp (f, "acos"))
    return over_a (SUB (MUL (CP (u), CALL ("ArcCos", CP (u))),
                        CALL ("Sqrt", SUB (NUM (1), POW (CP (u), NUM (2))))), a);
  if (!strcmp (f, "Erf") || !strcmp (f, "erf"))
    return over_a (ADD (MUL (CP (u), CALL ("Erf", CP (u))),
                        DIV (CALL ("Exp", NEG (POW (CP (u), NUM (2)))),
                             CALL ("Sqrt", m42_node_ident ("Pi")))), a);
  return NULL;
}

/* Integration by parts, over and over: the integral of p g is p G
 * minus the integral of dp G, which ends because p is a polynomial and
 * each turn lowers its degree. */
static M42Node *
integrate_by_parts (const M42Node *p, const M42Node *g, const char *var, int depth)
{
  g_autoptr (M42Node) big_g = NULL;
  g_autoptr (M42Node) dp = NULL;
  M42Node *raw_g, *raw_dp, *rest;

  if (depth > 8)
    return NULL;
  raw_g = integrate_node (g, var, depth + 1);
  if (raw_g == NULL)
    return NULL;
  big_g = m42_node_simplify (raw_g);
  m42_node_free (raw_g);

  raw_dp = m42_node_differentiate (p, var);
  if (raw_dp == NULL)
    return NULL;
  dp = m42_node_simplify (raw_dp);
  m42_node_free (raw_dp);

  if (dp->kind == M42_NODE_NUMBER && dp->number == 0)
    return MUL (CP (p), CP (big_g));

  {
    /* The next integrand is tidied before it is handed on: an
     * untidied (1/x)(x^2/2) is not recognised as the x/2 it is. */
    g_autoptr (M42Node) raw = MUL (CP (dp), CP (big_g));
    g_autoptr (M42Node) product = m42_node_simplify (raw);

    rest = integrate_node (product, var, depth + 1);
  }
  if (rest == NULL)
    return NULL;
  return SUB (MUL (CP (p), CP (big_g)), rest);
}

/* Log, ArcTan and ArcSin are the factors one differentiates rather
 * than integrates: their derivatives are algebraic, so what is left
 * after one turn of the parts is a rational function. */
static gboolean
differentiate_this_one (const M42Node *n)
{
  if (n->kind != M42_NODE_CALL || n->children->len != 1)
    return FALSE;
  return !strcmp (n->name, "Log") || !strcmp (n->name, "log") ||
         !strcmp (n->name, "ArcTan") || !strcmp (n->name, "atan") ||
         !strcmp (n->name, "ArcSin") || !strcmp (n->name, "asin") ||
         !strcmp (n->name, "ArcCos") || !strcmp (n->name, "acos");
}

static M42Node *
integrate_product (const M42Node *a, const M42Node *b, const char *var, int depth)
{
  int degree;

  if (!m42_node_depends_on (a, var))
    {
      M42Node *ib = integrate_node (b, var, depth + 1);
      return ib != NULL ? MUL (CP (a), ib) : NULL;
    }
  if (!m42_node_depends_on (b, var))
    {
      M42Node *ia = integrate_node (a, var, depth + 1);
      return ia != NULL ? MUL (CP (b), ia) : NULL;
    }
  /* x Log[x] and its kind: the logarithm is the one to differentiate,
   * since its derivative is algebraic and what is left is rational. */
  if (differentiate_this_one (a) && is_polynomial (b, var, &degree))
    {
      M42Node *r = integrate_by_parts (a, b, var, depth);

      if (r != NULL)
        return r;
    }
  if (differentiate_this_one (b) && is_polynomial (a, var, &degree))
    {
      M42Node *r = integrate_by_parts (b, a, var, depth);

      if (r != NULL)
        return r;
    }

  if (is_polynomial (a, var, &degree) && degree <= 6)
    {
      M42Node *r = integrate_by_parts (a, b, var, depth);
      if (r != NULL)
        return r;
    }
  if (is_polynomial (b, var, &degree) && degree <= 6)
    return integrate_by_parts (b, a, var, depth);
  return NULL;
}


/* --- the integrals a first course actually sets -------------------------
 *
 * Beyond the rules above: a rational function by partial fractions, the
 * two square-root forms that give an inverse sine and an inverse
 * hyperbolic sine, an exponential times a sine or a cosine (the pair
 * that comes back to itself after two integrations by parts), the
 * squares of the sine and cosine through the double-angle formula, and
 * the two substitution patterns that carry most of the rest:
 * f'/f and f^n f'.
 */

/* A number as a node, written as a fraction when it is a simple one,
 * so that an integral reads ArcTan[x/2]/2 and not 0.5 ArcTan[x/2]. */
static M42Node *
number_node (double x)
{
  if (x == floor (x) || !isfinite (x))
    return NUM (x);
  for (int den = 2; den <= 64; den++)
    {
      double scaled = x * den;
      if (fabs (scaled - round (scaled)) < 1e-12 * MAX (1.0, fabs (scaled)))
        return DIV (NUM (round (scaled)), NUM (den));
    }
  return NUM (x);
}

/* The coefficients of a polynomial in var, lowest power first, or
 * FALSE.  Only numbers count as coefficients. */
static gboolean
poly_of (const M42Node *n, const char *var, GArray *out, int depth)
{
  if (depth > 16)
    return FALSE;

  switch (n->kind)
    {
    case M42_NODE_NUMBER:
      g_array_set_size (out, 1);
      g_array_index (out, double, 0) = n->number;
      return TRUE;

    case M42_NODE_IDENT:
      if (strcmp (n->name, var) != 0)
        return FALSE;
      g_array_set_size (out, 2);
      g_array_index (out, double, 0) = 0;
      g_array_index (out, double, 1) = 1;
      return TRUE;

    case M42_NODE_UNARY:
      if (n->op != M42_TOK_MINUS || !poly_of (m42_node_child (n, 0), var, out, depth + 1))
        return FALSE;
      for (guint i = 0; i < out->len; i++)
        g_array_index (out, double, i) = -g_array_index (out, double, i);
      return TRUE;

    case M42_NODE_BINARY:
      {
        g_autoptr (GArray) a = g_array_new (FALSE, TRUE, sizeof (double));
        g_autoptr (GArray) b = g_array_new (FALSE, TRUE, sizeof (double));

        if (n->op == M42_TOK_CARET)
          {
            const M42Node *e = m42_node_child (n, 1);
            int k;

            if (e->kind != M42_NODE_NUMBER || e->number < 0 || e->number > 12 ||
                e->number != floor (e->number) ||
                !poly_of (m42_node_child (n, 0), var, a, depth + 1))
              return FALSE;
            k = (int) e->number;
            g_array_set_size (out, 1);
            g_array_index (out, double, 0) = 1;
            for (int t = 0; t < k; t++)
              {
                g_autoptr (GArray) acc = g_array_new (FALSE, TRUE, sizeof (double));
                g_array_set_size (acc, out->len + a->len - 1);
                for (guint i = 0; i < out->len; i++)
                  for (guint j = 0; j < a->len; j++)
                    g_array_index (acc, double, i + j) +=
                      g_array_index (out, double, i) * g_array_index (a, double, j);
                g_array_set_size (out, acc->len);
                memcpy (out->data, acc->data, sizeof (double) * acc->len);
              }
            return TRUE;
          }

        if (!poly_of (m42_node_child (n, 0), var, a, depth + 1) ||
            !poly_of (m42_node_child (n, 1), var, b, depth + 1))
          return FALSE;

        switch (n->op)
          {
          case M42_TOK_PLUS: case M42_TOK_MINUS:
            {
              guint len = MAX (a->len, b->len);
              g_array_set_size (out, len);
              for (guint i = 0; i < len; i++)
                {
                  double x = i < a->len ? g_array_index (a, double, i) : 0;
                  double y = i < b->len ? g_array_index (b, double, i) : 0;
                  g_array_index (out, double, i) = n->op == M42_TOK_PLUS ? x + y : x - y;
                }
              return TRUE;
            }
          case M42_TOK_STAR:
            g_array_set_size (out, a->len + b->len - 1);
            memset (out->data, 0, sizeof (double) * out->len);
            for (guint i = 0; i < a->len; i++)
              for (guint j = 0; j < b->len; j++)
                g_array_index (out, double, i + j) +=
                  g_array_index (a, double, i) * g_array_index (b, double, j);
            return TRUE;
          case M42_TOK_SLASH:
            if (b->len != 1 || g_array_index (b, double, 0) == 0)
              return FALSE;
            g_array_set_size (out, a->len);
            for (guint i = 0; i < a->len; i++)
              g_array_index (out, double, i) = g_array_index (a, double, i) /
                                               g_array_index (b, double, 0);
            return TRUE;
          default:
            return FALSE;
          }
      }

    default:
      return FALSE;
    }
}

static void
poly_trim (GArray *p)
{
  while (p->len > 1 && fabs (g_array_index (p, double, p->len - 1)) < 1e-12)
    g_array_set_size (p, p->len - 1);
}

/* --- a polynomial whose coefficients are letters -------------------------
 *
 * poly_of above wants numbers, which is what a root finder needs.  To
 * solve a x^2 + b x + c == 0 with a, b and c left as letters we need
 * the same coefficients as expressions, so here they are.
 */

/* out[i] gets what is already there plus this, and the array is made
 * long enough on the way.  It takes the node. */
static void
term_add (GPtrArray *out, guint power, M42Node *what)
{
  while (out->len <= power)
    g_ptr_array_add (out, NUM (0));
  g_ptr_array_index (out, power) = ADD (g_ptr_array_index (out, power), what);
}

static GPtrArray *
terms_new (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) m42_node_free);
}

static gboolean
terms_of (const M42Node *n, const char *var, GPtrArray *out, int depth)
{
  if (depth > 16)
    return FALSE;

  /* Anything the variable is not in is a coefficient on its own. */
  if (!m42_node_depends_on (n, var))
    {
      term_add (out, 0, CP (n));
      return TRUE;
    }

  switch (n->kind)
    {
    case M42_NODE_IDENT:
      term_add (out, 1, NUM (1));       /* it depends on var and is a name */
      return TRUE;

    case M42_NODE_UNARY:
      {
        g_autoptr (GPtrArray) a = terms_new ();

        if (n->op != M42_TOK_MINUS || !terms_of (m42_node_child (n, 0), var, a, depth + 1))
          return FALSE;
        for (guint i = 0; i < a->len; i++)
          term_add (out, i, NEG (CP (g_ptr_array_index (a, i))));
        return TRUE;
      }

    case M42_NODE_BINARY:
      switch (n->op)
        {
        case M42_TOK_PLUS:
        case M42_TOK_MINUS:
          {
            g_autoptr (GPtrArray) a = terms_new ();
            g_autoptr (GPtrArray) b = terms_new ();

            if (!terms_of (m42_node_child (n, 0), var, a, depth + 1) ||
                !terms_of (m42_node_child (n, 1), var, b, depth + 1))
              return FALSE;
            for (guint i = 0; i < a->len; i++)
              term_add (out, i, CP (g_ptr_array_index (a, i)));
            for (guint i = 0; i < b->len; i++)
              term_add (out, i, n->op == M42_TOK_PLUS ? CP (g_ptr_array_index (b, i))
                                                      : NEG (CP (g_ptr_array_index (b, i))));
            return TRUE;
          }

        case M42_TOK_STAR:
          {
            g_autoptr (GPtrArray) a = terms_new ();
            g_autoptr (GPtrArray) b = terms_new ();

            if (!terms_of (m42_node_child (n, 0), var, a, depth + 1) ||
                !terms_of (m42_node_child (n, 1), var, b, depth + 1))
              return FALSE;
            for (guint i = 0; i < a->len; i++)
              for (guint k = 0; k < b->len; k++)
                term_add (out, i + k, MUL (CP (g_ptr_array_index (a, i)),
                                           CP (g_ptr_array_index (b, k))));
            return TRUE;
          }

        case M42_TOK_SLASH:
          {
            g_autoptr (GPtrArray) a = terms_new ();

            /* Dividing by the variable would take us out of the
             * polynomials, so only a coefficient may be underneath. */
            if (m42_node_depends_on (m42_node_child (n, 1), var) ||
                !terms_of (m42_node_child (n, 0), var, a, depth + 1))
              return FALSE;
            for (guint i = 0; i < a->len; i++)
              term_add (out, i, DIV (CP (g_ptr_array_index (a, i)),
                                     CP (m42_node_child (n, 1))));
            return TRUE;
          }

        case M42_TOK_CARET:
          {
            const M42Node *e = m42_node_child (n, 1);
            g_autoptr (GPtrArray) base = terms_new ();
            g_autoptr (GPtrArray) acc = terms_new ();
            int power;

            if (e->kind != M42_NODE_NUMBER || e->number != floor (e->number) ||
                e->number < 0 || e->number > 8)
              return FALSE;
            power = (int) e->number;
            if (!terms_of (m42_node_child (n, 0), var, base, depth + 1))
              return FALSE;
            term_add (acc, 0, NUM (1));
            for (int t = 0; t < power; t++)
              {
                g_autoptr (GPtrArray) next = terms_new ();

                for (guint i = 0; i < acc->len; i++)
                  for (guint k = 0; k < base->len; k++)
                    term_add (next, i + k, MUL (CP (g_ptr_array_index (acc, i)),
                                                CP (g_ptr_array_index (base, k))));
                g_ptr_array_set_size (acc, 0);
                for (guint i = 0; i < next->len; i++)
                  term_add (acc, i, CP (g_ptr_array_index (next, i)));
              }
            for (guint i = 0; i < acc->len; i++)
              term_add (out, i, CP (g_ptr_array_index (acc, i)));
            return TRUE;
          }

        default:
          return FALSE;
        }

    default:
      return FALSE;
    }
}

GPtrArray *
m42_node_poly_terms (const M42Node *n, const char *var)
{
  GPtrArray *raw = terms_new ();
  GPtrArray *out;

  if (!terms_of (n, var, raw, 0))
    {
      g_ptr_array_unref (raw);
      return NULL;
    }
  out = terms_new ();
  for (guint i = 0; i < raw->len; i++)
    g_ptr_array_add (out, m42_node_simplify (g_ptr_array_index (raw, i)));
  g_ptr_array_unref (raw);

  /* A leading zero is not a degree. */
  while (out->len > 1)
    {
      const M42Node *top = g_ptr_array_index (out, out->len - 1);

      if (top->kind == M42_NODE_NUMBER && top->number == 0)
        g_ptr_array_set_size (out, out->len - 1);
      else
        break;
    }
  return out;
}

/* A polynomial as a tree, lowest power first in the array. */
static M42Node *
poly_node (const GArray *p, const char *var)
{
  M42Node *out = NULL;

  for (guint i = 0; i < p->len; i++)
    {
      double c = g_array_index (p, double, i);
      M42Node *term;

      if (fabs (c) < 1e-14)
        continue;
      if (i == 0)
        term = NUM (c);
      else
        {
          M42Node *power = i == 1 ? m42_node_ident (var)
                                  : POW (m42_node_ident (var), NUM (i));
          term = c == 1 ? power : MUL (NUM (c), power);
        }
      out = out == NULL ? term : ADD (out, term);
    }
  return out != NULL ? out : NUM (0);
}

/* Long division: p = q d + r, with the remainder lower in degree. */
static void
poly_divide (const GArray *p, const GArray *d, GArray *q, GArray *r)
{
  g_array_set_size (r, p->len);
  memcpy (r->data, p->data, sizeof (double) * p->len);
  g_array_set_size (q, 1);
  g_array_index (q, double, 0) = 0;

  if (d->len < 1 || p->len < d->len)
    return;

  g_array_set_size (q, p->len - d->len + 1);
  memset (q->data, 0, sizeof (double) * q->len);

  for (int i = (int) p->len - (int) d->len; i >= 0; i--)
    {
      double factor = g_array_index (r, double, i + d->len - 1) /
                      g_array_index (d, double, d->len - 1);

      g_array_index (q, double, i) = factor;
      for (guint j = 0; j < d->len; j++)
        g_array_index (r, double, i + j) -= factor * g_array_index (d, double, j);
    }
  poly_trim (r);
}

/* The integral of (px + q) / (a x^2 + b x + c), whichever of the three
 * shapes the quadratic has.  This is the heart of partial fractions. */
static M42Node *
integrate_over_quadratic (double p, double q, double a, double b, double c, const char *var)
{
  double disc = b * b - 4 * a * c;
  M42Node *x = m42_node_ident (var);

  if (fabs (a) < 1e-14)
    {
      /* Not really a quadratic: (p x + q)/(b x + c). */
      if (fabs (b) < 1e-14)
        return fabs (c) < 1e-14 ? NULL
               : ADD (DIV (MUL (NUM (p / (2 * c)), POW (CP (x), NUM (2))), NUM (1)),
                      MUL (NUM (q / c), CP (x)));
      {
        /* p/b x + (q - p c / b)/b log|b x + c| */
        M42Node *linear = ADD (MUL (NUM (b), CP (x)), NUM (c));
        M42Node *first = MUL (number_node (p / b), CP (x));
        M42Node *second = MUL (number_node ((q - p * c / b) / b),
                               CALL ("Log", CALL ("Abs", linear)));
        m42_node_free (x);
        return ADD (first, second);
      }
    }

  if (disc > 1e-12)
    {
      /* Two real roots: two logarithms. */
      double r1 = (-b + sqrt (disc)) / (2 * a);
      double r2 = (-b - sqrt (disc)) / (2 * a);
      double a1 = (p * r1 + q) / (a * (r1 - r2));
      double a2 = (p * r2 + q) / (a * (r2 - r1));
      M42Node *term1 = MUL (number_node (a1), CALL ("Log", CALL ("Abs", SUB (CP (x), number_node (r1)))));
      M42Node *term2 = MUL (number_node (a2), CALL ("Log", CALL ("Abs", SUB (CP (x), number_node (r2)))));

      m42_node_free (x);
      return ADD (term1, term2);
    }

  if (fabs (disc) <= 1e-12)
    {
      /* A double root: a logarithm and a reciprocal. */
      double r = -b / (2 * a);
      M42Node *shifted = SUB (CP (x), number_node (r));
      M42Node *term1 = MUL (number_node (p / a), CALL ("Log", CALL ("Abs", CP (shifted))));
      M42Node *term2 = NEG (DIV (number_node ((p * r + q) / a), shifted));

      m42_node_free (x);
      return ADD (term1, term2);
    }

  {
    /* Complex roots: a logarithm and an inverse tangent. */
    double root = sqrt (-disc);
    M42Node *quad = ADD (ADD (MUL (NUM (a), POW (CP (x), NUM (2))), MUL (NUM (b), CP (x))),
                         NUM (c));
    M42Node *inner = DIV (ADD (MUL (NUM (2 * a), CP (x)), NUM (b)), NUM (root));
    M42Node *term1 = MUL (number_node (p / (2 * a)), CALL ("Log", CALL ("Abs", quad)));
    M42Node *term2 = MUL (number_node ((2 * a * q - b * p) / (a * root)), CALL ("ArcTan", inner));

    m42_node_free (x);
    return ADD (term1, term2);
  }
}

/* A quotient of two polynomials, integrated: divide out the whole part
 * and deal with what is left over. */
static M42Node *
integrate_rational (const M42Node *num, const M42Node *den, const char *var, int depth)
{
  g_autoptr (GArray) p = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) d = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) q = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) r = g_array_new (FALSE, TRUE, sizeof (double));
  M42Node *whole = NULL, *rest = NULL;

  if (!poly_of (num, var, p, 0) || !poly_of (den, var, d, 0))
    return NULL;
  poly_trim (p);
  poly_trim (d);
  if (d->len < 2 || d->len > 3)
    return NULL;                 /* linear and quadratic denominators */

  poly_divide (p, d, q, r);

  if (q->len > 1 || fabs (g_array_index (q, double, 0)) > 1e-14)
    {
      g_autoptr (M42Node) quotient = poly_node (q, var);
      whole = integrate_node (quotient, var, depth + 1);
      if (whole == NULL)
        return NULL;
    }

  {
    double rp = r->len > 1 ? g_array_index (r, double, 1) : 0;
    double rq = r->len > 0 ? g_array_index (r, double, 0) : 0;
    double da = d->len > 2 ? g_array_index (d, double, 2) : 0;
    double db = d->len > 1 ? g_array_index (d, double, 1) : 0;
    double dc = g_array_index (d, double, 0);

    if (fabs (rp) > 1e-14 || fabs (rq) > 1e-14)
      {
        rest = integrate_over_quadratic (rp, rq, da, db, dc, var);
        if (rest == NULL)
          {
            m42_node_free (whole);
            return NULL;
          }
      }
  }

  if (whole == NULL)
    return rest;
  if (rest == NULL)
    return whole;
  return ADD (whole, rest);
}

/* a x + b, as a pair, when the tree is that. */
static gboolean
linear_parts (const M42Node *n, const char *var, double *a, double *b)
{
  return linear_coeffs (n, var, a, b);
}

/* The two square roots that give an inverse sine and its hyperbolic
 * cousin: 1/Sqrt[k - x^2] and 1/Sqrt[k + x^2]. */
static M42Node *
integrate_inverse_root (const M42Node *den, const char *var)
{
  g_autoptr (GArray) inside = g_array_new (FALSE, TRUE, sizeof (double));
  double c0, c1, c2;

  if (den->kind != M42_NODE_CALL || den->children->len != 1 ||
      (strcmp (den->name, "Sqrt") != 0 && strcmp (den->name, "sqrt") != 0))
    return NULL;
  if (!poly_of (m42_node_child (den, 0), var, inside, 0))
    return NULL;
  poly_trim (inside);
  if (inside->len != 3)
    return NULL;

  c0 = g_array_index (inside, double, 0);
  c1 = g_array_index (inside, double, 1);
  c2 = g_array_index (inside, double, 2);
  if (fabs (c1) > 1e-12 || c0 <= 0)
    return NULL;

  if (fabs (c2 + 1) < 1e-12)      /* k - x^2 */
    return CALL ("ArcSin", DIV (m42_node_ident (var), NUM (sqrt (c0))));
  if (fabs (c2 - 1) < 1e-12)      /* k + x^2 */
    return CALL ("Log", ADD (m42_node_ident (var),
                             CALL ("Sqrt", ADD (POW (m42_node_ident (var), NUM (2)),
                                                NUM (c0)))));
  return NULL;
}

/* Exp[a x] Sin[b x] and Exp[a x] Cos[b x], the pair that comes back to
 * itself after two integrations by parts. */
static M42Node *
integrate_exp_times_trig (const M42Node *first, const M42Node *second, const char *var)
{
  const M42Node *e = NULL, *t = NULL;
  double a, b, junk;
  gboolean is_sine;
  M42Node *inside;

  for (int swap = 0; swap < 2; swap++)
    {
      const M42Node *x = swap ? second : first;
      const M42Node *y = swap ? first : second;

      if (x->kind == M42_NODE_CALL && x->children->len == 1 &&
          (!strcmp (x->name, "Exp") || !strcmp (x->name, "exp")) &&
          y->kind == M42_NODE_CALL && y->children->len == 1 &&
          (!strcmp (y->name, "Sin") || !strcmp (y->name, "sin") ||
           !strcmp (y->name, "Cos") || !strcmp (y->name, "cos")))
        {
          e = x;
          t = y;
          break;
        }
    }
  if (e == NULL)
    return NULL;
  if (!linear_parts (m42_node_child (e, 0), var, &a, &junk) || fabs (junk) > 1e-12 ||
      !linear_parts (m42_node_child (t, 0), var, &b, &junk) || fabs (junk) > 1e-12)
    return NULL;
  if (fabs (a * a + b * b) < 1e-14)
    return NULL;

  is_sine = t->name[0] == 'S' || t->name[0] == 's';
  inside = MUL (NUM (b), m42_node_ident (var));

  {
    M42Node *exp_part = CALL ("Exp", MUL (NUM (a), m42_node_ident (var)));
    M42Node *sine = CALL ("Sin", CP (inside));
    M42Node *cosine = CALL ("Cos", inside);
    M42Node *bracket = is_sine
      ? SUB (MUL (NUM (a), sine), MUL (NUM (b), cosine))
      : ADD (MUL (NUM (a), cosine), MUL (NUM (b), sine));

    return DIV (MUL (exp_part, bracket), NUM (a * a + b * b));
  }
}

/* Sin[a x]^2 and Cos[a x]^2, through the double angle. */
static M42Node *
integrate_trig_square (const M42Node *base, double power, const char *var)
{
  double a, b;
  gboolean is_sine;

  if (fabs (power - 2) > 1e-12 || base->kind != M42_NODE_CALL || base->children->len != 1)
    return NULL;
  is_sine = !strcmp (base->name, "Sin") || !strcmp (base->name, "sin");
  if (!is_sine && strcmp (base->name, "Cos") != 0 && strcmp (base->name, "cos") != 0)
    return NULL;
  if (!linear_parts (m42_node_child (base, 0), var, &a, &b) || fabs (b) > 1e-12 ||
      fabs (a) < 1e-14)
    return NULL;

  {
    M42Node *half = DIV (m42_node_ident (var), NUM (2));
    M42Node *wave = DIV (CALL ("Sin", MUL (NUM (2 * a), m42_node_ident (var))), NUM (4 * a));

    return is_sine ? SUB (half, wave) : ADD (half, wave);
  }
}

/* --- powers of the trigonometric functions -------------------------------
 *
 * The reduction every table has, which takes two off the power at a
 * time until nothing is left but the power one or nothing at all:
 *
 *   INT sin^n u du = -sin^(n-1)u cos u / n + (n-1)/n INT sin^(n-2)u du
 *   INT cos^n u du =  cos^(n-1)u sin u / n + (n-1)/n INT cos^(n-2)u du
 *   INT tan^n u du =  tan^(n-1)u / (n-1)          -  INT tan^(n-2)u du
 *
 * The inside has to be linear in the variable, so that du is a number
 * times dx and the whole answer is divided by that number at the end.
 */

enum { TRIG_SIN, TRIG_COS, TRIG_TAN };

/* The antiderivative with respect to u itself. */
static M42Node *
trig_power_in_u (int which, int k, const M42Node *u)
{
  const char *name = which == TRIG_SIN ? "Sin" : which == TRIG_COS ? "Cos" : "Tan";

  if (which == TRIG_TAN)
    {
      if (k == 0)
        return CP (u);
      if (k == 1)
        return NEG (CALL ("Log", CALL ("Abs", CALL ("Cos", CP (u)))));
      return SUB (DIV (POW (CALL (name, CP (u)), NUM (k - 1)), NUM (k - 1)),
                  trig_power_in_u (which, k - 2, u));
    }

  if (k == 0)
    return CP (u);
  if (k == 1)
    return which == TRIG_SIN ? NEG (CALL ("Cos", CP (u))) : CALL ("Sin", CP (u));

  {
    M42Node *edge =
      which == TRIG_SIN
        ? NEG (DIV (MUL (POW (CALL ("Sin", CP (u)), NUM (k - 1)), CALL ("Cos", CP (u))),
                    NUM (k)))
        : DIV (MUL (POW (CALL ("Cos", CP (u)), NUM (k - 1)), CALL ("Sin", CP (u))),
               NUM (k));

    return ADD (edge, MUL (DIV (NUM (k - 1), NUM (k)), trig_power_in_u (which, k - 2, u)));
  }
}

static M42Node *
integrate_trig_power (const M42Node *base, double power, const char *var)
{
  const M42Node *u;
  double a, b;
  int which;

  if (base->kind != M42_NODE_CALL || base->children->len != 1 ||
      power != floor (power))
    return NULL;
  if (!strcmp (base->name, "Sin") || !strcmp (base->name, "sin"))
    which = TRIG_SIN;
  else if (!strcmp (base->name, "Cos") || !strcmp (base->name, "cos"))
    which = TRIG_COS;
  else if (!strcmp (base->name, "Tan") || !strcmp (base->name, "tan"))
    which = TRIG_TAN;
  else
    return NULL;

  u = m42_node_child (base, 0);
  if (!linear_coeffs (u, var, &a, &b) || fabs (a) < 1e-14)
    return NULL;

  /* One over a square: the two that are worth a rule of their own. */
  if (power == -2)
    {
      M42Node *answer;

      if (which == TRIG_COS)
        answer = CALL ("Tan", CP (u));
      else if (which == TRIG_SIN)
        answer = NEG (DIV (CALL ("Cos", CP (u)), CALL ("Sin", CP (u))));
      else
        return NULL;
      return a == 1 ? answer : DIV (answer, NUM (a));
    }

  if (power < 2 || power > 12)
    return NULL;
  {
    M42Node *answer = trig_power_in_u (which, (int) power, u);

    return a == 1 ? answer : DIV (answer, NUM (a));
  }
}

/* A number in front, taken off: -Sin[x] is (-1) Sin[x] and 2 x is 2
 * times x.  What comes back belongs to the node it came from.  This is
 * what lets a substitution see that Sin[x] and the -Sin[x] it wants
 * are the same thing but for a factor. */
static const M42Node *
without_number (const M42Node *n, double *factor)
{
  *factor = 1;
  if (n->kind == M42_NODE_UNARY && n->op == M42_TOK_MINUS)
    {
      *factor = -1;
      n = m42_node_child (n, 0);
    }
  if (n->kind == M42_NODE_BINARY && n->op == M42_TOK_STAR &&
      m42_node_child (n, 0)->kind == M42_NODE_NUMBER)
    {
      *factor *= m42_node_child (n, 0)->number;
      n = m42_node_child (n, 1);
    }
  else if (n->kind == M42_NODE_NUMBER)
    {
      *factor *= n->number;
      return NULL;                 /* nothing left but the number */
    }
  return n;
}

/* Whether two expressions are the same but for a number in front, and
 * what that number is: have = factor * want. */
static gboolean
same_but_for_a_number (const M42Node *want, const M42Node *have, double *factor)
{
  double wf, hf;
  const M42Node *w = without_number (want, &wf);
  const M42Node *h = without_number (have, &hf);
  g_autoptr (GString) sw = g_string_new (NULL);
  g_autoptr (GString) sh = g_string_new (NULL);

  if (w == NULL || h == NULL || wf == 0)
    return FALSE;
  m42_node_to_string (sw, w);
  m42_node_to_string (sh, h);
  if (strcmp (sw->str, sh->str) != 0)
    return FALSE;
  *factor = hf / wf;
  return TRUE;
}

/* The substitution a first course leans on: an integrand that is a
 * function of something times the derivative of that something.  The
 * inner function is integrated on its own against a fresh name and the
 * something put back in, so x Exp[x^2] becomes Exp[x^2]/2. */
static M42Node *
integrate_by_substitution (const M42Node *a, const M42Node *b, const char *var, int depth)
{
  if (depth > 6)
    return NULL;

  for (int swap = 0; swap < 2; swap++)
    {
      const M42Node *outer = swap ? b : a;
      const M42Node *rest = swap ? a : b;
      const M42Node *inner;
      g_autoptr (M42Node) derivative = NULL;
      g_autoptr (M42Node) want = NULL;
      g_autoptr (M42Node) have = NULL;
      double factor = 1;

      if (outer->kind != M42_NODE_CALL || outer->children->len != 1)
        continue;
      inner = m42_node_child (outer, 0);
      if (!m42_node_depends_on (inner, var))
        continue;
      /* A linear inside is already handled, and better, above. */
      {
        double la, lb;
        if (linear_coeffs (inner, var, &la, &lb))
          continue;
      }

      derivative = m42_node_differentiate (inner, var);
      if (derivative == NULL)
        continue;
      want = m42_node_simplify (derivative);
      have = m42_node_simplify (rest);

      /* Either side may carry a number in front of it -- 2x against x,
       * or Sin[x] against the -Sin[x] the derivative gave. */
      if (!same_but_for_a_number (want, have, &factor))
        continue;

      /* Integrate the outer function against a name of its own. */
      {
        g_autoptr (M42Node) against = m42_node_call1 (outer->name, m42_node_ident ("$u"));
        M42Node *raw = integrate_node (against, "$u", depth + 1);
        M42Node *back;

        if (raw == NULL)
          continue;
        back = m42_node_substitute (raw, "$u", inner);
        m42_node_free (raw);
        return factor == 1 ? back : MUL (number_node (factor), back);
      }
    }
  return NULL;
}

/* f'/f, whose integral is the logarithm of f, and f^n f', whose
 * integral is the next power up. */
static M42Node *
integrate_by_pattern (const M42Node *n, const char *var, int depth)
{
  if (depth > 6)
    return NULL;

  /* A quotient whose top is the derivative of its bottom. */
  if (n->kind == M42_NODE_BINARY && n->op == M42_TOK_SLASH)
    {
      const M42Node *top = m42_node_child (n, 0), *bottom = m42_node_child (n, 1);
      g_autoptr (M42Node) d = m42_node_differentiate (bottom, var);
      g_autoptr (M42Node) want = NULL;
      g_autoptr (M42Node) have = NULL;
      double factor;

      if (d == NULL)
        return NULL;
      want = m42_node_simplify (d);
      have = m42_node_simplify (top);
      if (same_but_for_a_number (want, have, &factor))
        {
          M42Node *log = CALL ("Log", CALL ("Abs", CP (bottom)));

          return factor == 1 ? log : MUL (number_node (factor), log);
        }
      return NULL;
    }

  /* A product with a power in it: f^k f'. */
  if (n->kind == M42_NODE_BINARY && n->op == M42_TOK_STAR)
    {
      for (int swap = 0; swap < 2; swap++)
        {
          const M42Node *a = m42_node_child (n, swap ? 1 : 0);
          const M42Node *b = m42_node_child (n, swap ? 0 : 1);
          const M42Node *base = a;
          double k = 1;

          if (a->kind == M42_NODE_BINARY && a->op == M42_TOK_CARET &&
              m42_node_child (a, 1)->kind == M42_NODE_NUMBER)
            {
              base = m42_node_child (a, 0);
              k = m42_node_child (a, 1)->number;
            }
          if (k == -1 || !m42_node_depends_on (base, var))
            continue;

          {
            g_autoptr (M42Node) d = m42_node_differentiate (base, var);
            g_autoptr (M42Node) want = NULL;
            g_autoptr (M42Node) have = NULL;
            double factor = 1;

            if (d == NULL)
              continue;
            want = m42_node_simplify (d);
            have = m42_node_simplify (b);

            /* b may be a number times f', the minus sign of a
             * derivative included: Cos[x]^2 Sin[x] is -f^2 f'. */
            if (same_but_for_a_number (want, have, &factor))
              {
                M42Node *raised = POW (CP (base), NUM (k + 1));

                return MUL (number_node (factor / (k + 1)), raised);
              }
          }
        }
    }
  return NULL;
}

static M42Node *
integrate_node (const M42Node *n, const char *var, int depth)
{
  if (depth > 12)
    return NULL;

  if (!m42_node_depends_on (n, var))
    return MUL (CP (n), m42_node_ident (var));

  /* The Gaussian: INT e^(A x^2 + B x + C) dx, which is what the error
   * function is for.  Completing the square turns it into
   *   e^(C + B^2/4a) sqrt(pi/a)/2 Erf(sqrt(a) (x - B/2a)),  a = -A > 0,
   * and with A = -1 and B = 0 that is the sqrt(pi)/2 Erf(x) of every
   * table.  Written this way there is no rule to look up: it falls out
   * of the substitution. */
  if (n->kind == M42_NODE_CALL && n->children->len == 1 &&
      (!strcmp (n->name, "Exp") || !strcmp (n->name, "exp")))
    {
      GArray *coefficients = g_array_new (FALSE, FALSE, sizeof (double));
      M42Node *answer = NULL;

      if (m42_node_polynomial (m42_node_child (n, 0), var, coefficients) &&
          coefficients->len == 3)
        {
          double c = g_array_index (coefficients, double, 0);
          double b = g_array_index (coefficients, double, 1);
          double a = -g_array_index (coefficients, double, 2);

          if (a > 0)
            {
              M42Node *shifted = SUB (m42_node_ident (var), NUM (b / (2 * a)));

              answer = MUL (MUL (NUM (exp (c + b * b / (4 * a))),
                                 DIV (CALL ("Sqrt", DIV (m42_node_ident ("Pi"), NUM (a))),
                                      NUM (2))),
                            CALL ("Erf", MUL (NUM (sqrt (a)), shifted)));
            }
        }
      g_array_free (coefficients, TRUE);
      if (answer != NULL)
        return answer;
    }

  switch (n->kind)
    {
    case M42_NODE_IDENT:      /* x -> x^2/2 */
      return DIV (POW (m42_node_ident (var), NUM (2)), NUM (2));

    case M42_NODE_UNARY:
      if (n->op == M42_TOK_MINUS)
        {
          M42Node *i = integrate_node (m42_node_child (n, 0), var, depth + 1);
          return i != NULL ? NEG (i) : NULL;
        }
      return NULL;

    case M42_NODE_LIST:
      {
        M42Node *r = m42_node_new (M42_NODE_LIST);
        for (guint i = 0; i < n->children->len; i++)
          {
            M42Node *c = integrate_node (m42_node_child (n, i), var, depth + 1);
            if (c == NULL)
              {
                m42_node_free (r);
                return NULL;
              }
            g_ptr_array_add (r->children, c);
          }
        return r;
      }

    case M42_NODE_BINARY:
      {
        const M42Node *a = m42_node_child (n, 0), *b = m42_node_child (n, 1);
        double la, lb;

        switch (n->op)
          {
          case M42_TOK_PLUS: case M42_TOK_MINUS:
            {
              M42Node *ia = integrate_node (a, var, depth + 1);
              M42Node *ib = ia != NULL ? integrate_node (b, var, depth + 1) : NULL;
              if (ib == NULL)
                {
                  m42_node_free (ia);
                  return NULL;
                }
              return m42_node_binary (n->op, ia, ib);
            }

          case M42_TOK_STAR:
            {
              M42Node *r = integrate_exp_times_trig (a, b, var);

              if (r == NULL)
                r = integrate_by_pattern (n, var, depth);
              if (r == NULL)
                r = integrate_by_substitution (a, b, var, depth);
              if (r != NULL)
                return r;
            }
            return integrate_product (a, b, var, depth);

          case M42_TOK_SLASH:
            if (!m42_node_depends_on (b, var))
              {
                M42Node *ia = integrate_node (a, var, depth + 1);
                return ia != NULL ? DIV (ia, CP (b)) : NULL;
              }
            /* f'/f, a rational function, or one over a square root --
             * and, failing those, the quotient looked at as the
             * product a (1/b), which is where Log[x]/x is caught. */
            {
              M42Node *r = integrate_by_pattern (n, var, depth);

              if (r == NULL)
                {
                  g_autoptr (M42Node) product =
                    MUL (CP (a), DIV (NUM (1), CP (b)));

                  r = integrate_by_pattern (product, var, depth);
                  if (r == NULL)
                    r = integrate_by_substitution (a, m42_node_child (product, 1), var, depth);
                }

              if (r == NULL)
                r = integrate_rational (a, b, var, depth);
              if (r == NULL && !m42_node_depends_on (a, var))
                {
                  M42Node *root = integrate_inverse_root (b, var);
                  if (root != NULL)
                    r = a->kind == M42_NODE_NUMBER && a->number == 1
                        ? root : MUL (CP (a), root);
                }
              if (r != NULL)
                return r;
            }
            /* c/(a x + b) */
            if (!m42_node_depends_on (a, var) && linear_coeffs (b, var, &la, &lb) && la != 0)
              return DIV (MUL (CP (a), CALL ("Log", CALL ("Abs", CP (b)))), NUM (la));
            /* c over a power: as a negative power. */
            if (!m42_node_depends_on (a, var) && b->kind == M42_NODE_BINARY &&
                b->op == M42_TOK_CARET && m42_node_child (b, 1)->kind == M42_NODE_NUMBER)
              {
                g_autoptr (M42Node) inv = POW (CP (m42_node_child (b, 0)),
                                               NUM (-m42_node_child (b, 1)->number));
                M42Node *i = integrate_node (inv, var, depth + 1);
                return i != NULL ? MUL (CP (a), i) : NULL;
              }
            /* One over a product, split one factor at a time: it is
             * what turns 1/(x Log[x]) into the (1/x)/Log[x] whose
             * integral is a logarithm of a logarithm. */
            if (b->kind == M42_NODE_BINARY && b->op == M42_TOK_STAR && depth < 6)
              for (int side = 0; side < 2; side++)
                {
                  g_autoptr (M42Node) split =
                    DIV (DIV (CP (a), CP (m42_node_child (b, side))),
                         CP (m42_node_child (b, 1 - side)));
                  M42Node *r = integrate_node (split, var, depth + 1);

                  if (r != NULL)
                    return r;
                }
            return NULL;

          case M42_TOK_CARET:
            /* Sin[a x]^2 and Cos[a x]^2 through the double angle, and
             * any other whole power by the reduction. */
            if (b->kind == M42_NODE_NUMBER)
              {
                M42Node *r = integrate_trig_square (a, b->number, var);

                if (r == NULL)
                  r = integrate_trig_power (a, b->number, var);
                if (r != NULL)
                  return r;
              }
            /* (a x + b)^k */
            if (b->kind == M42_NODE_NUMBER && linear_coeffs (a, var, &la, &lb) && la != 0)
              {
                double k = b->number;
                if (k == -1)
                  return DIV (CALL ("Log", CALL ("Abs", CP (a))), NUM (la));
                return DIV (POW (CP (a), NUM (k + 1)), NUM (la * (k + 1)));
              }
            /* c^(a x + b) */
            if (!m42_node_depends_on (a, var) && linear_coeffs (b, var, &la, &lb) && la != 0)
              return DIV (POW (CP (a), CP (b)), MUL (NUM (la), CALL ("Log", CP (a))));
            return NULL;

          default:
            return NULL;
          }
      }

    case M42_NODE_CALL:
      return integrate_call (n, var);

    default:
      return NULL;
    }
}

M42Node *
m42_node_integrate (const M42Node *n, const char *var)
{
  M42Node *raw = integrate_node (n, var, 0);
  M42Node *simple;

  if (raw == NULL)
    return NULL;
  simple = m42_node_simplify (raw);
  m42_node_free (raw);
  return simple;
}

/* --- expansion ------------------------------------------------------------
 *
 * Multiplying out is the easy half: a sum times anything is the sum of
 * the products, and a sum to a whole power is that multiplication done
 * again and again.  The other half is gathering what comes out, so
 * that x x + x + x + 1 is written x^2 + 2 x + 1; terms are gathered by
 * what they look like once their number is taken off the front.
 */

static M42Node *expand_node (const M42Node *n, int depth);

/* a (b + c) -> a b + a c, either way round. */
static M42Node *
distribute (M42Node *a, M42Node *b, int depth)
{
  gboolean a_sum = a->kind == M42_NODE_BINARY &&
                   (a->op == M42_TOK_PLUS || a->op == M42_TOK_MINUS);
  gboolean b_sum = b->kind == M42_NODE_BINARY &&
                   (b->op == M42_TOK_PLUS || b->op == M42_TOK_MINUS);

  if (depth > 12)
    return MUL (a, b);

  if (b_sum)
    {
      M42Node *left = distribute (CP (a), CP (m42_node_child (b, 0)), depth + 1);
      M42Node *right = distribute (a, CP (m42_node_child (b, 1)), depth + 1);
      int op = b->op;

      m42_node_free (b);
      return m42_node_binary (op, left, right);
    }
  if (a_sum)
    {
      M42Node *left = distribute (CP (m42_node_child (a, 0)), CP (b), depth + 1);
      M42Node *right = distribute (CP (m42_node_child (a, 1)), b, depth + 1);
      int op = a->op;

      m42_node_free (a);
      return m42_node_binary (op, left, right);
    }
  return MUL (a, b);
}

static M42Node *
expand_node (const M42Node *n, int depth)
{
  if (depth > 16)
    return CP (n);

  if (n->kind == M42_NODE_BINARY)
    {
      switch (n->op)
        {
        case M42_TOK_PLUS: case M42_TOK_MINUS:
          return m42_node_binary (n->op,
                                  expand_node (m42_node_child (n, 0), depth + 1),
                                  expand_node (m42_node_child (n, 1), depth + 1));

        case M42_TOK_STAR:
          return distribute (expand_node (m42_node_child (n, 0), depth + 1),
                             expand_node (m42_node_child (n, 1), depth + 1), depth);

        case M42_TOK_SLASH:
          {
            /* (a + b)/c is a/c + b/c, which keeps the terms apart. */
            M42Node *num = expand_node (m42_node_child (n, 0), depth + 1);
            M42Node *den = expand_node (m42_node_child (n, 1), depth + 1);

            if (num->kind == M42_NODE_BINARY &&
                (num->op == M42_TOK_PLUS || num->op == M42_TOK_MINUS))
              {
                M42Node *left = DIV (CP (m42_node_child (num, 0)), CP (den));
                M42Node *right = DIV (CP (m42_node_child (num, 1)), den);
                int op = num->op;

                m42_node_free (num);
                return m42_node_binary (op, expand_node (left, depth + 1),
                                        expand_node (right, depth + 1));
              }
            return DIV (num, den);
          }

        case M42_TOK_CARET:
          {
            const M42Node *base = m42_node_child (n, 0);
            const M42Node *e = m42_node_child (n, 1);

            if (e->kind == M42_NODE_NUMBER && e->number >= 0 && e->number <= 12 &&
                e->number == floor (e->number) &&
                base->kind == M42_NODE_BINARY &&
                (base->op == M42_TOK_PLUS || base->op == M42_TOK_MINUS))
              {
                M42Node *acc = NUM (1);
                for (int i = 0; i < (int) e->number; i++)
                  acc = distribute (acc, expand_node (base, depth + 1), depth);
                return acc;
              }
            return CP (n);
          }

        default:
          return CP (n);
        }
    }

  if (n->kind == M42_NODE_UNARY && n->op == M42_TOK_MINUS)
    return NEG (expand_node (m42_node_child (n, 0), depth + 1));

  return CP (n);
}

/* Every term of a sum, with the sign it carries. */
static void
flatten_sum (const M42Node *n, GPtrArray *terms, GArray *signs, int sign, int depth)
{
  if (depth < 24 && n->kind == M42_NODE_BINARY &&
      (n->op == M42_TOK_PLUS || n->op == M42_TOK_MINUS))
    {
      flatten_sum (m42_node_child (n, 0), terms, signs, sign, depth + 1);
      flatten_sum (m42_node_child (n, 1), terms, signs,
                   n->op == M42_TOK_MINUS ? -sign : sign, depth + 1);
      return;
    }
  if (depth < 24 && n->kind == M42_NODE_UNARY && n->op == M42_TOK_MINUS)
    {
      flatten_sum (m42_node_child (n, 0), terms, signs, -sign, depth + 1);
      return;
    }
  g_ptr_array_add (terms, (gpointer) n);
  g_array_append_val (signs, sign);
}

/* A term split into the number in front of it and everything else. */
static void
split_coefficient (const M42Node *n, double *coeff, M42Node **rest)
{
  if (n->kind == M42_NODE_NUMBER)
    {
      *coeff = n->number;
      *rest = NULL;
      return;
    }
  /* A minus in front is part of the number, not of the term: without
   * this, -x and 2 x look like different terms and are not gathered. */
  if (n->kind == M42_NODE_UNARY && n->op == M42_TOK_MINUS)
    {
      split_coefficient (m42_node_child (n, 0), coeff, rest);
      *coeff = -*coeff;
      return;
    }
  if (n->kind == M42_NODE_BINARY && n->op == M42_TOK_STAR)
    {
      double c1, c2;
      M42Node *r1, *r2;

      split_coefficient (m42_node_child (n, 0), &c1, &r1);
      split_coefficient (m42_node_child (n, 1), &c2, &r2);
      *coeff = c1 * c2;
      if (r1 == NULL)
        *rest = r2;
      else if (r2 == NULL)
        *rest = r1;
      else
        {
          *rest = MUL (r1, r2);
          return;
        }
      return;
    }
  if (n->kind == M42_NODE_BINARY && n->op == M42_TOK_SLASH &&
      m42_node_child (n, 1)->kind == M42_NODE_NUMBER &&
      m42_node_child (n, 1)->number != 0)
    {
      double c;
      M42Node *r;

      split_coefficient (m42_node_child (n, 0), &c, &r);
      *coeff = c / m42_node_child (n, 1)->number;
      *rest = r;
      return;
    }
  *coeff = 1;
  *rest = CP (n);
}

/* How high a term reaches, so that the sum can be written with the
 * biggest power first, the way a polynomial is set out. */
static int
term_degree (const M42Node *n)
{
  if (n == NULL || n->kind == M42_NODE_NUMBER)
    return 0;
  if (n->kind == M42_NODE_IDENT)
    return 1;
  if (n->kind == M42_NODE_BINARY)
    {
      if (n->op == M42_TOK_STAR)
        return term_degree (m42_node_child (n, 0)) + term_degree (m42_node_child (n, 1));
      if (n->op == M42_TOK_CARET && m42_node_child (n, 1)->kind == M42_NODE_NUMBER)
        return term_degree (m42_node_child (n, 0)) * (int) m42_node_child (n, 1)->number;
      if (n->op == M42_TOK_SLASH)
        return term_degree (m42_node_child (n, 0));
    }
  return 1;
}

/* Every factor of a product, in the order it was written. */
static void
flatten_product (const M42Node *n, GPtrArray *factors, int depth)
{
  if (depth < 24 && n->kind == M42_NODE_BINARY && n->op == M42_TOK_STAR)
    {
      flatten_product (m42_node_child (n, 0), factors, depth + 1);
      flatten_product (m42_node_child (n, 1), factors, depth + 1);
      return;
    }
  g_ptr_array_add (factors, (gpointer) n);
}

static int
compare_factors (gconstpointer a, gconstpointer b)
{
  const M42Node *x = *(const M42Node * const *) a, *y = *(const M42Node * const *) b;
  g_autoptr (GString) sx = g_string_new (NULL);
  g_autoptr (GString) sy = g_string_new (NULL);

  m42_node_to_string (sx, x);
  m42_node_to_string (sy, y);
  return g_strcmp0 (sx->str, sy->str);
}

/* The same product always written the same way round, with repeated
 * factors gathered into powers, so that x y y and y x y are both x y^2
 * and are seen to be one term. */
static M42Node *
canonical_product (const M42Node *n)
{
  g_autoptr (GPtrArray) factors = g_ptr_array_new ();
  g_autoptr (GPtrArray) bases = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_node_free);
  g_autoptr (GArray) exps = g_array_new (FALSE, FALSE, sizeof (double));
  M42Node *out = NULL;

  flatten_product (n, factors, 0);
  if (factors->len < 2)
    return m42_node_copy (n);

  g_ptr_array_sort (factors, compare_factors);

  for (guint i = 0; i < factors->len; i++)
    {
      const M42Node *f = g_ptr_array_index (factors, i);
      const M42Node *base = f;
      double e = 1;

      if (f->kind == M42_NODE_BINARY && f->op == M42_TOK_CARET &&
          m42_node_child (f, 1)->kind == M42_NODE_NUMBER)
        {
          base = m42_node_child (f, 0);
          e = m42_node_child (f, 1)->number;
        }

      if (bases->len > 0)
        {
          g_autoptr (GString) prev = g_string_new (NULL);
          g_autoptr (GString) here = g_string_new (NULL);

          m42_node_to_string (prev, g_ptr_array_index (bases, bases->len - 1));
          m42_node_to_string (here, base);
          if (strcmp (prev->str, here->str) == 0)
            {
              g_array_index (exps, double, exps->len - 1) += e;
              continue;
            }
        }
      g_ptr_array_add (bases, m42_node_copy (base));
      g_array_append_val (exps, e);
    }

  for (guint i = 0; i < bases->len; i++)
    {
      M42Node *f = m42_node_copy (g_ptr_array_index (bases, i));
      double e = g_array_index (exps, double, i);

      if (e != 1)
        f = m42_node_binary (M42_TOK_CARET, f, m42_node_number (e));
      out = out == NULL ? f : m42_node_binary (M42_TOK_STAR, out, f);
    }
  return out != NULL ? out : m42_node_copy (n);
}

typedef struct {
  char    *key;      /* the term without its number, as it prints */
  M42Node *rest;     /* that same term as a tree, owned */
  double   coeff;
  int      degree;
} Term;

static void
term_free (Term *t)
{
  g_free (t->key);
  m42_node_free (t->rest);
  g_free (t);
}

static int
compare_terms (gconstpointer a, gconstpointer b)
{
  const Term *x = *(Term * const *) a, *y = *(Term * const *) b;

  if (x->degree != y->degree)
    return y->degree - x->degree;
  return g_strcmp0 (x->key, y->key);
}

/* Gathers like terms in an expanded sum. */
static M42Node *
collect_terms (const M42Node *n)
{
  g_autoptr (GPtrArray) raw = g_ptr_array_new ();
  g_autoptr (GArray) signs = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GPtrArray) terms = g_ptr_array_new_with_free_func ((GDestroyNotify) term_free);
  g_autoptr (GHashTable) by_key = g_hash_table_new (g_str_hash, g_str_equal);
  M42Node *out = NULL;

  flatten_sum (n, raw, signs, 1, 0);

  for (guint i = 0; i < raw->len; i++)
    {
      const M42Node *t = g_ptr_array_index (raw, i);
      g_autoptr (M42Node) simple = m42_node_simplify (t);
      double coeff;
      M42Node *rest;
      g_autoptr (GString) key = g_string_new (NULL);
      Term *found;

      split_coefficient (simple, &coeff, &rest);
      coeff *= g_array_index (signs, int, i);
      if (rest != NULL)
        {
          M42Node *ordered = canonical_product (rest);
          M42Node *reduced = m42_node_simplify (ordered);

          m42_node_free (rest);
          m42_node_free (ordered);
          rest = reduced;
          m42_node_to_string (key, rest);
        }
      else
        g_string_assign (key, "");

      found = g_hash_table_lookup (by_key, key->str);
      if (found != NULL)
        {
          found->coeff += coeff;
          m42_node_free (rest);
        }
      else
        {
          Term *term = g_new0 (Term, 1);
          term->key = g_strdup (key->str);
          term->rest = rest;
          term->coeff = coeff;
          term->degree = term_degree (rest);
          g_ptr_array_add (terms, term);
          g_hash_table_insert (by_key, term->key, term);
        }
    }

  g_ptr_array_sort (terms, compare_terms);

  for (guint i = 0; i < terms->len; i++)
    {
      Term *t = g_ptr_array_index (terms, i);
      M42Node *piece;
      double c = t->coeff;

      if (c == 0)
        continue;
      if (t->rest == NULL)
        piece = NUM (c);
      else if (c == 1)
        piece = CP (t->rest);
      else if (c == -1)
        piece = NEG (CP (t->rest));
      else
        piece = MUL (NUM (c), CP (t->rest));

      out = out == NULL ? piece : ADD (out, piece);
    }

  return out != NULL ? out : NUM (0);
}

M42Node *
m42_node_expand (const M42Node *n)
{
  g_autoptr (M42Node) expanded = expand_node (n, 0);
  g_autoptr (M42Node) gathered = collect_terms (expanded);

  return m42_node_simplify (gathered);
}

/* --- the transforms of a fourth course -----------------------------------
 *
 * Laplace by the table every course hands out, read forwards and
 * backwards; the Fourier series of a function on an interval, worked
 * out by quadrature; and the discrete transform of a list, which is
 * what a computer means by Fourier.
 */

/* t^n, and how many; -1 when the tree is not a power of the variable. */
static int
power_of (const M42Node *n, const char *var)
{
  if (n->kind == M42_NODE_IDENT && strcmp (n->name, var) == 0)
    return 1;
  if (n->kind == M42_NODE_BINARY && n->op == M42_TOK_CARET &&
      m42_node_child (n, 0)->kind == M42_NODE_IDENT &&
      strcmp (m42_node_child (n, 0)->name, var) == 0 &&
      m42_node_child (n, 1)->kind == M42_NODE_NUMBER &&
      m42_node_child (n, 1)->number >= 0 &&
      m42_node_child (n, 1)->number == floor (m42_node_child (n, 1)->number))
    return (int) m42_node_child (n, 1)->number;
  return -1;
}

/* n! as a double, for the table. */
static double
factorial_of (int n)
{
  double acc = 1;

  for (int i = 2; i <= n; i++)
    acc *= i;
  return acc;
}

static M42Node *laplace_of (const M42Node *n, const char *t, const char *sname, int depth);

/* The transform of one term, by the table:
 *   1        -> 1/s              t^n     -> n!/s^(n+1)
 *   e^(at)   -> 1/(s - a)        Sin[wt] -> w/(s^2 + w^2)
 *   Cos[wt]  -> s/(s^2 + w^2)    Sinh    -> w/(s^2 - w^2)
 *   Cosh[wt] -> s/(s^2 - w^2)
 * with e^(at) f(t) shifting the s in whatever f gives. */
static M42Node *
laplace_term (const M42Node *n, const char *t, const char *sname, int depth)
{
  M42Node *s = m42_node_ident (sname);
  int power;

  if (depth > 8)
    {
      m42_node_free (s);
      return NULL;
    }

  if (!m42_node_depends_on (n, t))
    return DIV (CP (n), s);        /* a constant */

  power = power_of (n, t);
  if (power >= 0)
    return DIV (NUM (factorial_of (power)), POW (s, NUM (power + 1)));

  if (n->kind == M42_NODE_CALL && n->children->len == 1)
    {
      const char *f = n->name;
      double w, b;

      if (!linear_coeffs (m42_node_child (n, 0), t, &w, &b) || fabs (b) > 1e-12)
        {
          m42_node_free (s);
          return NULL;
        }

      if (!strcmp (f, "Exp") || !strcmp (f, "exp"))
        return DIV (NUM (1), SUB (s, NUM (w)));
      if (!strcmp (f, "Sin") || !strcmp (f, "sin"))
        return DIV (NUM (w), ADD (POW (s, NUM (2)), NUM (w * w)));
      if (!strcmp (f, "Cos") || !strcmp (f, "cos"))
        return DIV (CP (s), ADD (POW (s, NUM (2)), NUM (w * w)));
      if (!strcmp (f, "Sinh") || !strcmp (f, "sinh"))
        return DIV (NUM (w), SUB (POW (s, NUM (2)), NUM (w * w)));
      if (!strcmp (f, "Cosh") || !strcmp (f, "cosh"))
        return DIV (CP (s), SUB (POW (s, NUM (2)), NUM (w * w)));
      m42_node_free (s);
      return NULL;
    }

  m42_node_free (s);
  return NULL;
}

/* e^(at) f(t): whatever f gives, with s - a in place of s. */
static M42Node *
laplace_shifted (const M42Node *a, const M42Node *b, const char *t,
                 const char *sname, int depth)
{
  for (int swap = 0; swap < 2; swap++)
    {
      const M42Node *e = swap ? b : a;
      const M42Node *rest = swap ? a : b;
      double rate, offset;

      if (e->kind != M42_NODE_CALL || e->children->len != 1 ||
          (strcmp (e->name, "Exp") != 0 && strcmp (e->name, "exp") != 0))
        continue;
      if (!linear_coeffs (m42_node_child (e, 0), t, &rate, &offset) || fabs (offset) > 1e-12)
        continue;

      {
        g_autoptr (M42Node) inner = laplace_of (rest, t, "$s", depth + 1);
        M42Node *shifted;

        if (inner == NULL)
          continue;
        {
          g_autoptr (M42Node) minus = SUB (m42_node_ident (sname), NUM (rate));
          shifted = m42_node_substitute (inner, "$s", minus);
        }
        return shifted;
      }
    }
  return NULL;
}

static M42Node *
laplace_of (const M42Node *n, const char *t, const char *sname, int depth)
{
  if (depth > 8)
    return NULL;

  switch (n->kind)
    {
    case M42_NODE_BINARY:
      if (n->op == M42_TOK_PLUS || n->op == M42_TOK_MINUS)
        {
          M42Node *a = laplace_of (m42_node_child (n, 0), t, sname, depth + 1);
          M42Node *b = a != NULL ? laplace_of (m42_node_child (n, 1), t, sname, depth + 1) : NULL;

          if (b == NULL)
            {
              m42_node_free (a);
              return NULL;
            }
          return m42_node_binary (n->op, a, b);
        }
      if (n->op == M42_TOK_STAR)
        {
          const M42Node *a = m42_node_child (n, 0), *b = m42_node_child (n, 1);
          M42Node *r;

          /* A number in front comes through untouched. */
          if (!m42_node_depends_on (a, t))
            {
              r = laplace_of (b, t, sname, depth + 1);
              return r != NULL ? MUL (CP (a), r) : NULL;
            }
          if (!m42_node_depends_on (b, t))
            {
              r = laplace_of (a, t, sname, depth + 1);
              return r != NULL ? MUL (CP (b), r) : NULL;
            }
          r = laplace_shifted (a, b, t, sname, depth);
          if (r != NULL)
            return r;

          /* t f(t) is minus the derivative of what f gives. */
          for (int swap = 0; swap < 2; swap++)
            {
              const M42Node *x = swap ? b : a;
              const M42Node *y = swap ? a : b;

              if (power_of (x, t) == 1)
                {
                  g_autoptr (M42Node) inner = laplace_of (y, t, sname, depth + 1);
                  M42Node *d;

                  if (inner == NULL)
                    continue;
                  d = m42_node_differentiate (inner, sname);
                  if (d == NULL)
                    continue;
                  return NEG (d);
                }
            }
          return NULL;
        }
      if (n->op == M42_TOK_SLASH && !m42_node_depends_on (m42_node_child (n, 1), t))
        {
          M42Node *r = laplace_of (m42_node_child (n, 0), t, sname, depth + 1);
          return r != NULL ? DIV (r, CP (m42_node_child (n, 1))) : NULL;
        }
      return laplace_term (n, t, sname, depth);

    case M42_NODE_UNARY:
      if (n->op == M42_TOK_MINUS)
        {
          M42Node *r = laplace_of (m42_node_child (n, 0), t, sname, depth + 1);
          return r != NULL ? NEG (r) : NULL;
        }
      return NULL;

    default:
      return laplace_term (n, t, sname, depth);
    }
}

/* Backwards, by the same table read the other way: what is in s comes
 * back as what it was in t.  A quotient of polynomials is split into
 * partial fractions first, which is how it is done by hand. */
static M42Node *
inverse_laplace_of (const M42Node *n, const char *sname, const char *t, int depth)
{
  g_autoptr (GArray) num = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) den = g_array_new (FALSE, TRUE, sizeof (double));

  if (depth > 8)
    return NULL;

  if (n->kind == M42_NODE_BINARY && (n->op == M42_TOK_PLUS || n->op == M42_TOK_MINUS))
    {
      M42Node *a = inverse_laplace_of (m42_node_child (n, 0), sname, t, depth + 1);
      M42Node *b = a != NULL ? inverse_laplace_of (m42_node_child (n, 1), sname, t, depth + 1) : NULL;

      if (b == NULL)
        {
          m42_node_free (a);
          return NULL;
        }
      return m42_node_binary (n->op, a, b);
    }
  if (n->kind == M42_NODE_BINARY && n->op == M42_TOK_STAR &&
      !m42_node_depends_on (m42_node_child (n, 0), sname))
    {
      M42Node *r = inverse_laplace_of (m42_node_child (n, 1), sname, t, depth + 1);
      return r != NULL ? MUL (CP (m42_node_child (n, 0)), r) : NULL;
    }

  if (n->kind != M42_NODE_BINARY || n->op != M42_TOK_SLASH)
    return NULL;
  if (!poly_of (m42_node_child (n, 0), sname, num, 0) ||
      !poly_of (m42_node_child (n, 1), sname, den, 0))
    return NULL;
  while (num->len > 1 && fabs (g_array_index (num, double, num->len - 1)) < 1e-12)
    g_array_set_size (num, num->len - 1);
  while (den->len > 1 && fabs (g_array_index (den, double, den->len - 1)) < 1e-12)
    g_array_set_size (den, den->len - 1);

  /* c/s^n comes back as a power of t. */
  if (num->len == 1 && den->len >= 2)
    {
      gboolean pure_power = TRUE;

      for (guint i = 0; i + 1 < den->len; i++)
        if (fabs (g_array_index (den, double, i)) > 1e-12)
          pure_power = FALSE;
      if (pure_power)
        {
          int k = (int) den->len - 2;      /* 1/s^(k+1) -> t^k/k! */
          double c = g_array_index (num, double, 0) /
                     g_array_index (den, double, den->len - 1) / factorial_of (k);

          if (k == 0)
            return NUM (c);
          return DIV (POW (m42_node_ident (t), NUM (k)), number_node (1 / c));
        }
    }

  if (den->len == 2)
    {
      /* c/(a s + b) -> (c/a) e^(-b/a t) */
      double a = g_array_index (den, double, 1), b = g_array_index (den, double, 0);
      double c = g_array_index (num, double, 0);

      if (num->len != 1)
        return NULL;
      return MUL (number_node (c / a), CALL ("Exp", MUL (number_node (-b / a), m42_node_ident (t))));
    }

  if (den->len > 3)
    {
      /* A denominator of higher degree is split at a real root -- every
       * real polynomial of odd degree has one, and the ones a course
       * sets have them anyway -- into a linear piece and the rest,
       * each of which the table above knows.  This is the partial
       * fractions of a Laplace exercise, done the way it is by hand. */
      double root;
      gboolean found = FALSE;
      g_autoptr (GArray) rest = g_array_new (FALSE, TRUE, sizeof (double));
      double numerator_at_root = 0, rest_at_root = 0;

      {
        /* A root, by scanning for a change of sign and closing in. */
        double previous = 0;
        gboolean have_previous = FALSE;

        for (double x = -50; x <= 50 && !found; x += 0.05)
          {
            double y = 0;

            for (guint i = den->len; i > 0; i--)
              y = y * x + g_array_index (den, double, i - 1);
            if (have_previous && ((y == 0) || ((y < 0) != (previous < 0))))
              {
                double lo = x - 0.05, hi = x, flo = previous;

                for (int k = 0; k < 80; k++)
                  {
                    double mid = (lo + hi) / 2, fm = 0;

                    for (guint i = den->len; i > 0; i--)
                      fm = fm * mid + g_array_index (den, double, i - 1);
                    if ((fm < 0) == (flo < 0))
                      {
                        lo = mid;
                        flo = fm;
                      }
                    else
                      hi = mid;
                  }
                root = (lo + hi) / 2;
                if (fabs (root - round (root)) < 1e-9)
                  root = round (root);
                found = TRUE;
              }
            previous = y;
            have_previous = TRUE;
          }
      }
      if (!found)
        return NULL;

      /* Divide the root out: den = (s - root) rest. */
      {
        guint m = den->len - 1;

        g_array_set_size (rest, m);
        g_array_index (rest, double, m - 1) = g_array_index (den, double, m);
        for (guint i = m - 1; i > 0; i--)
          g_array_index (rest, double, i - 1) =
            g_array_index (den, double, i) + root * g_array_index (rest, double, i);
      }

      /* A = N(root)/rest(root), the coefficient of the linear piece. */
      for (guint i = num->len; i > 0; i--)
        numerator_at_root = numerator_at_root * root + g_array_index (num, double, i - 1);
      for (guint i = rest->len; i > 0; i--)
        rest_at_root = rest_at_root * root + g_array_index (rest, double, i - 1);
      if (fabs (rest_at_root) < 1e-12)
        return NULL;

      {
        double a_coefficient = numerator_at_root / rest_at_root;
        g_autoptr (GArray) remainder = g_array_new (FALSE, TRUE, sizeof (double));
        M42Node *first, *second;

        /* What is left: (N(s) - A rest(s)) / ((s - root) rest(s)), whose
         * top has (s - root) as a factor. */
        g_array_set_size (remainder, MAX (num->len, rest->len));
        for (guint i = 0; i < remainder->len; i++)
          {
            double n_i = i < num->len ? g_array_index (num, double, i) : 0;
            double r_i = i < rest->len ? g_array_index (rest, double, i) : 0;

            g_array_index (remainder, double, i) = n_i - a_coefficient * r_i;
          }
        /* Divide that top by (s - root). */
        {
          guint m = remainder->len;
          g_autoptr (GArray) quotient = g_array_new (FALSE, TRUE, sizeof (double));

          if (m < 2)
            return NULL;
          g_array_set_size (quotient, m - 1);
          g_array_index (quotient, double, m - 2) = g_array_index (remainder, double, m - 1);
          for (guint i = m - 2; i > 0; i--)
            g_array_index (quotient, double, i - 1) =
              g_array_index (remainder, double, i) + root * g_array_index (quotient, double, i);

          /* A/(s - root), and the rest over what is left of the bottom. */
          {
            g_autoptr (M42Node) linear = SUB (m42_node_ident (sname), number_node (root));
            g_autoptr (M42Node) piece_one = DIV (number_node (a_coefficient), CP (linear));
            g_autoptr (M42Node) bottom = poly_node (rest, sname);
            g_autoptr (M42Node) top = poly_node (quotient, sname);
            g_autoptr (M42Node) piece_two = DIV (CP (top), CP (bottom));

            first = inverse_laplace_of (piece_one, sname, t, depth + 1);
            second = first != NULL ? inverse_laplace_of (piece_two, sname, t, depth + 1) : NULL;
            if (second == NULL)
              {
                m42_node_free (first);
                return NULL;
              }
            return ADD (first, second);
          }
        }
      }
    }

  if (den->len == 3)
    {
      double a = g_array_index (den, double, 2);
      double b = g_array_index (den, double, 1);
      double c = g_array_index (den, double, 0);
      double p = num->len > 1 ? g_array_index (num, double, 1) : 0;
      double q = g_array_index (num, double, 0);
      double disc = b * b - 4 * a * c;
      double alpha = -b / (2 * a);

      if (disc < -1e-12)
        {
          /* A shifted sine and cosine. */
          double beta = sqrt (-disc) / (2 * a);
          M42Node *wave = ADD (MUL (number_node (p / a), CALL ("Cos", MUL (number_node (beta), m42_node_ident (t)))),
                               MUL (number_node ((q / a - alpha * p / a) / beta),
                                    CALL ("Sin", MUL (number_node (beta), m42_node_ident (t)))));

          if (fabs (alpha) < 1e-12)
            return wave;
          return MUL (CALL ("Exp", MUL (number_node (alpha), m42_node_ident (t))), wave);
        }
      if (disc > 1e-12)
        {
          /* Two exponentials. */
          double r1 = (-b + sqrt (disc)) / (2 * a), r2 = (-b - sqrt (disc)) / (2 * a);
          double a1 = (p * r1 + q) / (a * (r1 - r2)), a2 = (p * r2 + q) / (a * (r2 - r1));

          return ADD (MUL (number_node (a1), CALL ("Exp", MUL (number_node (r1), m42_node_ident (t)))),
                      MUL (number_node (a2), CALL ("Exp", MUL (number_node (r2), m42_node_ident (t)))));
        }
      {
        /* A repeated root: (A + B t) e^(rt). */
        double r = alpha;
        M42Node *bracket = ADD (number_node (p / a), MUL (number_node ((q - p * r) / a), m42_node_ident (t)));

        return MUL (bracket, CALL ("Exp", MUL (number_node (r), m42_node_ident (t))));
      }
    }
  return NULL;
}

M42Node *
m42_node_laplace (const M42Node *n, const char *t, const char *s)
{
  M42Node *raw = laplace_of (n, t, s, 0);
  M42Node *simple;

  if (raw == NULL)
    return NULL;
  simple = m42_node_simplify (raw);
  m42_node_free (raw);
  return simple;
}

M42Node *
m42_node_inverse_laplace (const M42Node *n, const char *s, const char *t)
{
  M42Node *raw = inverse_laplace_of (n, s, t, 0);
  M42Node *simple;

  if (raw == NULL)
    return NULL;
  simple = m42_node_simplify (raw);
  m42_node_free (raw);
  return simple;
}

/* --- the Z transform ------------------------------------------------------
 *
 * The one a course on discrete systems hands out, taken one sided:
 *
 *   1        z/(z - 1)              a^n     z/(z - a)
 *   n        z/(z - 1)^2            n a^n   a z/(z - a)^2
 *   n^2      z (z + 1)/(z - 1)^3
 *   Sin[b n] z Sin[b]/(z^2 - 2 z Cos[b] + 1)
 *   Cos[b n] z (z - Cos[b])/(z^2 - 2 z Cos[b] + 1)
 *
 * with sums and constant multiples taken apart the way the Laplace
 * transform above takes them.  Anything else comes back NULL and the
 * caller leaves the transform as it was written.
 */
static M42Node *z_of (const M42Node *n, const char *var, const char *zname, int depth);

/* a^n, where a is anything the index is not in: the base, or NULL. */
static const M42Node *
geometric_base (const M42Node *n, const char *var)
{
  if (n->kind != M42_NODE_BINARY || n->op != M42_TOK_CARET)
    return NULL;
  if (m42_node_depends_on (m42_node_child (n, 0), var))
    return NULL;
  {
    const M42Node *e = m42_node_child (n, 1);

    if (e->kind == M42_NODE_IDENT && strcmp (e->name, var) == 0)
      return m42_node_child (n, 0);
  }
  return NULL;
}

/* z^2 - 2 z Cos[b] + 1, which both waves stand over. */
static M42Node *
wave_bottom (const M42Node *z, const M42Node *b)
{
  return ADD (SUB (POW (CP (z), NUM (2)),
                   MUL (MUL (NUM (2), CP (z)), CALL ("Cos", CP (b)))),
              NUM (1));
}

static M42Node *
z_of (const M42Node *n, const char *var, const char *zname, int depth)
{
  g_autoptr (M42Node) z = m42_node_ident (zname);

  if (depth > 8)
    return NULL;

  /* Anything the index is not in is a step of that height. */
  if (!m42_node_depends_on (n, var))
    return MUL (CP (n), DIV (CP (z), SUB (CP (z), NUM (1))));

  if (n->kind == M42_NODE_IDENT && strcmp (n->name, var) == 0)
    return DIV (CP (z), POW (SUB (CP (z), NUM (1)), NUM (2)));

  if (n->kind == M42_NODE_BINARY)
    {
      if (n->op == M42_TOK_PLUS || n->op == M42_TOK_MINUS)
        {
          M42Node *a = z_of (m42_node_child (n, 0), var, zname, depth + 1);
          M42Node *b = a != NULL ? z_of (m42_node_child (n, 1), var, zname, depth + 1) : NULL;

          if (b == NULL)
            {
              m42_node_free (a);
              return NULL;
            }
          return m42_node_binary (n->op, a, b);
        }

      /* n^2, and a^n. */
      if (n->op == M42_TOK_CARET)
        {
          const M42Node *base = m42_node_child (n, 0), *e = m42_node_child (n, 1);

          if (base->kind == M42_NODE_IDENT && strcmp (base->name, var) == 0 &&
              e->kind == M42_NODE_NUMBER && e->number == 2)
            return DIV (MUL (CP (z), ADD (CP (z), NUM (1))),
                        POW (SUB (CP (z), NUM (1)), NUM (3)));
          {
            const M42Node *a = geometric_base (n, var);

            if (a != NULL)
              return DIV (CP (z), SUB (CP (z), CP (a)));
          }
          return NULL;
        }

      if (n->op == M42_TOK_STAR)
        {
          const M42Node *a = m42_node_child (n, 0), *b = m42_node_child (n, 1);
          const M42Node *base;

          /* A number in front comes through untouched. */
          if (!m42_node_depends_on (a, var))
            {
              M42Node *r = z_of (b, var, zname, depth + 1);

              return r != NULL ? MUL (CP (a), r) : NULL;
            }
          if (!m42_node_depends_on (b, var))
            {
              M42Node *r = z_of (a, var, zname, depth + 1);

              return r != NULL ? MUL (CP (b), r) : NULL;
            }

          /* n a^n, either way round. */
          for (int swap = 0; swap < 2; swap++)
            {
              const M42Node *left = swap ? b : a, *right = swap ? a : b;

              base = geometric_base (right, var);
              if (base != NULL && left->kind == M42_NODE_IDENT &&
                  strcmp (left->name, var) == 0)
                return DIV (MUL (CP (base), CP (z)),
                            POW (SUB (CP (z), CP (base)), NUM (2)));
            }
          return NULL;
        }
      if (n->op == M42_TOK_SLASH && !m42_node_depends_on (m42_node_child (n, 1), var))
        {
          M42Node *r = z_of (m42_node_child (n, 0), var, zname, depth + 1);

          return r != NULL ? DIV (r, CP (m42_node_child (n, 1))) : NULL;
        }
      return NULL;
    }

  if (n->kind == M42_NODE_UNARY && n->op == M42_TOK_MINUS)
    {
      M42Node *r = z_of (m42_node_child (n, 0), var, zname, depth + 1);

      return r != NULL ? NEG (r) : NULL;
    }

  /* Sin[b n] and Cos[b n], with b anything the index is not in. */
  if (n->kind == M42_NODE_CALL && n->children->len == 1)
    {
      gboolean sine = !strcmp (n->name, "Sin") || !strcmp (n->name, "sin");
      gboolean cosine = !strcmp (n->name, "Cos") || !strcmp (n->name, "cos");
      const M42Node *inside = m42_node_child (n, 0);
      g_autoptr (M42Node) b = NULL;

      if (!sine && !cosine)
        return NULL;
      if (inside->kind == M42_NODE_IDENT && strcmp (inside->name, var) == 0)
        b = NUM (1);
      else if (inside->kind == M42_NODE_BINARY && inside->op == M42_TOK_STAR)
        {
          const M42Node *l = m42_node_child (inside, 0), *r = m42_node_child (inside, 1);

          if (r->kind == M42_NODE_IDENT && strcmp (r->name, var) == 0 &&
              !m42_node_depends_on (l, var))
            b = CP (l);
          else if (l->kind == M42_NODE_IDENT && strcmp (l->name, var) == 0 &&
                   !m42_node_depends_on (r, var))
            b = CP (r);
        }
      if (b == NULL)
        return NULL;
      if (sine)
        return DIV (MUL (CP (z), CALL ("Sin", CP (b))), wave_bottom (z, b));
      return DIV (MUL (CP (z), SUB (CP (z), CALL ("Cos", CP (b)))), wave_bottom (z, b));
    }
  return NULL;
}

M42Node *
m42_node_ztransform (const M42Node *n, const char *var, const char *zname)
{
  M42Node *raw = z_of (n, var, zname, 0);
  M42Node *simple;

  if (raw == NULL)
    return NULL;
  simple = m42_node_simplify (raw);
  m42_node_free (raw);
  return simple;
}

/* --- and back again -------------------------------------------------------
 *
 * X(z)/z split into partial fractions is a sum of c/(z - a), and each
 * of those came from c a^n.  That is the whole method a course
 * teaches, and it is the whole of what is here.
 */

/* c/(z - a)^k as its numbers, or FALSE.  The denominator has to be a
 * line in z, or a line squared, and the numerator free of it. */
static gboolean
simple_pole (const M42Node *term, const char *zname, M42Node **c, M42Node **a,
             int *order)
{
  const M42Node *top, *bottom;
  g_autoptr (GArray) den = g_array_new (FALSE, TRUE, sizeof (double));

  *order = 1;
  if (term->kind != M42_NODE_BINARY || term->op != M42_TOK_SLASH)
    return FALSE;
  top = m42_node_child (term, 0);
  bottom = m42_node_child (term, 1);
  if (m42_node_depends_on (top, zname))
    return FALSE;
  /* (z - a)^2 underneath: the same pole, twice over. */
  if (bottom->kind == M42_NODE_BINARY && bottom->op == M42_TOK_CARET &&
      m42_node_child (bottom, 1)->kind == M42_NODE_NUMBER &&
      m42_node_child (bottom, 1)->number == 2)
    {
      *order = 2;
      bottom = m42_node_child (bottom, 0);
    }
  if (!poly_of (bottom, zname, den, 0))
    return FALSE;
  poly_trim (den);

  /* A square written out rather than as a power: z^2 - 2 a z + a^2 is
   * (z - a)^2, which Apart leaves as it found it. */
  if (den->len == 3 && *order == 1)
    {
      double p = g_array_index (den, double, 2);
      double q = g_array_index (den, double, 1);
      double r = g_array_index (den, double, 0);
      double disc = q * q - 4 * p * r;

      if (fabs (p) < 1e-14 || fabs (disc) > 1e-9 * MAX (1.0, q * q))
        return FALSE;
      *order = 2;
      *c = p == 1 ? CP (top) : DIV (CP (top), number_node (p));
      *a = number_node (-q / (2 * p));
      return TRUE;
    }

  if (den->len != 2 || fabs (g_array_index (den, double, 1)) < 1e-14)
    return FALSE;
  {
    double lead = g_array_index (den, double, 1);
    double root = -g_array_index (den, double, 0) / lead;

    *c = lead == 1 ? CP (top) : DIV (CP (top), number_node (lead));
    *a = number_node (root);
  }
  return TRUE;
}

/* Every term of a sum, added to the answer one at a time. */
static gboolean
inverse_z_terms (const M42Node *n, const char *zname, const char *var,
                 M42Node **out, gboolean negate, int depth)
{
  if (depth > 12)
    return FALSE;

  if (n->kind == M42_NODE_BINARY && (n->op == M42_TOK_PLUS || n->op == M42_TOK_MINUS))
    return inverse_z_terms (m42_node_child (n, 0), zname, var, out, negate, depth + 1) &&
           inverse_z_terms (m42_node_child (n, 1), zname, var, out,
                            n->op == M42_TOK_MINUS ? !negate : negate, depth + 1);
  if (n->kind == M42_NODE_UNARY && n->op == M42_TOK_MINUS)
    return inverse_z_terms (m42_node_child (n, 0), zname, var, out, !negate, depth + 1);

  /* A term with no z left in it belongs to the very first step alone,
   * which this method has nothing to say about. */
  if (!m42_node_depends_on (n, zname))
    return FALSE;

  {
    M42Node *c = NULL, *a = NULL;
    M42Node *piece;
    int order = 1;

    if (!simple_pole (n, zname, &c, &a, &order))
      return FALSE;
    if (order == 2)
      {
        /* c/(z - a)^2 came from c n a^(n-1). */
        if (a->kind == M42_NODE_NUMBER && a->number == 1)
          {
            m42_node_free (a);
            piece = MUL (c, m42_node_ident (var));
          }
        else
          piece = MUL (MUL (c, m42_node_ident (var)),
                       POW (a, SUB (m42_node_ident (var), NUM (1))));
      }
    /* c a^n, and a of one is just c. */
    else if (a->kind == M42_NODE_NUMBER && a->number == 1)
      {
        m42_node_free (a);
        piece = c;
      }
    else
      piece = MUL (c, POW (a, m42_node_ident (var)));
    if (negate)
      piece = NEG (piece);
    *out = *out == NULL ? piece : ADD (*out, piece);
    return TRUE;
  }
}

M42Node *
m42_node_inverse_ztransform (const M42Node *n, const char *zname, const char *var)
{
  g_autoptr (GArray) top = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) bottom = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (M42Node) over_z = NULL;
  g_autoptr (M42Node) split = NULL;
  M42Node *out = NULL;

  /* X(z)/z, done on the coefficients rather than by writing another
   * fraction underneath: the numerator of a Z transform has a factor
   * of z in it, and taking that off is exactly dividing by z.  Written
   * as a tower instead, nothing would cancel it and the split below
   * would have nothing to work with. */
  {
    /* A sum of fractions goes over one denominator first, so that
     * there is a numerator and a denominator to take apart. */
    g_autoptr (M42Node) whole = m42_node_together (n);
    const M42Node *num = whole != NULL ? whole : n, *den = NULL;

    if (num->kind == M42_NODE_BINARY && num->op == M42_TOK_SLASH)
      {
        den = m42_node_child (num, 1);
        num = m42_node_child (num, 0);
      }
    if (!poly_of (num, zname, top, 0))
      return NULL;
    if (den != NULL && !poly_of (den, zname, bottom, 0))
      return NULL;
    if (den == NULL)
      {
        g_array_set_size (bottom, 1);
        g_array_index (bottom, double, 0) = 1;
      }
    poly_trim (top);
    poly_trim (bottom);

    if (top->len > 1 && fabs (g_array_index (top, double, 0)) < 1e-14)
      g_array_remove_index (top, 0);        /* the factor of z comes off */
    else
      {
        double nothing = 0;                 /* or z goes underneath */

        g_array_prepend_val (bottom, nothing);
      }
    {
      g_autoptr (M42Node) p = m42_node_from_polynomial (top, zname);
      g_autoptr (M42Node) q = m42_node_from_polynomial (bottom, zname);

      if (p == NULL || q == NULL)
        return NULL;
      over_z = DIV (g_steal_pointer (&p), g_steal_pointer (&q));
    }
  }

  split = m42_node_apart (over_z, zname);
  if (split == NULL)
    split = m42_node_simplify (over_z);
  if (split == NULL)
    return NULL;
  if (!inverse_z_terms (split, zname, var, &out, FALSE, 0))
    {
      m42_node_free (out);
      return NULL;
    }
  {
    M42Node *simple = m42_node_simplify (out);

    m42_node_free (out);
    return simple;
  }
}

/* --- polynomials and rational functions, for the outside world ---------- */

gboolean
m42_node_polynomial (const M42Node *n, const char *var, GArray *out)
{
  if (!poly_of (n, var, out, 0))
    return FALSE;
  poly_trim (out);
  return TRUE;
}

M42Node *
m42_node_from_polynomial (const GArray *coefficients, const char *var)
{
  return poly_node (coefficients, var);
}

void
m42_polynomial_divide (const GArray *p, const GArray *d, GArray *q, GArray *r)
{
  poly_divide (p, d, q, r);
}

/* One fraction over one denominator: a/b + c/d is (a d + c b)/(b d),
 * and the same rule applied all the way down a sum. */
static M42Node *
together_of (const M42Node *n, int depth)
{
  if (depth > 16)
    return m42_node_copy (n);

  if (n->kind == M42_NODE_BINARY && (n->op == M42_TOK_PLUS || n->op == M42_TOK_MINUS))
    {
      g_autoptr (M42Node) a = together_of (m42_node_child (n, 0), depth + 1);
      g_autoptr (M42Node) b = together_of (m42_node_child (n, 1), depth + 1);
      gboolean a_fraction = a->kind == M42_NODE_BINARY && a->op == M42_TOK_SLASH;
      gboolean b_fraction = b->kind == M42_NODE_BINARY && b->op == M42_TOK_SLASH;
      const M42Node *an = a_fraction ? m42_node_child (a, 0) : a;
      const M42Node *ad = a_fraction ? m42_node_child (a, 1) : NULL;
      const M42Node *bn = b_fraction ? m42_node_child (b, 0) : b;
      const M42Node *bd = b_fraction ? m42_node_child (b, 1) : NULL;
      M42Node *top, *bottom;

      if (!a_fraction && !b_fraction)
        return m42_node_binary (n->op, m42_node_copy (a), m42_node_copy (b));

      if (ad != NULL && bd != NULL)
        {
          g_autoptr (GString) sa = g_string_new (NULL);
          g_autoptr (GString) sb = g_string_new (NULL);

          m42_node_to_string (sa, ad);
          m42_node_to_string (sb, bd);
          if (strcmp (sa->str, sb->str) == 0)
            {
              /* The same bottom already: only the tops are added. */
              top = m42_node_binary (n->op, CP (an), CP (bn));
              bottom = CP (ad);
              return DIV (top, bottom);
            }
        }

      top = m42_node_binary (n->op,
                             bd != NULL ? MUL (CP (an), CP (bd)) : CP (an),
                             ad != NULL ? MUL (CP (bn), CP (ad)) : CP (bn));
      bottom = ad != NULL && bd != NULL ? MUL (CP (ad), CP (bd))
               : ad != NULL ? CP (ad) : CP (bd);
      return DIV (top, bottom);
    }

  if (n->kind == M42_NODE_BINARY && n->op == M42_TOK_STAR)
    {
      g_autoptr (M42Node) a = together_of (m42_node_child (n, 0), depth + 1);
      g_autoptr (M42Node) b = together_of (m42_node_child (n, 1), depth + 1);

      return MUL (CP (a), CP (b));
    }
  return m42_node_copy (n);
}

M42Node *
m42_node_together (const M42Node *n)
{
  g_autoptr (M42Node) raw = together_of (n, 0);
  g_autoptr (M42Node) expanded = NULL;

  /* The top multiplied out, so that it reads as one polynomial. */
  if (raw->kind == M42_NODE_BINARY && raw->op == M42_TOK_SLASH)
    {
      M42Node *top = m42_node_expand (m42_node_child (raw, 0));
      M42Node *bottom = m42_node_simplify (m42_node_child (raw, 1));

      return DIV (top, bottom);
    }
  expanded = m42_node_expand (raw);
  return m42_node_simplify (expanded);
}

/* A rational function as a sum of simple fractions: the whole part,
 * then one piece for each factor of the bottom. */
M42Node *
m42_node_apart (const M42Node *n, const char *var)
{
  g_autoptr (GArray) num = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) den = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) quotient = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) remainder = g_array_new (FALSE, TRUE, sizeof (double));
  M42Node *out = NULL;

  if (n->kind != M42_NODE_BINARY || n->op != M42_TOK_SLASH)
    return NULL;
  if (!m42_node_polynomial (m42_node_child (n, 0), var, num) ||
      !m42_node_polynomial (m42_node_child (n, 1), var, den))
    return NULL;
  if (den->len < 2)
    return NULL;

  m42_polynomial_divide (num, den, quotient, remainder);
  if (quotient->len > 1 || fabs (g_array_index (quotient, double, 0)) > 1e-14)
    out = poly_node (quotient, var);

  /* What is left, split at the roots of the bottom. */
  {
    g_autoptr (GArray) left = g_array_new (FALSE, TRUE, sizeof (double));
    g_autoptr (GArray) bottom = g_array_new (FALSE, TRUE, sizeof (double));

    g_array_set_size (left, remainder->len);
    memcpy (left->data, remainder->data, sizeof (double) * remainder->len);
    g_array_set_size (bottom, den->len);
    memcpy (bottom->data, den->data, sizeof (double) * den->len);
    poly_trim (left);

    if (left->len == 1 && fabs (g_array_index (left, double, 0)) < 1e-14)
      return out != NULL ? out : m42_node_number (0);

    while (bottom->len > 2)
      {
        /* A real root, by scanning for a change of sign. */
        double root = 0, previous = 0;
        gboolean found = FALSE, have_previous = FALSE;

        for (double x = -60; x <= 60 && !found; x += 0.05)
          {
            double y = 0;

            for (guint i = bottom->len; i > 0; i--)
              y = y * x + g_array_index (bottom, double, i - 1);
            if (have_previous && (y == 0 || (y < 0) != (previous < 0)))
              {
                double lo = x - 0.05, hi = x, flo = previous;

                for (int k = 0; k < 80; k++)
                  {
                    double mid = (lo + hi) / 2, fm = 0;

                    for (guint i = bottom->len; i > 0; i--)
                      fm = fm * mid + g_array_index (bottom, double, i - 1);
                    if ((fm < 0) == (flo < 0))
                      {
                        lo = mid;
                        flo = fm;
                      }
                    else
                      hi = mid;
                  }
                root = (lo + hi) / 2;
                if (fabs (root - round (root)) < 1e-9)
                  root = round (root);
                found = TRUE;
              }
            previous = y;
            have_previous = TRUE;
          }
        if (!found)
          break;

        {
          /* Divide the root out and take the piece that belongs to it. */
          guint m = bottom->len - 1;
          g_autoptr (GArray) rest = g_array_new (FALSE, TRUE, sizeof (double));
          double numerator_at_root = 0, rest_at_root = 0, coefficient;
          M42Node *piece;

          g_array_set_size (rest, m);
          g_array_index (rest, double, m - 1) = g_array_index (bottom, double, m);
          for (guint i = m - 1; i > 0; i--)
            g_array_index (rest, double, i - 1) =
              g_array_index (bottom, double, i) + root * g_array_index (rest, double, i);

          for (guint i = left->len; i > 0; i--)
            numerator_at_root = numerator_at_root * root + g_array_index (left, double, i - 1);
          for (guint i = rest->len; i > 0; i--)
            rest_at_root = rest_at_root * root + g_array_index (rest, double, i - 1);
          if (fabs (rest_at_root) < 1e-12)
            break;

          coefficient = numerator_at_root / rest_at_root;
          piece = DIV (number_node (coefficient),
                       SUB (m42_node_ident (var), number_node (root)));
          out = out == NULL ? piece : ADD (out, piece);

          /* What is left over the smaller bottom. */
          {
            g_autoptr (GArray) next = g_array_new (FALSE, TRUE, sizeof (double));
            g_autoptr (GArray) q = g_array_new (FALSE, TRUE, sizeof (double));
            g_autoptr (GArray) r = g_array_new (FALSE, TRUE, sizeof (double));
            g_autoptr (GArray) divisor = g_array_new (FALSE, TRUE, sizeof (double));

            g_array_set_size (next, MAX (left->len, rest->len));
            for (guint i = 0; i < next->len; i++)
              {
                double l_i = i < left->len ? g_array_index (left, double, i) : 0;
                double r_i = i < rest->len ? g_array_index (rest, double, i) : 0;

                g_array_index (next, double, i) = l_i - coefficient * r_i;
              }
            g_array_set_size (divisor, 2);
            g_array_index (divisor, double, 0) = -root;
            g_array_index (divisor, double, 1) = 1;
            m42_polynomial_divide (next, divisor, q, r);
            poly_trim (q);

            g_array_set_size (left, q->len);
            memcpy (left->data, q->data, sizeof (double) * q->len);
            g_array_set_size (bottom, rest->len);
            memcpy (bottom->data, rest->data, sizeof (double) * rest->len);
          }
        }
        poly_trim (left);
        if (left->len == 1 && fabs (g_array_index (left, double, 0)) < 1e-14)
          return out != NULL ? out : m42_node_number (0);
      }

    /* Whatever bottom is left keeps the rest over it. */
    if (!(left->len == 1 && fabs (g_array_index (left, double, 0)) < 1e-14))
      {
        M42Node *piece = DIV (poly_node (left, var), poly_node (bottom, var));

        out = out == NULL ? piece : ADD (out, piece);
      }
  }
  return out != NULL ? m42_node_simplify (out) : NULL;
}
