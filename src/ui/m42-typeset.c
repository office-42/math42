/* m42-typeset.c - see m42-typeset.h
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every box knows its width, its ascent above the baseline and its
 * descent below it, so that a row can line its pieces up on one
 * baseline and a fraction can sit on the maths axis the way a printed
 * formula does.  Sizes are in points of the current font, so the whole
 * layout scales with it.
 */

#include "m42-typeset.h"
#include "m42-symbolic.h"
#include "m42-lexer.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

typedef enum {
  BOX_TEXT,     /* one Pango layout */
  BOX_ROW,      /* children side by side on one baseline */
  BOX_FRAC,     /* children[0] over children[1] */
  BOX_POWER,    /* children[0] with children[1] raised */
  BOX_SQRT,     /* a radical over children[0] */
  BOX_FENCE,    /* children[0] inside brackets that grow with it */
  BOX_GRID,     /* a matrix: rows x cols children, square brackets */
  BOX_PLOT,     /* a graph */
  BOX_BIGOP,    /* an operator with limits: children (sign, under, over) */
  BOX_STACK,    /* children one above another, left aligned */
} BoxKind;

struct _M42Box {
  BoxKind      kind;
  gboolean     tight;       /* BOX_FENCE: brackets drawn close in */
  PangoLayout *layout;      /* BOX_TEXT */
  GPtrArray   *children;
  double       size;        /* the font size this box was laid out at */
  char         open, close; /* BOX_FENCE: ( [ { | */
  guint        rows, cols;  /* BOX_GRID */
  const M42Plot *plot;      /* BOX_PLOT, borrowed from the value */
  double       r, g, b;     /* text colour */
  int          width, ascent, descent;
};

#define GAP(size)   ((int) ((size) * 0.18 + 1))
#define AXIS(size)  ((int) ((size) * 0.30))
#define PLOT_W 460
#define PLOT_H 300

static M42Box *box_from_node (GtkWidget *w, const M42Node *n, double size, int prec);

/* --- text ---------------------------------------------------------------- */

static M42Box *
box_new (BoxKind kind, double size)
{
  M42Box *b = g_new0 (M42Box, 1);
  b->kind = kind;
  b->size = size;
  b->children = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_box_free);
  return b;
}

void
m42_box_free (M42Box *box)
{
  if (box == NULL)
    return;
  g_clear_object (&box->layout);
  g_ptr_array_unref (box->children);
  g_free (box);
}

int m42_box_width (const M42Box *box)  { return box->width; }
int m42_box_height (const M42Box *box) { return box->ascent + box->descent; }

static M42Box *
box_text_full (GtkWidget *w, const char *text, double size, gboolean italic)
{
  M42Box *b = box_new (BOX_TEXT, size);
  PangoFontDescription *desc = pango_font_description_new ();
  PangoRectangle ink, log;

  b->layout = gtk_widget_create_pango_layout (w, text);
  pango_font_description_set_family (desc, "Cambria Math, STIX Two Math, DejaVu Serif, serif");
  pango_font_description_set_absolute_size (desc, size * PANGO_SCALE);
  if (italic)
    pango_font_description_set_style (desc, PANGO_STYLE_ITALIC);
  pango_layout_set_font_description (b->layout, desc);
  pango_font_description_free (desc);

  pango_layout_get_pixel_extents (b->layout, &ink, &log);
  b->width = log.width;
  b->ascent = PANGO_PIXELS (pango_layout_get_baseline (b->layout));
  b->descent = log.height - b->ascent;
  return b;
}

static M42Box *
box_text (GtkWidget *w, const char *text, double size)
{
  return box_text_full (w, text, size, FALSE);
}

/* The names that are really symbols. */
static const struct { const char *name, *glyph; } SYMBOLS[] = {
  { "Pi", "\317\200" }, { "pi", "\317\200" },
  { "Infinity", "\342\210\236" }, { "Inf", "\342\210\236" }, { "inf", "\342\210\236" },
  { "Degree", "\302\260" },
  { "EulerGamma", "\316\263" }, { "GoldenRatio", "\317\206" },
  { "Alpha", "\316\221" }, { "alpha", "\316\261" },
  { "Beta", "\316\222" },  { "beta", "\316\262" },
  { "Gamma", "\316\223" }, { "gamma", "\316\263" },
  { "Delta", "\316\224" }, { "delta", "\316\264" },
  { "Epsilon", "\316\225" }, { "epsilon", "\316\265" },
  { "Zeta", "\316\226" },  { "zeta", "\316\266" },
  { "Eta", "\316\227" },   { "eta", "\316\267" },
  { "Theta", "\316\230" }, { "theta", "\316\270" },
  { "Kappa", "\316\232" }, { "kappa", "\316\272" },
  { "Lambda", "\316\233" },{ "lambda", "\316\273" },
  { "Mu", "\316\234" },    { "mu", "\316\274" },
  { "Nu", "\316\235" },    { "nu", "\316\275" },
  { "Xi", "\316\236" },    { "xi", "\316\276" },
  { "Rho", "\316\241" },   { "rho", "\317\201" },
  { "Sigma", "\316\243" }, { "sigma", "\317\203" },
  { "Tau", "\316\244" },   { "tau", "\317\204" },
  { "Phi", "\316\246" },   { "phi", "\317\206" },
  { "Chi", "\316\247" },   { "chi", "\317\207" },
  { "Psi", "\316\250" },   { "psi", "\317\210" },
  { "Omega", "\316\251" }, { "omega", "\317\211" },
};

static M42Box *
box_ident (GtkWidget *w, const char *name, double size)
{
  for (guint i = 0; i < G_N_ELEMENTS (SYMBOLS); i++)
    if (strcmp (SYMBOLS[i].name, name) == 0)
      return box_text (w, SYMBOLS[i].glyph, size);
  /* One-letter names are variables and lean; longer ones are names of
   * things -- True, Null, a user function -- and stand upright. */
  return box_text_full (w, name, size, strlen (name) <= 2);
}

static M42Box *
box_number (GtkWidget *w, double x, double size)
{
  g_autoptr (GString) s = g_string_new (NULL);

  /* Infinity has a sign of its own. */
  if (isinf (x))
    return box_text (w, x > 0 ? "∞" : "−∞", size);
  m42_number_to_string (s, x);
  return box_text (w, s->str, size);
}

/* --- composites ----------------------------------------------------------- */

static void
row_add (M42Box *row, M42Box *child)
{
  g_ptr_array_add (row->children, child);
  row->width += child->width;
  row->ascent = MAX (row->ascent, child->ascent);
  row->descent = MAX (row->descent, child->descent);
}

static M42Box *
box_row (double size)
{
  return box_new (BOX_ROW, size);
}

static M42Box *
box_frac (M42Box *num, M42Box *den)
{
  M42Box *b = box_new (BOX_FRAC, num->size);
  int gap = GAP (b->size);

  g_ptr_array_add (b->children, num);
  g_ptr_array_add (b->children, den);
  b->width = MAX (num->width, den->width) + 2 * gap;
  b->ascent = m42_box_height (num) + gap + AXIS (b->size);
  b->descent = m42_box_height (den) + gap - AXIS (b->size);
  return b;
}

static M42Box *
box_power (M42Box *base, M42Box *exp)
{
  M42Box *b = box_new (BOX_POWER, base->size);
  int rise = (int) (base->ascent * 0.55 + 1);

  g_ptr_array_add (b->children, base);
  g_ptr_array_add (b->children, exp);
  b->width = base->width + exp->width + 1;
  b->ascent = MAX (base->ascent, rise + exp->ascent);
  b->descent = MAX (base->descent, exp->descent - rise);
  return b;
}

static M42Box *
box_sqrt (M42Box *child)
{
  M42Box *b = box_new (BOX_SQRT, child->size);
  int hook = (int) (child->size * 0.55) + 4;

  g_ptr_array_add (b->children, child);
  b->width = child->width + hook + 4;
  b->ascent = child->ascent + 5;
  b->descent = child->descent + 1;
  return b;
}

