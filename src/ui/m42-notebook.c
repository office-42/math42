/* m42-notebook.c - see m42-notebook.h
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The notebook is one custom widget that paints its cells through
 * Cairo: an In[n]:= label in grey, the input in a monospace face, then
 * Out[n]= and the result set as mathematics -- fractions stacked,
 * powers raised, matrices in brackets, graphs drawn -- by m42-typeset.
 * A bracket down the right edge groups each pair, as Mathematica draws
 * them.  The widget reports its natural height so that the scrolled
 * window above it can scroll.
 */

#include "m42-notebook.h"
#include "m42-typeset.h"

#include <cairo-pdf.h>
#include <pango/pangocairo.h>

typedef struct {
  int       n;
  char     *input;
  char     *printed;    /* what Print wrote, or NULL */
  char     *error;      /* the message, when the result was one */
  M42Box   *output;     /* the typeset result, or NULL for an error */
  M42Value *value;      /* kept because the boxes point into it */
} Cell;

struct _M42Notebook {
  GtkWidget  parent_instance;
  GPtrArray *cells;    /* of Cell* */
  double     scale;    /* how big the mathematics is drawn */
};

G_DEFINE_FINAL_TYPE (M42Notebook, m42_notebook, GTK_TYPE_WIDGET)

#define MARGIN_LEFT    76
#define MARGIN_RIGHT   28
#define MARGIN_TOP     12
#define CELL_GAP       16
#define LINE_GAP        5
#define MATH_SIZE      15.0

static void
cell_free (Cell *c)
{
  g_free (c->input);
  g_free (c->printed);
  g_free (c->error);
  g_clear_pointer (&c->output, m42_box_free);
  g_clear_pointer (&c->value, m42_value_unref);
  g_free (c);
}

static PangoLayout *
make_layout (GtkWidget *widget, const char *text, const char *font, int width)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (widget, text);
  PangoFontDescription *desc = pango_font_description_from_string (font);

  pango_layout_set_font_description (layout, desc);
  pango_font_description_free (desc);
  if (width > 0)
    {
      pango_layout_set_width (layout, width * PANGO_SCALE);
      pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
    }
  return layout;
}

/* Lays out one cell and returns its height; draws it if cr is given. */
static int
layout_cell (M42Notebook *self, Cell *c, cairo_t *cr, int y, int width)
{
  GtkWidget *widget = GTK_WIDGET (self);
  int text_width = MAX (width - MARGIN_LEFT - MARGIN_RIGHT, 40);
  int top = y;
  PangoLayout *label, *body;
  /* A cell with no number is a line of text -- a comment carried in
   * from a file, or typed to say what the next cell is for.  It has no
   * In[n] beside it and nothing under it. */
  gboolean is_text = c->n <= 0;
  g_autofree char *in_label = g_strdup_printf ("In[%d]:=", c->n);
  g_autofree char *out_label = g_strdup_printf ("Out[%d]=", c->n);
  int h;

  /* The input. */
  label = make_layout (widget, in_label, "Sans 9", 0);
  body = make_layout (widget, c->input, "Monospace 11", text_width);
  pango_layout_get_pixel_size (body, NULL, &h);
  if (cr != NULL)
    {
      if (!is_text)
        {
          cairo_set_source_rgb (cr, 0.45, 0.45, 0.45);
          cairo_move_to (cr, 10, y + 2);
          pango_cairo_show_layout (cr, label);
        }
      if (is_text)
        cairo_set_source_rgb (cr, 0.35, 0.35, 0.35);
      else
        cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
      cairo_move_to (cr, MARGIN_LEFT, y);
      pango_cairo_show_layout (cr, body);
    }
  g_object_unref (label);
  g_object_unref (body);
  y += h + LINE_GAP;

  /* Whatever Print wrote, before the result, as a notebook shows it. */
  if (c->printed != NULL)
    {
      body = make_layout (widget, c->printed, "Monospace 10", text_width);
      pango_layout_get_pixel_size (body, NULL, &h);
      if (cr != NULL)
        {
          cairo_set_source_rgb (cr, 0.25, 0.25, 0.3);
          cairo_move_to (cr, MARGIN_LEFT, y);
          pango_cairo_show_layout (cr, body);
        }
      g_object_unref (body);
      y += h + LINE_GAP;
    }

  /* The result: an error in words, anything else set as mathematics. */
  if (c->error != NULL)
    {
      label = make_layout (widget, out_label, "Sans 9", 0);
      body = make_layout (widget, c->error, "Sans 11", text_width);
      pango_layout_get_pixel_size (body, NULL, &h);
      if (cr != NULL)
        {
          cairo_set_source_rgb (cr, 0.45, 0.45, 0.45);
          cairo_move_to (cr, 10, y + 3);
          pango_cairo_show_layout (cr, label);
          cairo_set_source_rgb (cr, 0.75, 0.1, 0.1);
          cairo_move_to (cr, MARGIN_LEFT, y);
          pango_cairo_show_layout (cr, body);
        }
      g_object_unref (label);
      g_object_unref (body);
      y += h;
    }
  else if (c->output != NULL)
    {
      h = m42_box_height (c->output);
      if (cr != NULL)
        {
          label = make_layout (widget, out_label, "Sans 9", 0);
          cairo_set_source_rgb (cr, 0.45, 0.45, 0.45);
          cairo_move_to (cr, 10, y + 3);
          pango_cairo_show_layout (cr, label);
          g_object_unref (label);
          cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
          m42_box_draw (c->output, cr, MARGIN_LEFT, y);
        }
      y += h;
    }

  /* The cell bracket down the right edge. */
  if (cr != NULL)
    {
      double x = width - MARGIN_RIGHT + 12;
      cairo_set_source_rgb (cr, 0.55, 0.55, 0.65);
      cairo_set_line_width (cr, 1.0);
      cairo_move_to (cr, x - 4, top + 0.5);
      cairo_line_to (cr, x, top + 0.5);
      cairo_line_to (cr, x, y + 0.5);
      cairo_line_to (cr, x - 4, y + 0.5);
      cairo_stroke (cr);
    }

  return y - top;
}

