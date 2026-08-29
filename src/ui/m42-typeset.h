/* m42-typeset.h - laying out a value as mathematics
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A value becomes a tree of boxes -- text, rows, fractions, powers,
 * radicals, bracketed grids, graphs -- which measures itself when it
 * is built and draws itself through Cairo.  It is what turns
 * D[x^3, x] into 3 x^2 on the page rather than "3 x^2" in a terminal.
 */

#pragma once

#include <gtk/gtk.h>

#include "m42-value.h"

G_BEGIN_DECLS

typedef struct _M42Box M42Box;

/* Builds the boxes for a value.  widget supplies the Pango context, so
 * the text is measured in the font the window is using. */
M42Box *m42_box_from_value (GtkWidget *widget, const M42Value *v, double size);
void    m42_box_free (M42Box *box);

int m42_box_width (const M42Box *box);
int m42_box_height (const M42Box *box);

/* Draws with the box's top-left corner at (x, y). */
void m42_box_draw (const M42Box *box, cairo_t *cr, double x, double y);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (M42Box, m42_box_free)

G_END_DECLS