/* The integral sign, the summation sign and their relatives: the sign
 * itself, what is written under it and what is written over it.  What
 * the operator works on follows in the row, as it does on paper. */
static M42Box *
box_bigop (M42Box *sign, M42Box *under, M42Box *over)
{
  M42Box *b = box_new (BOX_BIGOP, sign->size);
  int width = sign->width;

  g_ptr_array_add (b->children, sign);
  g_ptr_array_add (b->children, under);
  g_ptr_array_add (b->children, over);

  if (under != NULL)
    width = MAX (width, under->width);
  if (over != NULL)
    width = MAX (width, over->width);
  b->width = width + 2;
  b->ascent = sign->ascent + (over != NULL ? m42_box_height (over) : 0);
  b->descent = sign->descent + (under != NULL ? m42_box_height (under) : 0);
  return b;
}

static M42Box *
box_fence_full (M42Box *child, char open, char close, gboolean tight)
{
  M42Box *b = box_new (BOX_FENCE, child->size);
  int w = (int) (child->size * (tight ? 0.20 : 0.30)) + 2;

  g_ptr_array_add (b->children, child);
  b->open = open;
  b->close = close;
  b->tight = tight;
  b->width = child->width + 2 * w + (tight ? 0 : 2);
  b->ascent = child->ascent + 1;
  b->descent = child->descent + 1;
  return b;
}

static M42Box *
box_fence (M42Box *child, char open, char close)
{
  return box_fence_full (child, open, close, FALSE);
}

/* --- from a value or a tree ------------------------------------------------- */

/* The same precedences the printer uses, so that a child that binds
 * less tightly than its parent is fenced. */
static int
node_precedence (const M42Node *n)
{
  switch (n->kind)
    {
    case M42_NODE_RULE:    return 3;
    case M42_NODE_BINARY:
      switch (n->op)
        {
        case M42_TOK_OR:  return 4;
        case M42_TOK_AND: return 5;
        case M42_TOK_EQ: case M42_TOK_NE: case M42_TOK_LT:
        case M42_TOK_LE: case M42_TOK_GT: case M42_TOK_GE: return 6;
        case M42_TOK_PLUS: case M42_TOK_MINUS: return 7;
        case M42_TOK_STAR: case M42_TOK_PERCENT: case M42_TOK_DOT:
        case M42_TOK_BACKSLASH: return 8;
        case M42_TOK_SLASH: return 12;   /* drawn as a fraction: never fenced */
        case M42_TOK_CARET: return 10;
        default: return 8;
        }
    case M42_NODE_UNARY:
      return n->op == M42_TOK_MINUS || n->op == M42_TOK_NOT ? 9 : 11;
    default:
      return 12;
    }
}

static const char *
binary_glyph (int op)
{
  switch (op)
    {
    case M42_TOK_PLUS:    return " + ";
    case M42_TOK_MINUS:   return " \342\210\222 ";   /* minus sign */
    case M42_TOK_PERCENT: return " mod ";
    case M42_TOK_DOT:     return " \302\267 ";       /* middle dot */
    case M42_TOK_BACKSLASH: return " \\ ";
    case M42_TOK_EQ:      return " = ";
    case M42_TOK_NE:      return " \342\211\240 ";
    case M42_TOK_LT:      return " < ";
    case M42_TOK_LE:      return " \342\211\244 ";
    case M42_TOK_GT:      return " > ";
    case M42_TOK_GE:      return " \342\211\245 ";
    case M42_TOK_AND:     return " \342\210\247 ";
    case M42_TOK_OR:      return " \342\210\250 ";
    default:              return " ? ";
    }
}

/* A child box, unfenced: the caller decides whether it needs brackets,
 * so that it does not get two sets of them. */
static M42Box *
box_child (GtkWidget *w, const M42Node *child, double size, int parent_prec)
{
  return box_from_node (w, child, size, 0);
}

static M42Box *
maybe_fence (M42Box *b, const M42Node *child, int parent_prec)
{
  if (node_precedence (child) < parent_prec ||
      (child->kind == M42_NODE_NUMBER && child->number < 0 && parent_prec >= 8))
    return box_fence (b, '(', ')');
  return b;
}

static M42Box *
box_call (GtkWidget *w, const M42Node *n, double size)
{
  const char *f = n->name;
  M42Box *row;

  /* Sqrt draws its radical. */
  if ((!strcmp (f, "Sqrt") || !strcmp (f, "sqrt")) && n->children->len == 1)
    return box_sqrt (box_from_node (w, m42_node_child (n, 0), size, 0));

  /* Not is a sign, not a name. */
  if (!strcmp (f, "Not") && n->children->len == 1)
    {
      row = box_row (size);
      row_add (row, box_text (w, "Â¬", size));
      row_add (row, box_from_node (w, m42_node_child (n, 0), size, 9));
      return row;
    }

  /* Abs draws its bars. */
  if ((!strcmp (f, "Abs") || !strcmp (f, "abs")) && n->children->len == 1)
    return box_fence (box_from_node (w, m42_node_child (n, 0), size, 0), '|', '|');

  /* An integral, with its bounds on the sign when it has them and the
   * d of the variable after the integrand, upright as it is set. */
  if ((!strcmp (f, "Integrate") || !strcmp (f, "NIntegrate") || !strcmp (f, "int")) &&
      n->children->len == 2)
    {
      const M42Node *spec = m42_node_child (n, 1);
      const M42Node *var = spec;
      M42Box *sign = box_text (w, "\342\210\253", size * 2.0);   /* integral sign */
      M42Box *under = NULL, *over = NULL;

      if (spec->kind == M42_NODE_LIST && spec->children->len == 3)
        {
          var = m42_node_child (spec, 0);
          under = box_from_node (w, m42_node_child (spec, 1), size * 0.62, 0);
          over = box_from_node (w, m42_node_child (spec, 2), size * 0.62, 0);
        }

      row = box_row (size);
      row_add (row, box_bigop (sign, under, over));
      row_add (row, box_from_node (w, m42_node_child (n, 0), size, 8));
      row_add (row, box_text (w, "\342\200\211", size));
      row_add (row, box_text_full (w, "d", size, FALSE));
      row_add (row, box_from_node (w, var, size, 12));
      return row;
    }

  /* A sum or a product: the sign, with the iterator under it and where
   * it stops written over it. */
  if ((!strcmp (f, "Sum") || !strcmp (f, "Product")) && n->children->len == 2 &&
      m42_node_child (n, 1)->kind == M42_NODE_LIST)
    {
      const M42Node *spec = m42_node_child (n, 1);
      M42Box *sign = box_text (w, !strcmp (f, "Sum") ? "\342\210\221" : "\342\210\217", size * 1.7);
      M42Box *under = NULL, *over = NULL;

      if (spec->children->len >= 2)
        {
          M42Box *bottom = box_row (size * 0.62);

          row_add (bottom, box_from_node (w, m42_node_child (spec, 0), size * 0.62, 12));
          if (spec->children->len >= 3)
            {
              row_add (bottom, box_text (w, " = ", size * 0.62));
              row_add (bottom, box_from_node (w, m42_node_child (spec, 1), size * 0.62, 0));
              over = box_from_node (w, m42_node_child (spec, 2), size * 0.62, 0);
            }
          else
            over = box_from_node (w, m42_node_child (spec, 1), size * 0.62, 0);
          under = bottom;
        }

      row = box_row (size);
      row_add (row, box_bigop (sign, under, over));
      row_add (row, box_from_node (w, m42_node_child (n, 0), size, 8));
      return row;
    }

  /* A limit, with what tends to what written under the word. */
  if (!strcmp (f, "Limit") && n->children->len == 2)
    {
      M42Box *word = box_text_full (w, "lim", size, FALSE);
      M42Box *under = box_from_node (w, m42_node_child (n, 1), size * 0.62, 0);

      row = box_row (size);
      row_add (row, box_bigop (word, under, NULL));
      row_add (row, box_text (w, "\342\200\211", size));
      row_add (row, box_from_node (w, m42_node_child (n, 0), size, 8));
      return row;
    }

  /* The derivative that was left as it was written, in Leibniz's
   * notation, with the order raised when there is one. */
  if ((!strcmp (f, "D") || !strcmp (f, "Derivative")) && n->children->len == 2)
    {
      const M42Node *spec = m42_node_child (n, 1);
      const M42Node *var = spec;
      const M42Node *order = NULL;
      M42Box *num, *den;

      if (spec->kind == M42_NODE_LIST && spec->children->len == 2)
        {
          var = m42_node_child (spec, 0);
          order = m42_node_child (spec, 1);
        }

      num = order != NULL
        ? box_power (box_text_full (w, "d", size, FALSE),
                     box_from_node (w, order, size * 0.7, 0))
        : box_text_full (w, "d", size, FALSE);
      den = box_row (size);
      row_add (den, box_text_full (w, "d", size, FALSE));
      row_add (den, box_from_node (w, var, size, 12));
      if (order != NULL)
        {
          M42Box *raised = box_power (box_row (size), box_from_node (w, order, size * 0.7, 0));
          row_add (den, raised);
        }

      row = box_row (size);
      row_add (row, box_frac (num, den));
      row_add (row, box_fence (box_from_node (w, m42_node_child (n, 0), size, 0), '(', ')'));
      return row;
    }

  row = box_row (size);
  row_add (row, box_text_full (w, f, size, FALSE));
  {
    M42Box *inner = box_row (size);
    for (guint i = 0; i < n->children->len; i++)
      {
        if (i > 0)
          row_add (inner, box_text (w, ", ", size));
        row_add (inner, box_from_node (w, m42_node_child (n, i), size, 0));
      }
    row_add (row, box_fence_full (inner, '[', ']', TRUE));
  }
  return row;
}