static void
m42_notebook_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  M42Notebook *self = M42_NOTEBOOK (widget);
  int width = gtk_widget_get_width (widget);
  int height = gtk_widget_get_height (widget);
  cairo_t *cr = gtk_snapshot_append_cairo (snapshot,
                                           &GRAPHENE_RECT_INIT (0, 0, width, height));
  int y = MARGIN_TOP;

  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_paint (cr);

  if (self->cells->len == 0)
    {
      PangoLayout *hint = make_layout (widget,
        "Type an expression below and press Enter.\n\n"
        "    2 + 2            D[x^3 + Sin[x], x]        Integrate[x^2, x]\n"
        "    Sqrt[2]          Solve[x^2 - 4 == 0, x]    A = [1 2; 3 4]\n"
        "    Sin[Pi/2]        Plot[Sin[x]/x, {x, -20, 20}]\n"
        "    Eigenvalues[[2 1; 1 2]]                    Table[i^2, {i, 1, 5}]\n\n"
        "Mathematica's Sin[x] and MATLAB's sin(x) both work.",
        "Monospace 10", width - 40);
      cairo_set_source_rgb (cr, 0.45, 0.45, 0.45);
      cairo_move_to (cr, 20, y);
      pango_cairo_show_layout (cr, hint);
      g_object_unref (hint);
    }

  for (guint i = 0; i < self->cells->len; i++)
    y += layout_cell (self, g_ptr_array_index (self->cells, i), cr, y, width) + CELL_GAP;

  cairo_destroy (cr);
}

static void
m42_notebook_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                      int *minimum, int *natural,
                      int *minimum_baseline, int *natural_baseline)
{
  M42Notebook *self = M42_NOTEBOOK (widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      int wide = 640;

      /* A graph must not be squeezed, so the notebook asks for the room
       * its widest cell needs. */
      for (guint i = 0; i < self->cells->len; i++)
        {
          Cell *c = g_ptr_array_index (self->cells, i);
          if (c->output != NULL)
            wide = MAX (wide, MARGIN_LEFT + m42_box_width (c->output) + MARGIN_RIGHT);
        }
      *minimum = 200;
      *natural = wide;
    }
  else
    {
      int width = for_size > 0 ? for_size : 640;
      int y = MARGIN_TOP;

      for (guint i = 0; i < self->cells->len; i++)
        y += layout_cell (self, g_ptr_array_index (self->cells, i), NULL, y, width) + CELL_GAP;
      *minimum = *natural = MAX (y, 60);
    }
}

static void
m42_notebook_dispose (GObject *object)
{
  M42Notebook *self = M42_NOTEBOOK (object);

  g_clear_pointer (&self->cells, g_ptr_array_unref);
  G_OBJECT_CLASS (m42_notebook_parent_class)->dispose (object);
}

