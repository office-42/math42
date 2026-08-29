/* m42-bigint.h - whole numbers with no ceiling
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Exact arithmetic in math42 is done in a gint64 pair for as long as
 * the numbers fit.  When they stop fitting -- 2^100, 100!, the two
 * hundredth Fibonacci number -- the work moves here, to a sign and a
 * row of digits in base a thousand million, which is the largest base
 * whose products still fit in a 64-bit multiply.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define M42_BIG_BASE 1000000000

typedef struct {
  int      sign;    /* -1, 0 or 1 */
  guint    len;     /* how many digits are in use */
  guint32 *digit;   /* least significant first, each below the base */
} M42Big;

M42Big *m42_big_from_int64 (gint64 x);
M42Big *m42_big_from_string (const char *text);   /* NULL if it is not a whole number */
M42Big *m42_big_copy (const M42Big *a);
void    m42_big_free (M42Big *a);

M42Big *m42_big_add (const M42Big *a, const M42Big *b);
M42Big *m42_big_subtract (const M42Big *a, const M42Big *b);
M42Big *m42_big_multiply (const M42Big *a, const M42Big *b);
/* a to the power e; NULL when the answer would be beyond reason. */
M42Big *m42_big_power (const M42Big *a, guint64 e);
/* a divided by a small number, with the remainder if wanted; NULL when
 * the divisor is nothing. */
M42Big *m42_big_divide_small (const M42Big *a, gint64 d, gint64 *remainder);
M42Big *m42_big_factorial (guint n);

int      m42_big_compare (const M42Big *a, const M42Big *b);
gboolean m42_big_is_zero (const M42Big *a);
/* TRUE when the number fits in a gint64, which is where it belongs. */
gboolean m42_big_fits_int64 (const M42Big *a, gint64 *out);
double   m42_big_to_double (const M42Big *a);
char    *m42_big_to_string (const M42Big *a);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (M42Big, m42_big_free)

G_END_DECLS