static M42Box *
box_from_node (GtkWidget *w, const M42Node *n, double size, int parent_prec)
{
  M42Box *b;

  switch (n->kind)
    {
    case M42_NODE_NUMBER:
      b = box_number (w, n->number, size);
      break;

    case M42_NODE_IDENT:
      b = box_ident (w, n->name, size);
      break;

    case M42_NODE_STRING:
      {
        g_autofree char *quoted = g_strdup_printf ("“%s”", n->name);
        b = box_text_full (w, quoted, size, FALSE);
      }
      break;

    case M42_NODE_UNARY:
      {
        M42Box *row = box_row (size);
        if (n->op == M42_TOK_MINUS)
          row_add (row, box_text (w, "\342\210\222", size));
        else if (n->op == M42_TOK_NOT)
          row_add (row, box_text (w, "\302\254", size));
        row_add (row, maybe_fence (box_child (w, m42_node_child (n, 0), size, 9),
                                   m42_node_child (n, 0), 9));
        if (n->op == M42_TOK_BANG)
          row_add (row, box_text (w, "!", size));
        else if (n->op == M42_TOK_QUOTE)
          row_add (row, box_text (w, "\341\265\200", size));   /* superscript T */
        b = row;
        break;
      }

    case M42_NODE_BINARY:
      {
        const M42Node *a = m42_node_child (n, 0), *c = m42_node_child (n, 1);
        int prec = node_precedence (n);

        if (n->op == M42_TOK_SLASH)
          {
            b = box_frac (box_from_node (w, a, size, 0), box_from_node (w, c, size, 0));
            break;
          }
        if (n->op == M42_TOK_CARET)
          {
            M42Box *base = maybe_fence (box_child (w, a, size, 11), a, 11);
            b = box_power (base, box_from_node (w, c, size * 0.72, 0));
            break;
          }
        {
          M42Box *row = box_row (size);
          row_add (row, maybe_fence (box_child (w, a, size, prec), a, prec));
          if (n->op == M42_TOK_STAR)
            row_add (row, box_text (w, c->kind == M42_NODE_NUMBER ? " \303\227 " : "\342\200\211", size));
          else
            row_add (row, box_text (w, binary_glyph (n->op), size));
          /* Where the grouping makes a difference -- a - (b - c) --
           * the right operand is fenced one step tighter.  For + and
           * *, where it makes none, it is not: 6 (x y^2) should be
           * written 6 x y^2, as the printed form has it. */
          {
            int right_prec = (n->op == M42_TOK_MINUS || n->op == M42_TOK_SLASH ||
                              n->op == M42_TOK_PERCENT) ? prec + 1 : prec;

            row_add (row, maybe_fence (box_child (w, c, size, right_prec), c, right_prec));
          }
          b = row;
        }
        break;
      }

    case M42_NODE_CALL:
      b = box_call (w, n, size);
      break;

    case M42_NODE_LIST:
      {
        M42Box *inner = box_row (size);
        for (guint i = 0; i < n->children->len; i++)
          {
            if (i > 0)
              row_add (inner, box_text (w, ", ", size));
            row_add (inner, box_from_node (w, m42_node_child (n, i), size, 0));
          }
        b = box_fence (inner, '{', '}');
        break;
      }

    case M42_NODE_RULE:
      {
        M42Box *row = box_row (size);
        row_add (row, box_from_node (w, m42_node_child (n, 0), size, 3));
        /* x -> 2 works its right side out now; x :> 2 keeps it as
         * written, and is shown with the colon it is typed with. */
        row_add (row, box_text (w, n->op ? " :\342\206\222 " : " \342\206\222 ", size));
        row_add (row, box_from_node (w, m42_node_child (n, 1), size, 3));
        b = row;
        break;
      }

    default:
      {
        g_autoptr (GString) s = g_string_new (NULL);
        m42_node_to_string (s, n);
        b = box_text (w, s->str, size);
        break;
      }
    }

  return maybe_fence (b, n, parent_prec);
}

static M42Box *box_from_value (GtkWidget *w, const M42Value *v, double size);

/* A matrix as a grid inside square brackets. */
static M42Box *
box_matrix (GtkWidget *w, const M42Value *v, double size, guint rows, guint cols)
{
  M42Box *b = box_new (BOX_GRID, size);
  int pad = GAP (size) * 2;
  g_autofree int *colw = g_new0 (int, cols);
  int total_h = 0;

  b->rows = rows;
  b->cols = cols;
  for (guint i = 0; i < rows; i++)
    {
      M42Value *row = m42_value_list_nth (v, i);
      int row_h = 0;
      for (guint j = 0; j < cols; j++)
        {
          M42Box *cell = box_from_value (w, m42_value_list_nth (row, j), size);
          g_ptr_array_add (b->children, cell);
          colw[j] = MAX (colw[j], cell->width);
          row_h = MAX (row_h, m42_box_height (cell));
        }
      total_h += row_h + (i + 1 < rows ? pad : 0);
    }

  b->width = 2 * ((int) (size * 0.32) + 3);
  for (guint j = 0; j < cols; j++)
    b->width += colw[j] + (j + 1 < cols ? pad * 2 : 0);
  b->ascent = total_h / 2 + AXIS (size) + pad;
  b->descent = total_h - total_h / 2 - AXIS (size) + pad;
  return b;
}