static void
m42_notebook_class_init (M42NotebookClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = m42_notebook_dispose;
  widget_class->snapshot = m42_notebook_snapshot;
  widget_class->measure = m42_notebook_measure;
  gtk_widget_class_set_css_name (widget_class, "notebook-canvas");
}

static void
m42_notebook_init (M42Notebook *self)
{
  self->cells = g_ptr_array_new_with_free_func ((GDestroyNotify) cell_free);
  self->scale = 1.0;
  gtk_widget_add_css_class (GTK_WIDGET (self), "m42-notebook");
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
}

GtkWidget *
m42_notebook_new (void)
{
  return g_object_new (M42_TYPE_NOTEBOOK, NULL);
}

void
m42_notebook_append (M42Notebook *self, int n, const char *input,
                     const M42Value *output, const char *printed)
{
  Cell *c = g_new0 (Cell, 1);

  c->n = n;
  c->input = g_strdup (input);
  c->printed = printed != NULL && *printed != '\0' ? g_strchomp (g_strdup (printed)) : NULL;
  if (output == NULL)
    ;                    /* a line of text, with nothing worked out */
  else if (output->kind == M42_VALUE_ERROR)
    c->error = m42_value_to_string (output);
  else if (output->kind != M42_VALUE_NULL)
    {
      /* The boxes hold on to the value: a graph is drawn from it. */
      c->value = m42_value_ref ((M42Value *) output);
      c->output = m42_box_from_value (GTK_WIDGET (self), c->value, MATH_SIZE * self->scale);
    }
  g_ptr_array_add (self->cells, c);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
m42_notebook_set_scale (M42Notebook *self, double scale)
{
  self->scale = CLAMP (scale, 0.5, 3.0);
  for (guint i = 0; i < self->cells->len; i++)
    {
      Cell *c = g_ptr_array_index (self->cells, i);

      if (c->value == NULL)
        continue;
      g_clear_pointer (&c->output, m42_box_free);
      c->output = m42_box_from_value (GTK_WIDGET (self), c->value, MATH_SIZE * self->scale);
    }
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

double
m42_notebook_get_scale (M42Notebook *self)
{
  return self->scale;
}

void
m42_notebook_clear (M42Notebook *self)
{
  g_ptr_array_set_size (self->cells, 0);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

/* --- out of the window ------------------------------------------------- */

gboolean
m42_notebook_export_pdf (M42Notebook *self, const char *path, GError **error)
{
  /* A4, in points, with a margin an inch wide at the sides. */
  const double page_w = 595, page_h = 842, margin = 48;
  cairo_surface_t *surface = cairo_pdf_surface_create (path, page_w, page_h);
  cairo_t *cr = cairo_create (surface);
  double y = margin;
  int width = (int) (page_w - 2 * margin);

  for (guint i = 0; i < self->cells->len; i++)
    {
      Cell *c = g_ptr_array_index (self->cells, i);
      int h = layout_cell (self, c, NULL, 0, width);

      if (y > margin && y + h > page_h - margin)
        {
          cairo_show_page (cr);
          y = margin;
        }
      cairo_save (cr);
      cairo_translate (cr, margin, y);
      layout_cell (self, c, cr, 0, width);
      cairo_restore (cr);
      y += h + CELL_GAP;
    }

  cairo_destroy (cr);
  cairo_surface_finish (surface);
  {
    cairo_status_t status = cairo_surface_status (surface);
    cairo_surface_destroy (surface);
    if (status != CAIRO_STATUS_SUCCESS)
      {
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s",
                     cairo_status_to_string (status));
        return FALSE;
      }
  }
  return TRUE;
}

const char *
m42_notebook_input_at (M42Notebook *self, double x, double y)
{
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  int top = MARGIN_TOP;

  for (guint i = 0; i < self->cells->len; i++)
    {
      Cell *c = g_ptr_array_index (self->cells, i);
      int h = layout_cell (self, c, NULL, top, width);

      if (y >= top && y <= top + h)
        return c->input;
      top += h + CELL_GAP;
    }
  return NULL;
}

char *
m42_notebook_get_inputs (M42Notebook *self)
{
  GString *out = g_string_new (NULL);

  for (guint i = 0; i < self->cells->len; i++)
    {
      Cell *c = g_ptr_array_index (self->cells, i);
      g_string_append (out, c->input);
      g_string_append_c (out, '\n');
    }
  return g_string_free (out, FALSE);
}
