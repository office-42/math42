/* m42-mat.h - MATLAB's own file of matrices
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A .mat file holds the variables of a MATLAB session: their names,
 * their shapes and their numbers.  math42 reads the level 5 files
 * every MATLAB since 1996 writes, compressed (version 7) or not, and
 * writes uncompressed ones of the same kind, which MATLAB and Octave
 * both load without a word.
 */

#pragma once

#include "m42-value.h"

G_BEGIN_DECLS

/* Everything in the file, as a list of values, with the names they
 * went in under handed back through names -- one for each, ending in
 * NULL, to be freed with g_strfreev.  NULL on failure, with the reason
 * in error. */
M42Value *m42_mat_read (const char *path, GStrv *names, GError **error);

/* The values under those names, one for each.  Numbers go out as a
 * matrix of doubles and a string as characters, which is what MATLAB
 * expects to find. */
gboolean m42_mat_write (const char *path, const char *const *names,
                        const M42Value *values, GError **error);

G_END_DECLS