static M42Box *
box_from_value (GtkWidget *w, const M42Value *v, double size)
{
  guint rows, cols;

  switch (v->kind)
    {
    case M42_VALUE_NUMBER:
      /* An exact fraction is stacked, the way it is written by hand. */
      if (v->exact && v->den != 1)
        return box_frac (box_number (w, (double) v->num, size),
                         box_number (w, (double) v->den, size));
      return box_number (w, v->u.number, size);

    case M42_VALUE_COMPLEX:
      {
        /* 3 + 4 i, with the upright double-struck i mathematics uses. */
        M42Box *row = box_row (size);

        if (v->u.cx.re != 0)
          {
            row_add (row, box_number (w, v->u.cx.re, size));
            row_add (row, box_text (w, v->u.cx.im < 0 ? " \342\210\222 " : " + ", size));
          }
        else if (v->u.cx.im < 0)
          row_add (row, box_text (w, "\342\210\222", size));
        if (fabs (v->u.cx.im) != 1)
          {
            row_add (row, box_number (w, fabs (v->u.cx.im), size));
            row_add (row, box_text (w, "\342\200\211", size));
          }
        /* The upright I the user types: the double-struck one that
         * printed mathematics uses is missing from too many fonts. */
        row_add (row, box_text_full (w, "I", size, FALSE));
        return row;
      }

    case M42_VALUE_STRING:
      {
        g_autofree char *quoted = g_strdup_printf ("“%s”", v->u.string);
        return box_text_full (w, quoted, size, FALSE);
      }

    case M42_VALUE_EXPR:
      return box_from_node (w, v->u.expr, size, 0);

    case M42_VALUE_LIST:
      if (m42_value_is_matrix (v, &rows, &cols) && rows > 1)
        return box_matrix (w, v, size, rows, cols);
      {
        M42Box *inner = box_row (size);
        guint n = m42_value_list_length (v);

        for (guint i = 0; i < n; i++)
          {
            if (n > M42_LIST_SHOWN && i == M42_LIST_SHOWN - 4)
              {
                g_autofree char *more = g_strdup_printf (", … %u more … ",
                                                         n - M42_LIST_SHOWN);
                row_add (inner, box_text (w, more, size));
                i = n - 4;
              }
            if (i > 0)
              row_add (inner, box_text (w, ", ", size));
            row_add (inner, box_from_value (w, m42_value_list_nth (v, i), size));
          }
        return box_fence (inner, '{', '}');
      }

    case M42_VALUE_PLOT:
      {
        M42Box *b = box_new (BOX_PLOT, size);
        b->plot = v->u.plot;
        b->width = PLOT_W;
        b->ascent = PLOT_H;
        b->descent = 0;
        return b;
      }

    default:
      {
        g_autofree char *text = m42_value_to_string (v);
        return box_text (w, text, size);
      }
    }
}

M42Box *
m42_box_from_value (GtkWidget *widget, const M42Value *v, double size)
{
  return box_from_value (widget, v, size);
}

/* --- drawing ---------------------------------------------------------------- */

static void draw_box (const M42Box *b, cairo_t *cr, double x, double baseline);

/* A bracket that grows with what it holds, drawn rather than set in
 * type so that it matches a tall fraction or a matrix. */
static void
draw_fence (cairo_t *cr, char which, double x, double top, double bottom, double size)
{
  double h = bottom - top, mid = (top + bottom) / 2;
  double w = size * 0.30;
  double lw = MAX (1.0, size * 0.06);

  cairo_save (cr);
  cairo_set_line_width (cr, lw);
  cairo_new_path (cr);
  switch (which)
    {
    case '|':
      cairo_move_to (cr, x + w / 2, top);
      cairo_line_to (cr, x + w / 2, bottom);
      break;
    case '[':
      cairo_move_to (cr, x + w, top);
      cairo_line_to (cr, x + lw / 2, top);
      cairo_line_to (cr, x + lw / 2, bottom);
      cairo_line_to (cr, x + w, bottom);
      break;
    case ']':
      cairo_move_to (cr, x, top);
      cairo_line_to (cr, x + w - lw / 2, top);
      cairo_line_to (cr, x + w - lw / 2, bottom);
      cairo_line_to (cr, x, bottom);
      break;
    case '(':
      cairo_move_to (cr, x + w, top);
      cairo_curve_to (cr, x + w * 0.1, top + h * 0.2, x + w * 0.1, bottom - h * 0.2, x + w, bottom);
      break;
    case ')':
      cairo_move_to (cr, x, top);
      cairo_curve_to (cr, x + w * 0.9, top + h * 0.2, x + w * 0.9, bottom - h * 0.2, x, bottom);
      break;
    /* A brace is two arcs meeting at a point halfway up, which is what
     * tells it apart from a parenthesis at a glance. */
    case '{':
      cairo_move_to (cr, x + w, top);
      cairo_curve_to (cr, x + w * 0.55, top + h * 0.06, x + w * 0.55, mid - h * 0.16, x + w * 0.5, mid - h * 0.02);
      cairo_line_to (cr, x + lw * 0.5, mid);
      cairo_line_to (cr, x + w * 0.5, mid + h * 0.02);
      cairo_curve_to (cr, x + w * 0.55, mid + h * 0.16, x + w * 0.55, bottom - h * 0.06, x + w, bottom);
      break;
    case '}':
      cairo_move_to (cr, x, top);
      cairo_curve_to (cr, x + w * 0.45, top + h * 0.06, x + w * 0.45, mid - h * 0.16, x + w * 0.5, mid - h * 0.02);
      cairo_line_to (cr, x + w - lw * 0.5, mid);
      cairo_line_to (cr, x + w * 0.5, mid + h * 0.02);
      cairo_curve_to (cr, x + w * 0.45, mid + h * 0.16, x + w * 0.45, bottom - h * 0.06, x, bottom);
      break;
    default:
      break;
    }
  cairo_stroke (cr);
  cairo_restore (cr);
}

/* --- graphs ------------------------------------------------------------------ */

/* A step for the ticks that lands on 1, 2 or 5 times a power of ten. */
static double
nice_step (double span)
{
  double raw = span / 6.0;
  double mag = pow (10, floor (log10 (raw > 0 ? raw : 1)));
  double norm = raw / mag;

  if (norm < 1.5) return mag;
  if (norm < 3)   return 2 * mag;
  if (norm < 7)   return 5 * mag;
  return 10 * mag;
}

/* --- surfaces -------------------------------------------------------------
 *
 * A surface is drawn in the projection a draughtsman would use: the
 * box turned a little to the left and tilted a little forward, its
 * quadrilaterals painted from the far corner towards the near one so
 * that the near ones cover what is behind them.  Each is filled with a
 * colour taken from its height and darkened by how it faces the light,
 * and outlined so the mesh shows.
 */

/* Which way the box is looked at.  The pair matters more than it
 * looks: at a yaw of 0.62 and a tilt of 0.52 the eye lies almost
 * exactly in the plane z = x, so anything flat in that plane -- the
 * surface z = x, or the curve {Sin[t], Cos[t], Sin[t]} -- is seen
 * edge on and drawn as a line.  These two are well clear of that
 * plane and of z = y. */
#define VIEW_YAW   0.70      /* radians the box is turned */
#define VIEW_TILT  0.40      /* radians it is tilted forward */

/* A number cut down to a few figures, so that an axis says 3 and not
 * 2.99999999999999. */
static double
round_significant (double x, int digits)
{
  double mag;

  if (x == 0 || !isfinite (x))
    return x;
  mag = pow (10, floor (log10 (fabs (x))) - digits + 1);
  return round (x / mag) * mag;
}

/* A point of the unit cube, in the picture. */
static void
project (double x, double y, double z, double *px, double *py)
{
  double cy = cos (VIEW_YAW), sy = sin (VIEW_YAW);
  double ct = cos (VIEW_TILT), st = sin (VIEW_TILT);
  double rx = x * cy - y * sy;
  double ry = x * sy + y * cy;

  *px = rx;
  *py = ry * st - z * ct;
}

/* How far a point is from the eye, for the painter. */
static double
depth_of (double x, double y, double z)
{
  return x * sin (VIEW_YAW) + y * cos (VIEW_YAW) + z * 0.2;
}

typedef struct {
  double depth;
  guint  i, j;
} Quad;

static int
compare_quads (gconstpointer a, gconstpointer b)
{
  const Quad *x = a, *y = b;
  return x->depth < y->depth ? -1 : x->depth > y->depth ? 1 : 0;
}

/* A height between 0 and 1 as a colour, cool below and warm above. */
static void
height_colour (double t, double *r, double *g, double *b)
{
  const double lo[3] = { 0.25, 0.35, 0.68 };
  const double mid[3] = { 0.55, 0.72, 0.72 };
  const double hi[3] = { 0.90, 0.62, 0.24 };
  const double *from = t < 0.5 ? lo : mid;
  const double *to = t < 0.5 ? mid : hi;
  double u = t < 0.5 ? t * 2 : (t - 0.5) * 2;

  *r = from[0] + (to[0] - from[0]) * u;
  *g = from[1] + (to[1] - from[1]) * u;
  *b = from[2] + (to[2] - from[2]) * u;
}

