/* m42-window.h - the notebook window: menu bar, cells, input line
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define M42_TYPE_WINDOW (m42_window_get_type ())
G_DECLARE_FINAL_TYPE (M42Window, m42_window, M42, WINDOW, GtkApplicationWindow)

GtkWidget *m42_window_new (GtkApplication *app);

/* Loads a .m42 file -- one expression per line -- and evaluates each
 * line into the notebook.  Reports failure to the user itself. */
gboolean m42_window_open_file (M42Window *self, GFile *file);

/* Writes what the window holds to a PDF: what --export-pdf does. */
/* The notebook written out in whatever format the name says: .m for a
 * MATLAB script, .wl for a Wolfram one, .nb for a Mathematica
 * notebook, .m42 for its own. */
gboolean m42_window_save_as (M42Window *self, const char *path, GError **error);

gboolean m42_window_export_pdf (M42Window *self, const char *path, GError **error);
gboolean m42_window_print_to_file (M42Window *self, const char *path, GError **error);

G_END_DECLS
