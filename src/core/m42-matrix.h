/* m42-matrix.h - linear algebra on lists of lists of numbers
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

#include "m42-value.h"

G_BEGIN_DECLS

/* A dense row-major matrix of doubles. */
typedef struct {
  guint   rows, cols;
  double *a;
} M42Matrix;

/* From a value: a matrix (list of rows) or a vector (one column when
 * as_column, else one row).  NULL if it is neither. */
M42Matrix *m42_matrix_from_value (const M42Value *v, gboolean as_column);
/* Back to a value: rows of lists; a single row or column becomes a
 * plain vector when flatten is set. */
M42Value  *m42_matrix_to_value (const M42Matrix *m, gboolean flatten);
M42Matrix *m42_matrix_new (guint rows, guint cols);
void       m42_matrix_free (M42Matrix *m);

static inline double *
m42_matrix_at (M42Matrix *m, guint i, guint j)
{
  return &m->a[i * m->cols + j];
}

M42Matrix *m42_matrix_transpose (const M42Matrix *m);
M42Matrix *m42_matrix_multiply (const M42Matrix *a, const M42Matrix *b);   /* NULL on shape mismatch */
double     m42_matrix_det (const M42Matrix *m);                            /* square only */
M42Matrix *m42_matrix_inverse (const M42Matrix *m);                        /* NULL if singular */
M42Matrix *m42_matrix_solve (const M42Matrix *a, const M42Matrix *b);      /* a x = b; NULL if singular */

/* The value-level operations the evaluator exposes; each returns a new
 * value, an M42_VALUE_ERROR when the shapes do not fit. */
/* Eigenvalues, largest first: Jacobi rotations for a symmetric matrix
 * and the QR iteration otherwise.  Complex eigenvalues are reported as
 * an error rather than silently dropped. */
M42Value *m42_value_eigenvalues (const M42Value *v);
/* Eigenvectors, one row each; symmetric matrices only. */
M42Value *m42_value_eigenvectors (const M42Value *v);
M42Value *m42_value_matrix_power (const M42Value *v, int k);

/* Row reduction, and what a linear algebra course builds on it. */
M42Value *m42_value_row_reduce (const M42Value *v);
M42Value *m42_value_null_space (const M42Value *v);
M42Value *m42_value_orthogonalize (const M42Value *v);
M42Value *m42_value_least_squares (const M42Value *a, const M42Value *b);
/* The coefficients of det(A - x I), lowest power first. */
M42Value *m42_value_characteristic (const M42Value *v);
M42Value *m42_value_matrix_exp (const M42Value *v);

/* The decompositions: {L, U, order}, {Q, R}, {U, S, V}, and what falls
 * out of the singular values. */
M42Value *m42_value_lu (const M42Value *v);
M42Value *m42_value_qr (const M42Value *v);
M42Value *m42_value_svd (const M42Value *v);
M42Value *m42_value_singular_values (const M42Value *v);
M42Value *m42_value_pseudo_inverse (const M42Value *v);
M42Value *m42_value_condition (const M42Value *v);
M42Value *m42_value_eigensystem (const M42Value *v);

M42Value *m42_value_dot (const M42Value *a, const M42Value *b);
M42Value *m42_value_transpose (const M42Value *v);
M42Value *m42_value_det (const M42Value *v);
M42Value *m42_value_inverse (const M42Value *v);
M42Value *m42_value_linear_solve (const M42Value *a, const M42Value *b);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (M42Matrix, m42_matrix_free)

G_END_DECLS