/* A curve through space, drawn in the same projection the surfaces
 * use: the box it lives in is squashed to the cube from -1 to 1 so
 * that whatever it is made of fills the room it has, and a floor is
 * laid under it to say which way is down. */
static void
draw_curve3d (const M42Plot *p, cairo_t *cr, double x0, double y0, double w, double h)
{
  double scale, cx, cy;
  double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;

  for (int i = 0; i < 8; i++)
    {
      double px, py;

      project (i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1, &px, &py);
      minx = MIN (minx, px); maxx = MAX (maxx, px);
      miny = MIN (miny, py); maxy = MAX (maxy, py);
    }
  scale = MIN (w / (maxx - minx), h / (maxy - miny)) * 0.94;
  cx = x0 + w / 2 - (minx + maxx) / 2 * scale;
  cy = y0 + h / 2 - (miny + maxy) / 2 * scale;

  {
    double px[4], py[4];

    project (-1, -1, -1, &px[0], &py[0]);
    project (1, -1, -1, &px[1], &py[1]);
    project (1, 1, -1, &px[2], &py[2]);
    project (-1, 1, -1, &px[3], &py[3]);
    cairo_set_source_rgb (cr, 0.96, 0.96, 0.97);
    cairo_move_to (cr, cx + px[0] * scale, cy + py[0] * scale);
    for (int k = 1; k < 4; k++)
      cairo_line_to (cr, cx + px[k] * scale, cy + py[k] * scale);
    cairo_close_path (cr);
    cairo_fill_preserve (cr);
    cairo_set_source_rgb (cr, 0.75, 0.75, 0.78);
    cairo_set_line_width (cr, 1);
    cairo_stroke (cr);
  }

  for (guint c = 0; c < p->curves->len; c++)
    {
      const M42Curve3D *curve = g_ptr_array_index (p->curves, c);
      guint n = curve->points->len / 3;
      double sx = curve->xmax > curve->xmin ? 2 / (curve->xmax - curve->xmin) : 0;
      double sy = curve->ymax > curve->ymin ? 2 / (curve->ymax - curve->ymin) : 0;
      double sz = curve->zmax > curve->zmin ? 2 / (curve->zmax - curve->zmin) : 0;

      cairo_set_line_width (cr, 1.6);
      for (guint i = 0; i < n; i++)
        {
          const double *at = &g_array_index (curve->points, double, i * 3);
          double x = -1 + (at[0] - curve->xmin) * sx;
          double y = -1 + (at[1] - curve->ymin) * sy;
          double z = -1 + (at[2] - curve->zmin) * sz;
          double px, py;

          project (x, y, z, &px, &py);
          if (i == 0)
            cairo_move_to (cr, cx + px * scale, cy + py * scale);
          else
            cairo_line_to (cr, cx + px * scale, cy + py * scale);
        }
      cairo_stroke (cr);
    }
}

