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
      /* A result wider than the room it has -- a graph, or a matrix of
       * long numbers -- is drawn smaller rather than over the edge.  On
       * paper the room is the page, and a graph used to run past it and
       * through the cell bracket. */
      int box_width = m42_box_width (c->output);
      double fit = box_width > text_width && box_width > 0
                     ? text_width / (double) box_width : 1.0;

      h = (int) (m42_box_height (c->output) * fit);
      if (cr != NULL)
        {
          label = make_layout (widget, out_label, "Sans 9", 0);
          cairo_set_source_rgb (cr, 0.45, 0.45, 0.45);
          cairo_move_to (cr, 10, y + 3);
          pango_cairo_show_layout (cr, label);
          g_object_unref (label);
          cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
          cairo_save (cr);
          cairo_translate (cr, MARGIN_LEFT, y);
          cairo_scale (cr, fit, fit);
          m42_box_draw (c->output, cr, 0, 0);
          cairo_restore (cr);
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

/* --- on paper -----------------------------------------------------------
 *
 * A notebook on paper is the same notebook, laid down the page a cell
 * at a time: a cell that will not fit on what is left of a page goes
 * to the next one, and a cell taller than a whole page is drawn
 * smaller so that it fits, since half a graph is worth nothing.  Each
 * page carries the notebook's name at the top and its number at the
 * foot, because a printed page that says nothing about where it came
 * from is a page nobody can file.
 *
 * The PDF and the printer share all of it, so what comes out of one is
 * what comes out of the other.
 */

#define PAPER_MARGIN 48.0   /* points: two thirds of an inch */
#define PAPER_HEAD   26.0   /* room above the first cell for the name */
#define PAPER_FOOT   22.0   /* room below the last for the number */

/* Where each page starts, and how much each cell had to be shrunk.
 * starts holds the first cell of every page; scales holds one number
 * per cell. */
typedef struct {
  GArray *starts;   /* of guint */
  GArray *scales;   /* of double */
  guint   pages;
} Paging;

static void
paging_clear (Paging *p)
{
  g_clear_pointer (&p->starts, g_array_unref);
  g_clear_pointer (&p->scales, g_array_unref);
}

static void
paginate (M42Notebook *self, Paging *out, double page_w, double page_h)
{
  double width = page_w - 2 * PAPER_MARGIN;
  double top = PAPER_MARGIN + PAPER_HEAD;
  double bottom = page_h - PAPER_MARGIN - PAPER_FOOT;
  double room = bottom - top;
  double y = top;
  guint first = 0;

  out->starts = g_array_new (FALSE, FALSE, sizeof (guint));
  out->scales = g_array_new (FALSE, FALSE, sizeof (double));
  g_array_append_val (out->starts, first);

  for (guint i = 0; i < self->cells->len; i++)
    {
      Cell *c = g_ptr_array_index (self->cells, i);
      double h = layout_cell (self, c, NULL, 0, (int) width);
      double scale = 1;

      /* Taller than any page: drawn smaller, and given a page of its
       * own so that nothing else is squeezed around it. */
      if (h > room)
        {
          scale = room / h;
          h = room;
        }
      if (y > top && y + h > bottom)
        {
          g_array_append_val (out->starts, i);
          y = top;
        }
      g_array_append_val (out->scales, scale);
      y += h + CELL_GAP;
    }
  out->pages = MAX (out->starts->len, 1);
}

/* One page of it, with its name above and its number below. */
static void
draw_page (M42Notebook *self, cairo_t *cr, const Paging *paging, guint page,
           const char *title, double page_w, double page_h)
{
  double width = page_w - 2 * PAPER_MARGIN;
  double top = PAPER_MARGIN + PAPER_HEAD;
  double bottom = page_h - PAPER_MARGIN - PAPER_FOOT;
  double y = top;
  guint from = g_array_index (paging->starts, guint, page);
  guint upto = page + 1 < paging->starts->len
                 ? g_array_index (paging->starts, guint, page + 1)
                 : self->cells->len;

  /* The name, and a hairline under it. */
  if (title != NULL && *title != '\0')
    {
      PangoLayout *label = make_layout (GTK_WIDGET (self), title, "Sans 9", 0);

      cairo_set_source_rgb (cr, 0.42, 0.42, 0.46);
      cairo_move_to (cr, PAPER_MARGIN, PAPER_MARGIN);
      pango_cairo_show_layout (cr, label);
      g_object_unref (label);
    }
  cairo_set_source_rgb (cr, 0.80, 0.80, 0.84);
  cairo_set_line_width (cr, 0.5);
  cairo_move_to (cr, PAPER_MARGIN, PAPER_MARGIN + PAPER_HEAD - 10);
  cairo_line_to (cr, page_w - PAPER_MARGIN, PAPER_MARGIN + PAPER_HEAD - 10);
  cairo_stroke (cr);

  for (guint i = from; i < upto && i < self->cells->len; i++)
    {
      Cell *c = g_ptr_array_index (self->cells, i);
      double scale = g_array_index (paging->scales, double, i);
      double h = layout_cell (self, c, NULL, 0, (int) width) * scale;

      cairo_save (cr);
      cairo_translate (cr, PAPER_MARGIN, y);
      if (scale != 1)
        cairo_scale (cr, scale, scale);
      cairo_set_source_rgb (cr, 0, 0, 0);
      layout_cell (self, c, cr, 0, (int) (width / scale));
      cairo_restore (cr);
      y += h + CELL_GAP;
    }

  {
    g_autofree char *number = g_strdup_printf ("%u of %u", page + 1, paging->pages);
    PangoLayout *foot = make_layout (GTK_WIDGET (self), number, "Sans 9", 0);
    int w, h;

    pango_layout_get_pixel_size (foot, &w, &h);
    cairo_set_source_rgb (cr, 0.42, 0.42, 0.46);
    cairo_move_to (cr, (page_w - w) / 2, bottom + 6);
    pango_cairo_show_layout (cr, foot);
    g_object_unref (foot);
  }
}

gboolean
m42_notebook_export_pdf (M42Notebook *self, const char *path, const char *title,
                         GError **error)
{
  /* A4, in points. */
  const double page_w = 595, page_h = 842;
  cairo_surface_t *surface = cairo_pdf_surface_create (path, page_w, page_h);
  cairo_t *cr = cairo_create (surface);
  Paging paging = { NULL, NULL, 0 };

  cairo_pdf_surface_set_metadata (surface, CAIRO_PDF_METADATA_TITLE,
                                  title != NULL ? title : "Notebook");
  cairo_pdf_surface_set_metadata (surface, CAIRO_PDF_METADATA_CREATOR, "Math42");

  paginate (self, &paging, page_w, page_h);
  for (guint page = 0; page < paging.pages; page++)
    {
      draw_page (self, cr, &paging, page, title, page_w, page_h);
      cairo_show_page (cr);
    }
  paging_clear (&paging);

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

/* --- the printer -------------------------------------------------------- */

typedef struct {
  M42Notebook *self;
  char        *title;
  Paging       paging;
} PrintJob;

static void
print_begin (GtkPrintOperation *op, GtkPrintContext *context, gpointer data)
{
  PrintJob *job = data;
  double page_w = gtk_print_context_get_width (context);
  double page_h = gtk_print_context_get_height (context);

  /* The page the printer offers is already inside its own margins, so
   * the width here is the paper's less what it cannot reach. */
  paginate (job->self, &job->paging, page_w + 2 * PAPER_MARGIN,
            page_h + 2 * PAPER_MARGIN);
  gtk_print_operation_set_n_pages (op, (int) job->paging.pages);
}

static void
print_draw (GtkPrintOperation *op, GtkPrintContext *context, int page, gpointer data)
{
  PrintJob *job = data;
  cairo_t *cr = gtk_print_context_get_cairo_context (context);
  double page_w = gtk_print_context_get_width (context) + 2 * PAPER_MARGIN;
  double page_h = gtk_print_context_get_height (context) + 2 * PAPER_MARGIN;

  cairo_save (cr);
  cairo_translate (cr, -PAPER_MARGIN, -PAPER_MARGIN);
  draw_page (job->self, cr, &job->paging, (guint) page, job->title, page_w, page_h);
  cairo_restore (cr);
}

static void
print_done (GtkPrintOperation *op, GtkPrintOperationResult result, gpointer data)
{
  PrintJob *job = data;

  paging_clear (&job->paging);
  g_free (job->title);
  g_free (job);
}

/* Both ways of printing: to a printer through its dialog, or straight
 * to a file, which is the same machinery with nothing at the end of it
 * but a name.  The second is what --print-to uses, and what makes the
 * printed pages something that can be looked at in a terminal. */
static gboolean
print_it (M42Notebook *self, const char *title, GtkWindow *parent,
          const char *to_file, GError **error)
{
  GtkPrintOperation *op = gtk_print_operation_new ();
  PrintJob *job = g_new0 (PrintJob, 1);
  GtkPrintOperationResult result;
  g_autoptr (GtkPageSetup) setup = gtk_page_setup_new ();

  job->self = self;
  job->title = g_strdup (title != NULL ? title : "Notebook");
  if (to_file != NULL)
    gtk_print_operation_set_export_filename (op, to_file);

  /* math42 draws its own margins, so the printer is asked for as much
   * of the sheet as it will give. */
  gtk_page_setup_set_paper_size_and_default_margins (setup, gtk_paper_size_new (NULL));
  gtk_print_operation_set_default_page_setup (op, setup);
  gtk_print_operation_set_use_full_page (op, FALSE);
  gtk_print_operation_set_unit (op, GTK_UNIT_POINTS);
  gtk_print_operation_set_embed_page_setup (op, TRUE);
  gtk_print_operation_set_job_name (op, job->title);
  g_signal_connect (op, "begin-print", G_CALLBACK (print_begin), job);
  g_signal_connect (op, "draw-page", G_CALLBACK (print_draw), job);
  g_signal_connect (op, "done", G_CALLBACK (print_done), job);

  result = gtk_print_operation_run (op,
                                    to_file != NULL
                                      ? GTK_PRINT_OPERATION_ACTION_EXPORT
                                      : GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                                    parent, error);
  g_object_unref (op);
  return result != GTK_PRINT_OPERATION_RESULT_ERROR;
}

gboolean
m42_notebook_print (M42Notebook *self, const char *title, GtkWindow *parent,
                    GError **error)
{
  return print_it (self, title, parent, NULL, error);
}

gboolean
m42_notebook_print_to_file (M42Notebook *self, const char *title, const char *path,
                            GError **error)
{
  return print_it (self, title, NULL, path, error);
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

/* What each cell answered, one string for each input and in the same
 * order, so that a format with a place for results can write them
 * beside the lines that made them.  A cell that answered nothing at
 * all -- a comment, or an assignment ending in a semicolon -- gets an
 * empty string rather than a hole. */
GStrv
m42_notebook_get_outputs (M42Notebook *self)
{
  GPtrArray *out = g_ptr_array_new ();

  for (guint i = 0; i < self->cells->len; i++)
    {
      Cell *c = g_ptr_array_index (self->cells, i);
      char *text = NULL;

      if (c->error != NULL)
        text = g_strdup (c->error);
      else if (c->value != NULL)
        text = m42_value_to_string (c->value);
      g_ptr_array_add (out, text != NULL ? text : g_strdup (""));
    }
  g_ptr_array_add (out, NULL);
  return (GStrv) g_ptr_array_free (out, FALSE);
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
