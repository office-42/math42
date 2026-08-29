/* m42-symbolic.h - expressions as trees: printing, simplifying,
 * differentiating, substituting
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

#include "m42-parser.h"

G_BEGIN_DECLS

/* Appends the expression as the user would type it: 2 x + Sin[y]. */
void m42_node_to_string (GString *out, const M42Node *n);
void m42_number_to_string (GString *out, double x);

/* Folds constants and drops the identities: x + 0, 1 x, x^1.
 * Returns a new tree. */
M42Node *m42_node_simplify (const M42Node *n);

/* d/dvar.  Returns a new tree, not simplified; NULL if some part of
 * the expression cannot be differentiated. */
M42Node *m42_node_differentiate (const M42Node *n, const char *var);

/* Multiplies out the brackets and gathers like terms, so that
 * (x + 1)^3 becomes x^3 + 3 x^2 + 3 x + 1.  Returns a new tree. */
M42Node *m42_node_expand (const M42Node *n);

/* The antiderivative with respect to var, without the constant.
 * Returns a new tree, or NULL when this integral is beyond the rules
 * math42 knows: powers, sums, constant multiples, 1/(a x + b), the
 * trigonometric, hyperbolic, exponential and logarithmic functions of
 * a linear argument, and polynomials times those, by parts. */
M42Node *m42_node_integrate (const M42Node *n, const char *var);

/* The Laplace transform of a function of t, as a function of s, by the
 * table a course hands out; NULL when the function is not in it. */
M42Node *m42_node_laplace (const M42Node *n, const char *t, const char *s);
/* And back again, splitting a quotient of polynomials into partial
 * fractions the way it is done by hand. */
M42Node *m42_node_inverse_laplace (const M42Node *n, const char *s, const char *t);

/* The one sided Z transform of a sequence in var, as a function of z.
 * NULL when it is not in the table a course hands out. */
M42Node *m42_node_ztransform (const M42Node *n, const char *var, const char *zname);

/* The sequence a Z transform came from, by splitting X(z)/z into
 * partial fractions.  NULL when that does not work out. */
M42Node *m42_node_inverse_ztransform (const M42Node *n, const char *zname,
                                      const char *var);

/* --- polynomials, as lists of coefficients, lowest power first ------- */

/* TRUE when the tree is a polynomial in var with numbers for
 * coefficients; the coefficients are put in out. */
gboolean m42_node_polynomial (const M42Node *n, const char *var, GArray *out);
/* And back: the coefficients as a tree. */
M42Node *m42_node_from_polynomial (const GArray *coefficients, const char *var);
/* p = q d + r, with the remainder lower in degree than d. */
void     m42_polynomial_divide (const GArray *p, const GArray *d, GArray *q, GArray *r);

/* A rational function split into partial fractions, or NULL when it is
 * not one that math42 can split. */
M42Node *m42_node_apart (const M42Node *n, const char *var);
/* A sum of fractions written over one denominator. */
M42Node *m42_node_together (const M42Node *n);

/* Every occurrence of the name replaced by a copy of the tree. */
M42Node *m42_node_substitute (const M42Node *n, const char *name, const M42Node *with);

/* TRUE if the name appears anywhere in the tree. */
gboolean m42_node_depends_on (const M42Node *n, const char *name);

/* The coefficients of a polynomial in var, lowest power first, as
 * expressions -- so that a x^2 + b x + c gives back a, b and c with
 * the letters still in them.  NULL when it is not a polynomial in var.
 * The caller owns the array and everything in it. */
GPtrArray *m42_node_poly_terms (const M42Node *n, const char *var);

G_END_DECLS