static void
draw_surface (const M42Plot *p, cairo_t *cr, double x0, double y0, double w, double h)
{
  const M42Surface *s = p->surface;
  guint nx = s->nx, ny = s->ny;
  g_autoptr (GArray) quads = g_array_new (FALSE, FALSE, sizeof (Quad));
  double scale, cx, cy;
  double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;

  /* Looked straight down on: a patch of colour for each cell, with the
   * same colours the projection uses for height.  There is no mesh and
   * no shading, because there is nothing to shade -- the picture is
   * the colour. */
  if (s->flat)
    {
      double cw = w / (double) (nx - 1), ch = h / (double) (ny - 1);

      for (guint i = 0; i + 1 < nx; i++)
        for (guint j = 0; j + 1 < ny; j++)
          {
            double z00 = s->z[i * ny + j], z10 = s->z[(i + 1) * ny + j];
            double z01 = s->z[i * ny + j + 1], z11 = s->z[(i + 1) * ny + j + 1];
            double middle = (z00 + z10 + z01 + z11) / 4;
            double r, g, b, t;

            if (!isfinite (middle))
              continue;
            t = s->zmax > s->zmin ? (middle - s->zmin) / (s->zmax - s->zmin) : 0.5;
            height_colour (CLAMP (t, 0, 1), &r, &g, &b);
            cairo_set_source_rgb (cr, r, g, b);
            /* Up the page is the larger y, as it is on every other
             * graph math42 draws. */
            cairo_rectangle (cr, x0 + i * cw, y0 + h - (j + 1) * ch, cw + 0.5, ch + 0.5);
            cairo_fill (cr);
          }
      cairo_set_source_rgb (cr, 0.55, 0.55, 0.58);
      cairo_set_line_width (cr, 1);
      cairo_rectangle (cr, x0, y0, w, h);
      cairo_stroke (cr);
      return;
    }

  /* Where the corners of the box land, so that the picture can be
   * scaled to fit whatever room it has. */
  for (int i = 0; i < 8; i++)
    {
      double px, py;
      project (i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1, &px, &py);
      minx = MIN (minx, px); maxx = MAX (maxx, px);
      miny = MIN (miny, py); maxy = MAX (maxy, py);
    }
  scale = MIN (w / (maxx - minx), h / (maxy - miny)) * 0.94;
  cx = x0 + w / 2 - (minx + maxx) / 2 * scale;
  cy = y0 + h / 2 - (miny + maxy) / 2 * scale;

#define GRID_X(i) (-1.0 + 2.0 * (i) / (double) (nx - 1))
#define GRID_Y(j) (-1.0 + 2.0 * (j) / (double) (ny - 1))
#define GRID_Z(i, j) (isfinite (s->z[(i) * ny + (j)]) \
                      ? -1.0 + 2.0 * (s->z[(i) * ny + (j)] - s->zmin) / (s->zmax - s->zmin) \
                      : NAN)

  for (guint i = 0; i + 1 < nx; i++)
    for (guint j = 0; j + 1 < ny; j++)
      {
        Quad q;
        double z = GRID_Z (i, j);

        if (!isfinite (z))
          continue;
        q.i = i;
        q.j = j;
        q.depth = depth_of (GRID_X (i) + 1.0 / nx, GRID_Y (j) + 1.0 / ny, z);
        g_array_append_val (quads, q);
      }
  g_array_sort (quads, compare_quads);

  /* The floor of the box, so the surface has something to stand on. */
  {
    double px[4], py[4];
    project (-1, -1, -1, &px[0], &py[0]);
    project (1, -1, -1, &px[1], &py[1]);
    project (1, 1, -1, &px[2], &py[2]);
    project (-1, 1, -1, &px[3], &py[3]);
    cairo_set_source_rgb (cr, 0.96, 0.96, 0.97);
    cairo_move_to (cr, cx + px[0] * scale, cy + py[0] * scale);
    for (int k = 1; k < 4; k++)
      cairo_line_to (cr, cx + px[k] * scale, cy + py[k] * scale);
    cairo_close_path (cr);
    cairo_fill_preserve (cr);
    cairo_set_source_rgb (cr, 0.75, 0.75, 0.78);
    cairo_set_line_width (cr, 1);
    cairo_stroke (cr);
  }

  for (guint k = 0; k < quads->len; k++)
    {
      Quad *q = &g_array_index (quads, Quad, k);
      guint i = q->i, j = q->j;
      double zs[4] = { GRID_Z (i, j), GRID_Z (i + 1, j), GRID_Z (i + 1, j + 1), GRID_Z (i, j + 1) };
      double xs[4] = { GRID_X (i), GRID_X (i + 1), GRID_X (i + 1), GRID_X (i) };
      double ys[4] = { GRID_Y (j), GRID_Y (j), GRID_Y (j + 1), GRID_Y (j + 1) };
      double r, g, b, shade, t;
      gboolean whole = TRUE;

      for (int c = 0; c < 4; c++)
        if (!isfinite (zs[c]))
          whole = FALSE;
      if (!whole)
        continue;

      t = ((zs[0] + zs[1] + zs[2] + zs[3]) / 4 + 1) / 2;
      height_colour (CLAMP (t, 0, 1), &r, &g, &b);

      /* A cheap normal: how steep the quad is across and along. */
      {
        double dzx = (zs[1] - zs[0]) * nx / 2.0;
        double dzy = (zs[3] - zs[0]) * ny / 2.0;
        shade = 1.0 / sqrt (1 + dzx * dzx * 0.02 + dzy * dzy * 0.02);
        shade = 0.55 + 0.45 * shade;
      }

      cairo_new_path (cr);
      for (int c = 0; c < 4; c++)
        {
          double px, py;
          project (xs[c], ys[c], zs[c], &px, &py);
          if (c == 0)
            cairo_move_to (cr, cx + px * scale, cy + py * scale);
          else
            cairo_line_to (cr, cx + px * scale, cy + py * scale);
        }
      cairo_close_path (cr);
      cairo_set_source_rgb (cr, r * shade, g * shade, b * shade);
      cairo_fill_preserve (cr);
      cairo_set_source_rgba (cr, 0.2, 0.2, 0.25, 0.35);
      cairo_set_line_width (cr, 0.4);
      cairo_stroke (cr);
    }

  /* The three edges of the box that stand behind the surface, and the
   * numbers at their ends. */
  {
    PangoLayout *label = pango_cairo_create_layout (cr);
    PangoFontDescription *desc = pango_font_description_new ();
    const double corners[3][3] = { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 } };
    double px, py, qx, qy;

    pango_font_description_set_family (desc, "DejaVu Sans, sans-serif");
    pango_font_description_set_absolute_size (desc, 10 * PANGO_SCALE);
    pango_layout_set_font_description (label, desc);

    cairo_set_source_rgb (cr, 0.45, 0.45, 0.5);
    cairo_set_line_width (cr, 1);
    for (int e = 0; e < 2; e++)
      {
        project (corners[e][0], corners[e][1], corners[e][2], &px, &py);
        project (corners[e + 1][0], corners[e + 1][1], corners[e + 1][2], &qx, &qy);
        cairo_move_to (cr, cx + px * scale, cy + py * scale);
        cairo_line_to (cr, cx + qx * scale, cy + qy * scale);
      }
    cairo_stroke (cr);

    /* Three numbers along each of the two front edges and the upright
     * one, set just outside the box so that nothing sits on top of the
     * surface, with the name of the variable further out again. */
    {
      /* The upright edge is hung on whichever corner of the box falls
       * furthest to the left, so its numbers stand clear of the
       * surface rather than over it. */
      double zx = -1, zy = -1, best = 1e30;

      for (int c = 0; c < 4; c++)
        {
          double corner_x = c & 1 ? 1 : -1, corner_y = c & 2 ? 1 : -1;

          project (corner_x, corner_y, 0, &px, &py);
          if (px < best)
            {
              best = px;
              zx = corner_x;
              zy = corner_y;
            }
        }

      struct {
        double from[3], to[3];     /* the edge, in box coordinates */
        double lo, hi;             /* what it runs between */
        const char *name;
        gboolean sideways;         /* labels pushed left, not outward */
      } axes[3] = {
        { { -1, -1, -1 }, { 1, -1, -1 }, s->xmin, s->xmax, p->xlabel, FALSE },
        { { -1, -1, -1 }, { -1, 1, -1 }, s->ymin, s->ymax, p->ylabel, FALSE },
        { { zx, zy, -1 }, { zx, zy, 1 }, s->zmin, s->zmax, NULL, TRUE },
      };

      for (guint a = 0; a < 3; a++)
        {
          for (int step = 0; step <= 2; step++)
            {
              double t = step / 2.0;
              double bx = axes[a].from[0] + (axes[a].to[0] - axes[a].from[0]) * t;
              double by = axes[a].from[1] + (axes[a].to[1] - axes[a].from[1]) * t;
              double bz = axes[a].from[2] + (axes[a].to[2] - axes[a].from[2]) * t;
              double ox, oy, len;
              g_autoptr (GString) text = g_string_new (NULL);
              int tw, th;

              /* Outward, away from the middle of the box, measured in
               * the picture rather than in the box, so that a number
               * never lands on the surface. */
              project (bx, by, bz, &px, &py);
              if (axes[a].sideways)
                {
                  ox = -1;
                  oy = 0;
                }
              else
                {
                  project (0, 0, 0, &qx, &qy);
                  ox = px - qx;
                  oy = py - qy;
                }
              len = hypot (ox, oy);
              if (len < 1e-9)
                {
                  ox = -1;
                  oy = 0;
                  len = 1;
                }
              ox = ox / len * 20;
              oy = oy / len * 20;

              m42_number_to_string (text, round_significant (axes[a].lo +
                                                             (axes[a].hi - axes[a].lo) * t, 3));
              pango_layout_set_text (label, text->str, -1);
              pango_layout_get_pixel_size (label, &tw, &th);
              cairo_set_source_rgb (cr, 0.4, 0.4, 0.45);
              cairo_move_to (cr, cx + px * scale + ox - tw / 2.0,
                             cy + py * scale + oy - th / 2.0);
              pango_cairo_show_layout (cr, label);

              /* A small mark on the edge itself. */
              cairo_set_line_width (cr, 1);
              cairo_move_to (cr, cx + px * scale, cy + py * scale);
              cairo_line_to (cr, cx + px * scale + ox * 0.25, cy + py * scale + oy * 0.25);
              cairo_stroke (cr);
            }

          if (axes[a].name != NULL)
            {
              double mx = (axes[a].from[0] + axes[a].to[0]) / 2 * 1.55;
              double my = (axes[a].from[1] + axes[a].to[1]) / 2 * 1.55;
              int tw, th;

              project (mx, my, -1.1, &px, &py);
              pango_layout_set_text (label, axes[a].name, -1);
              pango_layout_get_pixel_size (label, &tw, &th);
              cairo_set_source_rgb (cr, 0.2, 0.2, 0.25);
              cairo_move_to (cr, cx + px * scale - tw / 2.0, cy + py * scale - th / 2.0);
              pango_cairo_show_layout (cr, label);
            }
        }
    }

    g_object_unref (label);
    pango_font_description_free (desc);
  }
#undef GRID_X
#undef GRID_Y
#undef GRID_Z
}

