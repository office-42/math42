/* m42-notebook.h - the notebook: In[n]/Out[n] cells drawn with Pango
 * and Cairo
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "m42-eval.h"

G_BEGIN_DECLS

#define M42_TYPE_NOTEBOOK (m42_notebook_get_type ())
G_DECLARE_FINAL_TYPE (M42Notebook, m42_notebook, M42, NOTEBOOK, GtkWidget)

GtkWidget *m42_notebook_new (void);

/* Appends an In[n]/Out[n] pair, with whatever Print wrote in between.
 * The notebook draws itself again. */
void m42_notebook_append (M42Notebook *self, int n, const char *input,
                          const M42Value *output, const char *printed);

/* Makes the mathematics bigger or smaller: 1.0 is the usual size.
 * The cells are laid out again at the new size. */
void m42_notebook_set_scale (M42Notebook *self, double scale);
double m42_notebook_get_scale (M42Notebook *self);

/* Removes every cell. */
void m42_notebook_clear (M42Notebook *self);

/* Writes the notebook to a PDF, a page at a time.  Returns FALSE and
 * sets the error if the file could not be written. */
/* The notebook as pages: to a PDF, or to a printer.  The title is
 * written at the top of every page and into the PDF's own properties. */
gboolean m42_notebook_export_pdf (M42Notebook *self, const char *path,
                                  const char *title, GError **error);
gboolean m42_notebook_print (M42Notebook *self, const char *title,
                             GtkWindow *parent, GError **error);
gboolean m42_notebook_print_to_file (M42Notebook *self, const char *title,
                                     const char *path, GError **error);

/* The input of the cell at a point, or NULL: what a click gives back,
 * so that a line can be typed again. */
const char *m42_notebook_input_at (M42Notebook *self, double x, double y);

/* The cell whose result is a drawn figure at a point, or -1.  A right
 * click on one opens a menu that saves it as a PNG; this is the same
 * question asked from outside. */
int m42_notebook_figure_at (M42Notebook *self, double x, double y);

/* Writes the figure of a cell -- its result, drawn at twice the size
 * it has on the page -- to a PNG.  FALSE with the error set when it
 * could not be. */
gboolean m42_notebook_save_figure_png (M42Notebook *self, int cell, const char *path,
                                       GError **error);

/* The inputs so far, one per line, for saving. */
char *m42_notebook_get_inputs (M42Notebook *self);
GStrv m42_notebook_get_outputs (M42Notebook *self);

G_END_DECLS
