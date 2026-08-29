/* m42-help.h - what math42 can do, in a table
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every function the evaluator knows, with the shape of its call and a
 * line about it.  The evaluator answers ?Sin from here, and the window
 * builds its reference from the same table, so the two cannot drift
 * apart.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  const char *name;      /* Mathematica's spelling */
  const char *matlab;    /* MATLAB's, or NULL when there is none */
  const char *usage;     /* Sin[x] */
  const char *summary;   /* one line, no full stop */
  const char *section;   /* which part of the reference it belongs to */
} M42Function;

/* The table, ended by a row whose name is NULL. */
const M42Function *m42_functions (void);

/* The entry for a name in either spelling, or NULL. */
const M42Function *m42_function_find (const char *name);

G_END_DECLS