static void
draw_plot (const M42Box *b, cairo_t *cr, double x0, double y0)
{
  const M42Plot *p = b->plot;
  const double L = 46, R = 12, T = p->title != NULL ? 24 : 10, B = p->xlabel != NULL ? 44 : 30;
  double w = PLOT_W - L - R, h = PLOT_H - T - B;
  double sx = w / (p->xmax - p->xmin), sy = h / (p->ymax - p->ymin);
  PangoLayout *label;
  PangoFontDescription *desc = pango_font_description_new ();

  pango_font_description_set_family (desc, "DejaVu Sans, sans-serif");
  pango_font_description_set_absolute_size (desc, 10 * PANGO_SCALE);

  cairo_save (cr);
  cairo_translate (cr, x0, y0);

  /* The frame. */
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_rectangle (cr, L, T, w, h);
  cairo_fill (cr);

  /* A curve through space is drawn in the projection too. */
  if (p->curves != NULL && p->curves->len > 0)
    {
      draw_curve3d (p, cr, 8, T + 2, PLOT_W - 16, h - 6);
      pango_font_description_free (desc);
      cairo_restore (cr);
      return;
    }

  /* A graph of two variables is a different picture altogether --
   * unless it is looked straight down on, which wants the axes and the
   * frame every flat graph has, with the colour painted inside them. */
  if (p->surface != NULL && !p->surface->flat)
    {
      draw_surface (p, cr, 8, T + 2, PLOT_W - 16, h - 6);
      if (p->title != NULL)
        {
          int tw, th;
          PangoLayout *heading = pango_cairo_create_layout (cr);

          pango_layout_set_font_description (heading, desc);
          pango_layout_set_text (heading, p->title, -1);
          pango_layout_get_pixel_size (heading, &tw, &th);
          cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
          cairo_move_to (cr, (PLOT_W - tw) / 2.0, 0);
          pango_cairo_show_layout (cr, heading);
          g_object_unref (heading);
        }
      pango_font_description_free (desc);
      cairo_restore (cr);
      return;
    }

  /* Looked down on: the colour goes in first, and the axes over it. */
  if (p->surface != NULL && p->surface->flat)
    draw_surface (p, cr, L, T, w, h);

  /* A field of arrows, drawn where the points of a graph would go:
   * a line each way it points with a head on the end, dark where the
   * field is strong and pale where it is weak. */
  if (p->arrows != NULL)
    for (guint i = 0; i + 4 < p->arrows->len; i += 5)
      {
        const double *a = &g_array_index (p->arrows, double, i);
        double px = L + (a[0] - p->xmin) * sx;
        double py = T + h - (a[1] - p->ymin) * sy;
        double qx = L + (a[0] + a[2] - p->xmin) * sx;
        double qy = T + h - (a[1] + a[3] - p->ymin) * sy;
        double dx = qx - px, dy = qy - py;
        double len = hypot (dx, dy);
        double head = MIN (len * 0.42, 5.0);
        double strength = a[4];

        if (len < 0.6)
          continue;
        cairo_set_source_rgb (cr, 0.20 + 0.45 * (1 - strength),
                              0.35 + 0.40 * (1 - strength),
                              0.70 - 0.10 * strength);
        cairo_set_line_width (cr, 1.1);
        cairo_move_to (cr, px, py);
        cairo_line_to (cr, qx, qy);
        cairo_stroke (cr);

        /* The head: two short strokes back from the point. */
        {
          double ux = dx / len, uy = dy / len;
          double nx = -uy, ny = ux;

          cairo_move_to (cr, qx, qy);
          cairo_line_to (cr, qx - ux * head + nx * head * 0.5,
                             qy - uy * head + ny * head * 0.5);
          cairo_move_to (cr, qx, qy);
          cairo_line_to (cr, qx - ux * head - nx * head * 0.5,
                             qy - uy * head - ny * head * 0.5);
          cairo_stroke (cr);
        }
      }

  label = pango_cairo_create_layout (cr);
  pango_layout_set_font_description (label, desc);

  /* Ticks, and a light rule at each. */
  cairo_set_line_width (cr, 1);
  for (int axis = 0; axis < 2; axis++)
    {
      double lo = axis == 0 ? p->xmin : p->ymin;
      double hi = axis == 0 ? p->xmax : p->ymax;
      double step = nice_step (hi - lo);

      /* A logarithmic axis is marked at whole powers of ten, written
       * as the numbers they stand for. */
      gboolean logarithmic = axis == 0 ? p->log_x : p->log_y;

      if (logarithmic)
        step = MAX (1.0, round (step));

      for (double t = ceil (lo / step) * step; t <= hi + step * 1e-6; t += step)
        {
          g_autoptr (GString) text = g_string_new (NULL);
          int tw, th;
          double v = fabs (t) < step * 1e-9 ? 0 : t;

          if (logarithmic)
            m42_number_to_string (text, pow (10, round (v)));
          else
            m42_number_to_string (text, v);
          pango_layout_set_text (label, text->str, -1);
          pango_layout_get_pixel_size (label, &tw, &th);

          cairo_set_source_rgb (cr, 0.90, 0.90, 0.92);
          if (axis == 0)
            {
              double px = L + (t - lo) * sx;
              cairo_move_to (cr, px, T);
              cairo_line_to (cr, px, T + h);
              cairo_stroke (cr);
              cairo_set_source_rgb (cr, 0.35, 0.35, 0.35);
              cairo_move_to (cr, px - tw / 2.0, T + h + 4);
            }
          else
            {
              double py = T + h - (t - lo) * sy;
              cairo_move_to (cr, L, py);
              cairo_line_to (cr, L + w, py);
              cairo_stroke (cr);
              cairo_set_source_rgb (cr, 0.35, 0.35, 0.35);
              cairo_move_to (cr, L - tw - 6, py - th / 2.0);
            }
          pango_cairo_show_layout (cr, label);
        }
    }

  /* The axes themselves, where they fall inside the frame. */
  cairo_set_source_rgb (cr, 0.45, 0.45, 0.45);
  cairo_set_line_width (cr, 1);
  if (p->ymin < 0 && p->ymax > 0)
    {
      double py = T + h + p->ymin * sy;
      cairo_move_to (cr, L, py);
      cairo_line_to (cr, L + w, py);
      cairo_stroke (cr);
    }
  if (p->xmin < 0 && p->xmax > 0)
    {
      double px = L - p->xmin * sx;
      cairo_move_to (cr, px, T);
      cairo_line_to (cr, px, T + h);
      cairo_stroke (cr);
    }

  /* The data, clipped to the frame. */
  cairo_save (cr);
  cairo_rectangle (cr, L, T, w, h);
  cairo_clip (cr);
  for (guint i = 0; i < p->series->len; i++)
    {
      M42Series *ser = g_ptr_array_index (p->series, i);
      gboolean pen_down = FALSE;

      cairo_set_source_rgb (cr, ser->r, ser->g, ser->b);
      cairo_set_line_width (cr, 1.6);
      for (guint k = 0; k + 1 < ser->points->len; k += 2)
        {
          double vx = g_array_index (ser->points, double, k);
          double vy = g_array_index (ser->points, double, k + 1);
          double px, py;

          if (!isfinite (vx) || !isfinite (vy))
            {
              pen_down = FALSE;
              continue;
            }
          px = L + (vx - p->xmin) * sx;
          py = T + h - (vy - p->ymin) * sy;
          switch (ser->kind)
            {
            case M42_SERIES_LINE:
              if (pen_down)
                cairo_line_to (cr, px, py);
              else
                cairo_move_to (cr, px, py);
              pen_down = TRUE;
              break;
            case M42_SERIES_POINTS:
              cairo_arc (cr, px, py, 2.5, 0, 2 * G_PI);
              cairo_fill (cr);
              break;
            case M42_SERIES_STEM:
              {
                /* A stalk up from the axis with a dot on top. */
                double base = T + h + p->ymin * sy;

                base = CLAMP (base, T, T + h);
                cairo_move_to (cr, px, base);
                cairo_line_to (cr, px, py);
                cairo_stroke (cr);
                cairo_arc (cr, px, py, 2.5, 0, 2 * G_PI);
                cairo_fill (cr);
              }
              break;

            case M42_SERIES_STAIRS:
              /* Held flat to the next point, then stepped up to it. */
              if (pen_down)
                {
                  double x_before, y_before;

                  cairo_get_current_point (cr, &x_before, &y_before);
                  cairo_line_to (cr, px, y_before);
                  cairo_line_to (cr, px, py);
                }
              else
                cairo_move_to (cr, px, py);
              pen_down = TRUE;
              break;

            case M42_SERIES_BARS:
              {
                /* A bar as wide as its bin, or a comfortable width when
                 * the series did not say. */
                double bw = ser->width > 0 ? ser->width * sx
                                           : MAX (4.0, w / (ser->points->len / 2.0) * 0.7);
                double base = T + h + p->ymin * sy;

                base = CLAMP (base, T, T + h);
                cairo_rectangle (cr, px - bw * 0.45, MIN (py, base),
                                 bw * 0.9, fabs (base - py));
                cairo_fill_preserve (cr);
                cairo_save (cr);
                cairo_set_source_rgb (cr, ser->r * 0.7, ser->g * 0.7, ser->b * 0.7);
                cairo_set_line_width (cr, 1);
                cairo_stroke (cr);
                cairo_restore (cr);
                cairo_new_path (cr);
              }
              break;
            }
        }
      if (ser->kind == M42_SERIES_LINE || ser->kind == M42_SERIES_STAIRS)
        cairo_stroke (cr);
    }
  cairo_restore (cr);

  /* The contours, each level in its own colour. */
  if (p->contours != NULL)
    {
      cairo_save (cr);
      cairo_rectangle (cr, L, T, w, h);
      cairo_clip (cr);
      cairo_set_line_width (cr, 1.4);
      for (guint i = 0; i < p->contours->len; i++)
        {
          M42Contour *contour = g_ptr_array_index (p->contours, i);

          cairo_set_source_rgb (cr, contour->r, contour->g, contour->b);
          for (guint k = 0; k + 3 < contour->segments->len; k += 4)
            {
              double x1 = g_array_index (contour->segments, double, k);
              double y1 = g_array_index (contour->segments, double, k + 1);
              double x2 = g_array_index (contour->segments, double, k + 2);
              double y2 = g_array_index (contour->segments, double, k + 3);

              cairo_move_to (cr, L + (x1 - p->xmin) * sx, T + h - (y1 - p->ymin) * sy);
              cairo_line_to (cr, L + (x2 - p->xmin) * sx, T + h - (y2 - p->ymin) * sy);
            }
          cairo_stroke (cr);
        }
      cairo_restore (cr);
    }

  /* The border last, over the ends of the curves. */
  cairo_set_source_rgb (cr, 0.35, 0.35, 0.35);
  cairo_set_line_width (cr, 1);
  cairo_rectangle (cr, L + 0.5, T + 0.5, w, h);
  cairo_stroke (cr);

  /* The title above, the axis names beside their axes. */
  cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
  if (p->title != NULL)
    {
      int tw, th;
      pango_font_description_set_absolute_size (desc, 12 * PANGO_SCALE);
      pango_layout_set_font_description (label, desc);
      pango_layout_set_text (label, p->title, -1);
      pango_layout_get_pixel_size (label, &tw, &th);
      cairo_move_to (cr, L + (w - tw) / 2.0, 0);
      pango_cairo_show_layout (cr, label);
      pango_font_description_set_absolute_size (desc, 10 * PANGO_SCALE);
      pango_layout_set_font_description (label, desc);
    }
  if (p->xlabel != NULL)
    {
      int tw, th;
      pango_layout_set_text (label, p->xlabel, -1);
      pango_layout_get_pixel_size (label, &tw, &th);
      cairo_move_to (cr, L + w - tw, T + h + 15);
      pango_cairo_show_layout (cr, label);
    }
  if (p->ylabel != NULL)
    {
      pango_layout_set_text (label, p->ylabel, -1);
      cairo_move_to (cr, 2, T - 2);
      pango_cairo_show_layout (cr, label);
    }

  g_object_unref (label);
  pango_font_description_free (desc);
  cairo_restore (cr);
}

/* --- the drawing itself ------------------------------------------------------- */

static void
draw_box (const M42Box *b, cairo_t *cr, double x, double baseline)
{
  switch (b->kind)
    {
    case BOX_TEXT:
      cairo_move_to (cr, x, baseline - b->ascent);
      pango_cairo_show_layout (cr, b->layout);
      break;

    case BOX_ROW:
      for (guint i = 0; i < b->children->len; i++)
        {
          M42Box *c = g_ptr_array_index (b->children, i);
          draw_box (c, cr, x, baseline);
          x += c->width;
        }
      break;

    case BOX_STACK:
      {
        double y = baseline - b->ascent;
        for (guint i = 0; i < b->children->len; i++)
          {
            M42Box *c = g_ptr_array_index (b->children, i);
            draw_box (c, cr, x, y + c->ascent);
            y += m42_box_height (c);
          }
      }
      break;

    case BOX_FRAC:
      {
        M42Box *num = g_ptr_array_index (b->children, 0);
        M42Box *den = g_ptr_array_index (b->children, 1);
        int gap = GAP (b->size);
        double bar = baseline - AXIS (b->size);

        draw_box (num, cr, x + (b->width - num->width) / 2.0, bar - gap - num->descent);
        draw_box (den, cr, x + (b->width - den->width) / 2.0, bar + gap + den->ascent);
        cairo_set_line_width (cr, MAX (1.0, b->size * 0.055));
        cairo_move_to (cr, x + 1, bar);
        cairo_line_to (cr, x + b->width - 1, bar);
        cairo_stroke (cr);
      }
      break;

    case BOX_POWER:
      {
        M42Box *base = g_ptr_array_index (b->children, 0);
        M42Box *exp = g_ptr_array_index (b->children, 1);
        int rise = (int) (base->ascent * 0.55 + 1);

        draw_box (base, cr, x, baseline);
        draw_box (exp, cr, x + base->width + 1, baseline - rise);
      }
      break;

    case BOX_SQRT:
      {
        M42Box *c = g_ptr_array_index (b->children, 0);
        int hook = (int) (b->size * 0.55) + 4;
        double top = baseline - b->ascent + 1;
        double bottom = baseline + c->descent;

        cairo_set_line_width (cr, MAX (1.0, b->size * 0.06));
        cairo_move_to (cr, x, bottom - (bottom - top) * 0.45);
        cairo_line_to (cr, x + hook * 0.35, bottom - (bottom - top) * 0.30);
        cairo_line_to (cr, x + hook * 0.65, bottom);
        cairo_line_to (cr, x + hook, top);
        cairo_line_to (cr, x + b->width - 1, top);
        cairo_stroke (cr);
        draw_box (c, cr, x + hook + 3, baseline);
      }
      break;

    case BOX_FENCE:
      {
        M42Box *c = g_ptr_array_index (b->children, 0);
        int w = (int) (b->size * (b->tight ? 0.20 : 0.30)) + 2;
        double top = baseline - b->ascent, bottom = baseline + b->descent;

        draw_fence (cr, b->open, x, top, bottom, b->size);
        draw_box (c, cr, x + w + 1, baseline);
        draw_fence (cr, b->close, x + b->width - w, top, bottom, b->size);
      }
      break;

    case BOX_GRID:
      {
        int pad = GAP (b->size) * 2;
        int bracket = (int) (b->size * 0.32) + 3;
        g_autofree int *colw = g_new0 (int, b->cols);
        double top = baseline - b->ascent, bottom = baseline + b->descent;
        double y = top + pad;

        for (guint i = 0; i < b->rows; i++)
          for (guint j = 0; j < b->cols; j++)
            {
              M42Box *cell = g_ptr_array_index (b->children, i * b->cols + j);
              colw[j] = MAX (colw[j], cell->width);
            }

        for (guint i = 0; i < b->rows; i++)
          {
            double cx = x + bracket;
            int row_h = 0;

            for (guint j = 0; j < b->cols; j++)
              row_h = MAX (row_h, m42_box_height (g_ptr_array_index (b->children, i * b->cols + j)));
            for (guint j = 0; j < b->cols; j++)
              {
                M42Box *cell = g_ptr_array_index (b->children, i * b->cols + j);
                /* Numbers line up on the right, as they do in a table. */
                draw_box (cell, cr, cx + colw[j] - cell->width, y + cell->ascent);
                cx += colw[j] + pad * 2;
              }
            y += row_h + pad;
          }

        draw_fence (cr, '[', x, top, bottom, b->size);
        draw_fence (cr, ']', x + b->width - bracket, top, bottom, b->size);
      }
      break;

    case BOX_BIGOP:
      {
        M42Box *sign = g_ptr_array_index (b->children, 0);
        M42Box *under = g_ptr_array_index (b->children, 1);
        M42Box *over = g_ptr_array_index (b->children, 2);

        draw_box (sign, cr, x + (b->width - sign->width) / 2.0, baseline);
        if (over != NULL)
          draw_box (over, cr, x + (b->width - over->width) / 2.0,
                    baseline - sign->ascent - over->descent);
        if (under != NULL)
          draw_box (under, cr, x + (b->width - under->width) / 2.0,
                    baseline + sign->descent + under->ascent);
      }
      break;

    case BOX_PLOT:
      draw_plot (b, cr, x, baseline - b->ascent);
      break;
    }
}

void
m42_box_draw (const M42Box *box, cairo_t *cr, double x, double y)
{
  draw_box (box, cr, x, y + box->ascent);
}
