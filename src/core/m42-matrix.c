/* m42-matrix.c - see m42-matrix.h
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m42-matrix.h"

#include <math.h>
#include <string.h>

M42Matrix *
m42_matrix_new (guint rows, guint cols)
{
  M42Matrix *m = g_new0 (M42Matrix, 1);
  m->rows = rows;
  m->cols = cols;
  m->a = g_new0 (double, (gsize) rows * cols);
  return m;
}

void
m42_matrix_free (M42Matrix *m)
{
  if (m == NULL)
    return;
  g_free (m->a);
  g_free (m);
}

M42Matrix *
m42_matrix_from_value (const M42Value *v, gboolean as_column)
{
  guint rows, cols;
  M42Matrix *m;

  if (m42_value_is_matrix (v, &rows, &cols))
    {
      m = m42_matrix_new (rows, cols);
      for (guint i = 0; i < rows; i++)
        {
          M42Value *row = m42_value_list_nth (v, i);
          for (guint j = 0; j < cols; j++)
            *m42_matrix_at (m, i, j) = m42_value_list_nth (row, j)->u.number;
        }
      return m;
    }
  if (m42_value_is_vector (v))
    {
      guint n = m42_value_list_length (v);
      m = as_column ? m42_matrix_new (n, 1) : m42_matrix_new (1, n);
      for (guint i = 0; i < n; i++)
        m->a[i] = m42_value_list_nth (v, i)->u.number;
      return m;
    }
  return NULL;
}

M42Value *
m42_matrix_to_value (const M42Matrix *m, gboolean flatten)
{
  M42Value *out = m42_value_list_new ();

  if (flatten && (m->rows == 1 || m->cols == 1))
    {
      for (guint i = 0; i < m->rows * m->cols; i++)
        m42_value_list_append (out, m42_value_number (m->a[i]));
      return out;
    }
  for (guint i = 0; i < m->rows; i++)
    {
      M42Value *row = m42_value_list_new ();
      for (guint j = 0; j < m->cols; j++)
        m42_value_list_append (row, m42_value_number (m->a[i * m->cols + j]));
      m42_value_list_append (out, row);
    }
  return out;
}

M42Matrix *
m42_matrix_transpose (const M42Matrix *m)
{
  M42Matrix *t = m42_matrix_new (m->cols, m->rows);

  for (guint i = 0; i < m->rows; i++)
    for (guint j = 0; j < m->cols; j++)
      *m42_matrix_at (t, j, i) = m->a[i * m->cols + j];
  return t;
}

M42Matrix *
m42_matrix_multiply (const M42Matrix *a, const M42Matrix *b)
{
  M42Matrix *c;

  if (a->cols != b->rows)
    return NULL;
  c = m42_matrix_new (a->rows, b->cols);
  for (guint i = 0; i < a->rows; i++)
    for (guint j = 0; j < b->cols; j++)
      {
        double s = 0;
        for (guint k = 0; k < a->cols; k++)
          s += a->a[i * a->cols + k] * b->a[k * b->cols + j];
        *m42_matrix_at (c, i, j) = s;
      }
  return c;
}

/* Gauss-Jordan with partial pivoting on [a | b], leaving b as the
 * solution; returns FALSE if a is singular.  With b the identity this
 * inverts; with b a column it solves. */
static gboolean
gauss_jordan (M42Matrix *a, M42Matrix *b, double *det)
{
  guint n = a->rows;
  double d = 1.0;

  for (guint col = 0; col < n; col++)
    {
      guint pivot = col;
      double best = fabs (*m42_matrix_at (a, col, col));

      for (guint r = col + 1; r < n; r++)
        if (fabs (*m42_matrix_at (a, r, col)) > best)
          {
            best = fabs (*m42_matrix_at (a, r, col));
            pivot = r;
          }
      if (best < 1e-300)
        {
          if (det != NULL)
            *det = 0;
          return FALSE;
        }
      if (pivot != col)
        {
          for (guint j = 0; j < a->cols; j++)
            {
              double t = *m42_matrix_at (a, col, j);
              *m42_matrix_at (a, col, j) = *m42_matrix_at (a, pivot, j);
              *m42_matrix_at (a, pivot, j) = t;
            }
          if (b != NULL)
            for (guint j = 0; j < b->cols; j++)
              {
                double t = *m42_matrix_at (b, col, j);
                *m42_matrix_at (b, col, j) = *m42_matrix_at (b, pivot, j);
                *m42_matrix_at (b, pivot, j) = t;
              }
          d = -d;
        }

      {
        double p = *m42_matrix_at (a, col, col);
        d *= p;
        for (guint j = 0; j < a->cols; j++)
          *m42_matrix_at (a, col, j) /= p;
        if (b != NULL)
          for (guint j = 0; j < b->cols; j++)
            *m42_matrix_at (b, col, j) /= p;
      }

      for (guint r = 0; r < n; r++)
        {
          double f;
          if (r == col)
            continue;
          f = *m42_matrix_at (a, r, col);
          if (f == 0)
            continue;
          for (guint j = 0; j < a->cols; j++)
            *m42_matrix_at (a, r, j) -= f * *m42_matrix_at (a, col, j);
          if (b != NULL)
            for (guint j = 0; j < b->cols; j++)
              *m42_matrix_at (b, r, j) -= f * *m42_matrix_at (b, col, j);
        }
    }
  if (det != NULL)
    *det = d;
  return TRUE;
}

static M42Matrix *
copy (const M42Matrix *m)
{
  M42Matrix *c = m42_matrix_new (m->rows, m->cols);
  memcpy (c->a, m->a, sizeof (double) * m->rows * m->cols);
  return c;
}

double
m42_matrix_det (const M42Matrix *m)
{
  g_autoptr (M42Matrix) work = copy (m);
  double d;

  gauss_jordan (work, NULL, &d);
  return d;
}

M42Matrix *
m42_matrix_inverse (const M42Matrix *m)
{
  g_autoptr (M42Matrix) work = copy (m);
  M42Matrix *inv = m42_matrix_new (m->rows, m->rows);

  for (guint i = 0; i < m->rows; i++)
    *m42_matrix_at (inv, i, i) = 1;
  if (!gauss_jordan (work, inv, NULL))
    {
      m42_matrix_free (inv);
      return NULL;
    }
  return inv;
}

M42Matrix *
m42_matrix_solve (const M42Matrix *a, const M42Matrix *b)
{
  g_autoptr (M42Matrix) work = copy (a);
  M42Matrix *x = copy (b);

  if (!gauss_jordan (work, x, NULL))
    {
      m42_matrix_free (x);
      return NULL;
    }
  return x;
}

/* --- value level ------------------------------------------------------- */

M42Value *
m42_value_dot (const M42Value *a, const M42Value *b)
{
  gboolean a_vec = m42_value_is_vector (a), b_vec = m42_value_is_vector (b);
  g_autoptr (M42Matrix) ma = m42_matrix_from_value (a, FALSE);  /* row */
  g_autoptr (M42Matrix) mb = m42_matrix_from_value (b, TRUE);   /* column */
  g_autoptr (M42Matrix) c = NULL;

  if (ma == NULL || mb == NULL)
    return m42_value_error ("Dot expects vectors or matrices");

  if (a_vec && b_vec)
    {
      double s = 0;
      if (ma->cols != mb->rows)
        return m42_value_error ("Dot: vectors of lengths %u and %u", ma->cols, mb->rows);
      for (guint i = 0; i < ma->cols; i++)
        s += ma->a[i] * mb->a[i];
      return m42_value_number (s);
    }
  c = m42_matrix_multiply (ma, mb);
  if (c == NULL)
    return m42_value_error ("Dot: shapes %ux%u and %ux%u do not fit",
                            ma->rows, ma->cols, mb->rows, mb->cols);
  return m42_matrix_to_value (c, a_vec || b_vec);
}

M42Value *
m42_value_transpose (const M42Value *v)
{
  g_autoptr (M42Matrix) m = m42_matrix_from_value (v, FALSE);
  g_autoptr (M42Matrix) t = NULL;

  if (m == NULL)
    return m42_value_error ("Transpose expects a matrix");
  t = m42_matrix_transpose (m);
  return m42_matrix_to_value (t, FALSE);
}

M42Value *
m42_value_det (const M42Value *v)
{
  g_autoptr (M42Matrix) m = m42_matrix_from_value (v, FALSE);

  if (m == NULL || m->rows != m->cols)
    return m42_value_error ("Det expects a square matrix");
  return m42_value_number (m42_matrix_det (m));
}

M42Value *
m42_value_inverse (const M42Value *v)
{
  g_autoptr (M42Matrix) m = m42_matrix_from_value (v, FALSE);
  g_autoptr (M42Matrix) inv = NULL;

  if (m == NULL || m->rows != m->cols)
    return m42_value_error ("Inverse expects a square matrix");
  inv = m42_matrix_inverse (m);
  if (inv == NULL)
    return m42_value_error ("Inverse: the matrix is singular");
  return m42_matrix_to_value (inv, FALSE);
}

M42Value *
m42_value_linear_solve (const M42Value *a, const M42Value *b)
{
  g_autoptr (M42Matrix) ma = m42_matrix_from_value (a, FALSE);
  g_autoptr (M42Matrix) mb = m42_matrix_from_value (b, TRUE);
  g_autoptr (M42Matrix) x = NULL;

  if (ma == NULL || ma->rows != ma->cols)
    return m42_value_error ("LinearSolve expects a square matrix");
  if (mb == NULL || mb->rows != ma->rows)
    return m42_value_error ("LinearSolve: the right-hand side does not fit");
  x = m42_matrix_solve (ma, mb);
  if (x == NULL)
    return m42_value_error ("LinearSolve: the matrix is singular");
  return m42_matrix_to_value (x, m42_value_is_vector (b));
}

/* --- eigenvalues ---------------------------------------------------------
 *
 * A symmetric matrix is turned by Jacobi rotations, which are steady and
 * give the eigenvectors as well.  Anything else goes through the QR
 * iteration; when a 2x2 block will not split, the eigenvalues there are
 * complex and math42 says so rather than inventing real ones.
 */

static gboolean
is_symmetric (const M42Matrix *m)
{
  if (m->rows != m->cols)
    return FALSE;
  for (guint i = 0; i < m->rows; i++)
    for (guint j = i + 1; j < m->cols; j++)
      {
        double a = m->a[i * m->cols + j], b = m->a[j * m->cols + i];
        if (fabs (a - b) > 1e-12 * (1 + fabs (a) + fabs (b)))
          return FALSE;
      }
  return TRUE;
}

/* Jacobi: rotate away the largest off-diagonal entry, over and over.
 * On return the diagonal of a holds the eigenvalues and the columns of
 * v (when given) the eigenvectors. */
static void
jacobi (M42Matrix *a, M42Matrix *v)
{
  guint n = a->rows;

  if (v != NULL)
    for (guint i = 0; i < n; i++)
      *m42_matrix_at (v, i, i) = 1;

  for (int sweep = 0; sweep < 100; sweep++)
    {
      double off = 0;
      guint p = 0, q = 1;
      double best = -1;

      for (guint i = 0; i < n; i++)
        for (guint j = i + 1; j < n; j++)
          {
            double x = fabs (*m42_matrix_at (a, i, j));
            off += x * x;
            if (x > best)
              {
                best = x;
                p = i;
                q = j;
              }
          }
      if (n < 2 || off < 1e-26)
        return;

      {
        double app = *m42_matrix_at (a, p, p);
        double aqq = *m42_matrix_at (a, q, q);
        double apq = *m42_matrix_at (a, p, q);
        double theta, t, c, s;

        if (fabs (apq) < 1e-300)
          return;
        theta = (aqq - app) / (2 * apq);
        t = (theta >= 0 ? 1.0 : -1.0) / (fabs (theta) + sqrt (theta * theta + 1));
        c = 1 / sqrt (t * t + 1);
        s = t * c;

        for (guint k = 0; k < n; k++)
          {
            double akp = *m42_matrix_at (a, k, p), akq = *m42_matrix_at (a, k, q);
            *m42_matrix_at (a, k, p) = c * akp - s * akq;
            *m42_matrix_at (a, k, q) = s * akp + c * akq;
          }
        for (guint k = 0; k < n; k++)
          {
            double apk = *m42_matrix_at (a, p, k), aqk = *m42_matrix_at (a, q, k);
            *m42_matrix_at (a, p, k) = c * apk - s * aqk;
            *m42_matrix_at (a, q, k) = s * apk + c * aqk;
          }
        if (v != NULL)
          for (guint k = 0; k < n; k++)
            {
              double vkp = *m42_matrix_at (v, k, p), vkq = *m42_matrix_at (v, k, q);
              *m42_matrix_at (v, k, p) = c * vkp - s * vkq;
              *m42_matrix_at (v, k, q) = s * vkp + c * vkq;
            }
      }
    }
}

/* Gram-Schmidt QR, used by the iteration below. */
static void
qr_decompose (const M42Matrix *a, M42Matrix *q, M42Matrix *r)
{
  guint n = a->rows;

  for (guint j = 0; j < n; j++)
    {
      g_autofree double *v = g_new (double, n);
      double norm = 0;

      for (guint i = 0; i < n; i++)
        v[i] = a->a[i * n + j];
      for (guint k = 0; k < j; k++)
        {
          double dot = 0;
          for (guint i = 0; i < n; i++)
            dot += q->a[i * n + k] * a->a[i * n + j];
          *m42_matrix_at (r, k, j) = dot;
          for (guint i = 0; i < n; i++)
            v[i] -= dot * q->a[i * n + k];
        }
      for (guint i = 0; i < n; i++)
        norm += v[i] * v[i];
      norm = sqrt (norm);
      *m42_matrix_at (r, j, j) = norm;
      for (guint i = 0; i < n; i++)
        *m42_matrix_at (q, i, j) = norm > 1e-300 ? v[i] / norm : 0;
    }
}

/* The unshifted QR iteration: A = QR, then A <- RQ, until what is below
 * the diagonal has gone.  Returns FALSE when a 2x2 block survives. */
static gboolean
qr_iterate (M42Matrix *a)
{
  guint n = a->rows;
  g_autoptr (M42Matrix) q = m42_matrix_new (n, n);
  g_autoptr (M42Matrix) r = m42_matrix_new (n, n);

  for (int iter = 0; iter < 2000; iter++)
    {
      gboolean converged = TRUE;
      g_autoptr (M42Matrix) next = NULL;

      for (guint i = 1; i < n; i++)
        for (guint j = 0; j < i; j++)
          if (fabs (*m42_matrix_at (a, i, j)) > 1e-11 * (1 + fabs (*m42_matrix_at (a, i, i))))
            converged = FALSE;
      if (converged)
        return TRUE;

      qr_decompose (a, q, r);
      next = m42_matrix_multiply (r, q);
      if (next == NULL)
        return FALSE;
      memcpy (a->a, next->a, sizeof (double) * n * n);
    }

  /* One last look: a lone subdiagonal entry left in a 2x2 block means a
   * complex pair. */
  for (guint i = 1; i < n; i++)
    for (guint j = 0; j + 1 < i; j++)
      if (fabs (*m42_matrix_at (a, i, j)) > 1e-8)
        return FALSE;
  for (guint i = 1; i < n; i++)
    if (fabs (*m42_matrix_at (a, i, i - 1)) > 1e-8)
      return FALSE;
  return TRUE;
}

/* A number that is a whisker from a whole one is that whole one: the
 * arithmetic, not the matrix, put the whisker there. */
static double
tidy (double x)
{
  if (fabs (x - round (x)) < 1e-9 * MAX (1.0, fabs (x)))
    return round (x);
  return fabs (x) < 1e-12 ? 0 : x;
}

typedef struct { double re, im; } Eigenvalue;

static int
compare_eigenvalues (gconstpointer a, gconstpointer b)
{
  const Eigenvalue *x = a, *y = b;
  double mx = hypot (x->re, x->im), my = hypot (y->re, y->im);

  if (mx != my)
    return mx < my ? 1 : -1;      /* largest first */
  if (x->re != y->re)
    return x->re < y->re ? 1 : -1;
  return x->im < y->im ? 1 : -1;
}

M42Value *
m42_value_eigenvalues (const M42Value *v)
{
  g_autoptr (M42Matrix) m = m42_matrix_from_value (v, FALSE);
  g_autoptr (GArray) vals = g_array_new (FALSE, FALSE, sizeof (Eigenvalue));
  M42Value *out;
  guint n;

  if (m == NULL || m->rows != m->cols)
    return m42_value_error ("Eigenvalues expects a square matrix");
  n = m->rows;

  if (is_symmetric (m))
    {
      jacobi (m, NULL);
      for (guint i = 0; i < n; i++)
        {
          Eigenvalue e = { tidy (*m42_matrix_at (m, i, i)), 0 };
          g_array_append_val (vals, e);
        }
    }
  else
    {
      qr_iterate (m);
      /* What the iteration leaves is triangular but for the odd 2x2
       * block, whose pair of eigenvalues is complex; the quadratic
       * formula finishes those off. */
      for (guint i = 0; i < n; i++)
        {
          gboolean block = i + 1 < n &&
                           fabs (*m42_matrix_at (m, i + 1, i)) >
                             1e-9 * (1 + fabs (*m42_matrix_at (m, i, i)));

          if (block)
            {
              double a = *m42_matrix_at (m, i, i), b = *m42_matrix_at (m, i, i + 1);
              double c = *m42_matrix_at (m, i + 1, i), d = *m42_matrix_at (m, i + 1, i + 1);
              double trace = a + d, det = a * d - b * c;
              double disc = trace * trace / 4 - det;

              if (disc >= 0)
                {
                  Eigenvalue e1 = { tidy (trace / 2 + sqrt (disc)), 0 };
                  Eigenvalue e2 = { tidy (trace / 2 - sqrt (disc)), 0 };
                  g_array_append_val (vals, e1);
                  g_array_append_val (vals, e2);
                }
              else
                {
                  Eigenvalue e1 = { tidy (trace / 2), tidy (sqrt (-disc)) };
                  Eigenvalue e2 = { tidy (trace / 2), tidy (-sqrt (-disc)) };
                  g_array_append_val (vals, e1);
                  g_array_append_val (vals, e2);
                }
              i++;
            }
          else
            {
              Eigenvalue e = { tidy (*m42_matrix_at (m, i, i)), 0 };
              g_array_append_val (vals, e);
            }
        }
    }

  g_array_sort (vals, compare_eigenvalues);

  out = m42_value_list_new ();
  for (guint i = 0; i < vals->len; i++)
    {
      Eigenvalue *e = &g_array_index (vals, Eigenvalue, i);
      m42_value_list_append (out, m42_value_complex (e->re, e->im));
    }
  return out;
}

static M42Matrix *row_reduce (const M42Matrix *m, GArray *pivots);

/* The vector belonging to one real eigenvalue: whatever A - lambda I
 * sends to nothing.  Row reduction finds it, and the eigenvalue being
 * a little off only makes the pivot it should not have a little
 * larger than nothing, which the threshold in row_reduce already
 * allows for.  NULL when nothing came of it. */
static M42Value *
eigenvector_for (const M42Matrix *m, double lambda, guint which)
{
  guint n = m->rows;
  guint seen = 0;
  g_autoptr (M42Matrix) shifted = m42_matrix_new (n, n);
  g_autoptr (GArray) pivots = g_array_new (FALSE, FALSE, sizeof (guint));
  g_autoptr (M42Matrix) r = NULL;
  M42Value *out = NULL;
  double longest = 0;

  memcpy (shifted->a, m->a, sizeof (double) * n * n);
  for (guint i = 0; i < n; i++)
    *m42_matrix_at (shifted, i, i) -= lambda;

  r = row_reduce (shifted, pivots);
  for (guint col = 0; col < n && out == NULL; col++)
    {
      gboolean is_pivot = FALSE;

      for (guint i = 0; i < pivots->len; i++)
        if (g_array_index (pivots, guint, i) == col)
          is_pivot = TRUE;
      if (is_pivot)
        continue;
      /* A repeated eigenvalue asks for the second vector of its space,
       * and the third, in turn. */
      if (seen++ < which)
        continue;

      /* The free column set to one, the rest following from it. */
      out = m42_value_list_new ();
      for (guint j = 0; j < n; j++)
        {
          double x = 0;

          if (j == col)
            x = 1;
          else
            for (guint i = 0; i < pivots->len; i++)
              if (g_array_index (pivots, guint, i) == j)
                x = -*m42_matrix_at (r, i, col);
          longest = MAX (longest, fabs (x));
          m42_value_list_append (out, m42_value_number (tidy (x)));
        }
    }
  if (out != NULL && longest == 0)
    g_clear_pointer (&out, m42_value_unref);
  return out;
}

M42Value *
m42_value_eigenvectors (const M42Value *v)
{
  g_autoptr (M42Matrix) m = m42_matrix_from_value (v, FALSE);
  g_autoptr (M42Matrix) vecs = NULL;
  M42Value *out;
  guint n;

  if (m == NULL || m->rows != m->cols)
    return m42_value_error ("Eigenvectors expects a square matrix");

  /* Not symmetric: the eigenvalues first, and then what each of them
   * sends to nothing.  A complex pair has no real vector to give, and
   * math42 keeps its matrices real, so it says so. */
  if (!is_symmetric (m))
    {
      g_autoptr (M42Value) values = m42_value_eigenvalues (v);

      if (values->kind == M42_VALUE_ERROR)
        return m42_value_eigenvalues (v);   /* the same complaint, to hand on */
      out = m42_value_list_new ();
      for (guint i = 0; i < m42_value_list_length (values); i++)
        {
          M42Value *e = m42_value_list_nth (values, i);
          M42Value *vector;
          double lambda;

          if (e->kind == M42_VALUE_COMPLEX)
            {
              m42_value_unref (out);
              return m42_value_error ("Eigenvectors: that matrix has a complex pair, "
                                      "and math42 keeps its matrices real");
            }
          if (e->kind != M42_VALUE_NUMBER)
            {
              m42_value_unref (out);
              return m42_value_error ("Eigenvectors: that eigenvalue is not a number");
            }
          lambda = e->u.number;

          /* The same eigenvalue again wants the next vector of its
           * space. */
          {
            guint again = 0;

            for (guint k = 0; k < i; k++)
              {
                M42Value *before = m42_value_list_nth (values, k);

                if (before->kind == M42_VALUE_NUMBER &&
                    fabs (before->u.number - lambda) < 1e-9 * (1 + fabs (lambda)))
                  again++;
              }
            vector = eigenvector_for (m, lambda, again);
          }

          /* A matrix can have fewer eigenvectors than eigenvalues.
           * Mathematica fills the gap with a vector of nothing, which
           * says plainly that there is no further one, and so does
           * this. */
          if (vector == NULL)
            {
              vector = m42_value_list_new ();
              for (guint k = 0; k < m->rows; k++)
                m42_value_list_append (vector, m42_value_number (0));
            }
          m42_value_list_append (out, vector);
        }
      return out;
    }

  n = m->rows;
  vecs = m42_matrix_new (n, n);
  jacobi (m, vecs);

  /* Largest eigenvalue first, to match Eigenvalues. */
  out = m42_value_list_new ();
  {
    g_autofree gboolean *taken = g_new0 (gboolean, n);
    for (guint k = 0; k < n; k++)
      {
        guint best = 0;
        double best_val = -INFINITY;
        M42Value *row;

        for (guint i = 0; i < n; i++)
          if (!taken[i] && *m42_matrix_at (m, i, i) > best_val)
            {
              best_val = *m42_matrix_at (m, i, i);
              best = i;
            }
        taken[best] = TRUE;

        row = m42_value_list_new ();
        for (guint i = 0; i < n; i++)
          {
            double x = *m42_matrix_at (vecs, i, best);
            if (fabs (x) < 1e-12)
              x = 0;
            m42_value_list_append (row, m42_value_number (x));
          }
        m42_value_list_append (out, row);
      }
  }
  return out;
}

M42Value *
m42_value_matrix_power (const M42Value *v, int k)
{
  g_autoptr (M42Matrix) m = m42_matrix_from_value (v, FALSE);
  g_autoptr (M42Matrix) acc = NULL;

  if (m == NULL || m->rows != m->cols)
    return m42_value_error ("MatrixPower expects a square matrix");
  if (k < 0)
    {
      M42Matrix *inv = m42_matrix_inverse (m);
      if (inv == NULL)
        return m42_value_error ("MatrixPower: the matrix is singular");
      m42_matrix_free (m);
      m = inv;
      k = -k;
    }
  acc = m42_matrix_new (m->rows, m->rows);
  for (guint i = 0; i < m->rows; i++)
    *m42_matrix_at (acc, i, i) = 1;
  for (int i = 0; i < k; i++)
    {
      M42Matrix *next = m42_matrix_multiply (acc, m);
      m42_matrix_free (acc);
      acc = next;
    }
  return m42_matrix_to_value (acc, FALSE);
}

/* --- the rest of a linear algebra course --------------------------------
 *
 * Row reduction and what falls out of it -- the null space, a least
 * squares fit -- together with Gram-Schmidt, the characteristic
 * polynomial and the exponential of a matrix.
 */

/* The reduced row echelon form, with the pivot columns noted. */
static M42Matrix *
row_reduce (const M42Matrix *m, GArray *pivots)
{
  M42Matrix *r = m42_matrix_new (m->rows, m->cols);
  guint row = 0;

  memcpy (r->a, m->a, sizeof (double) * m->rows * m->cols);

  for (guint col = 0; col < r->cols && row < r->rows; col++)
    {
      guint best = row;

      for (guint i = row + 1; i < r->rows; i++)
        if (fabs (*m42_matrix_at (r, i, col)) > fabs (*m42_matrix_at (r, best, col)))
          best = i;
      if (fabs (*m42_matrix_at (r, best, col)) < 1e-10)
        continue;

      for (guint j = 0; j < r->cols; j++)
        {
          double t = *m42_matrix_at (r, row, j);
          *m42_matrix_at (r, row, j) = *m42_matrix_at (r, best, j);
          *m42_matrix_at (r, best, j) = t;
        }
      {
        double pivot = *m42_matrix_at (r, row, col);

        for (guint j = 0; j < r->cols; j++)
          *m42_matrix_at (r, row, j) /= pivot;
      }
      for (guint i = 0; i < r->rows; i++)
        {
          double factor;

          if (i == row)
            continue;
          factor = *m42_matrix_at (r, i, col);
          if (fabs (factor) < 1e-14)
            continue;
          for (guint j = 0; j < r->cols; j++)
            *m42_matrix_at (r, i, j) -= factor * *m42_matrix_at (r, row, j);
        }
      if (pivots != NULL)
        g_array_append_val (pivots, col);
      row++;
    }

  /* Tidy the whiskers left by the arithmetic. */
  for (guint i = 0; i < r->rows * r->cols; i++)
    if (fabs (r->a[i] - round (r->a[i])) < 1e-10)
      r->a[i] = round (r->a[i]);
  return r;
}

M42Value *
m42_value_row_reduce (const M42Value *v)
{
  g_autoptr (M42Matrix) m = m42_matrix_from_value (v, FALSE);
  g_autoptr (M42Matrix) r = NULL;

  if (m == NULL)
    return m42_value_error ("RowReduce expects a matrix");
  r = row_reduce (m, NULL);
  return m42_matrix_to_value (r, FALSE);
}

/* The null space: one vector for each column without a pivot. */
M42Value *
m42_value_null_space (const M42Value *v)
{
  g_autoptr (M42Matrix) m = m42_matrix_from_value (v, FALSE);
  g_autoptr (GArray) pivots = g_array_new (FALSE, FALSE, sizeof (guint));
  g_autoptr (M42Matrix) r = NULL;
  M42Value *out;

  if (m == NULL)
    return m42_value_error ("NullSpace expects a matrix");
  r = row_reduce (m, pivots);
  out = m42_value_list_new ();

  for (guint col = 0; col < r->cols; col++)
    {
      gboolean is_pivot = FALSE;
      M42Value *vector;

      for (guint i = 0; i < pivots->len; i++)
        if (g_array_index (pivots, guint, i) == col)
          is_pivot = TRUE;
      if (is_pivot)
        continue;

      /* The free column set to one, the pivots following from it. */
      vector = m42_value_list_new ();
      for (guint j = 0; j < r->cols; j++)
        {
          double x = 0;

          if (j == col)
            x = 1;
          else
            for (guint i = 0; i < pivots->len; i++)
              if (g_array_index (pivots, guint, i) == j)
                x = -*m42_matrix_at (r, i, col);
          m42_value_list_append (vector, m42_value_number (x == 0 ? 0 : x));
        }
      m42_value_list_append (out, vector);
    }
  return out;
}

/* Gram-Schmidt: the same space, with the vectors at right angles and
 * of length one. */
M42Value *
m42_value_orthogonalize (const M42Value *v)
{
  g_autoptr (M42Matrix) m = m42_matrix_from_value (v, FALSE);
  M42Value *out;

  if (m == NULL)
    return m42_value_error ("Orthogonalize expects a list of vectors");
  out = m42_value_list_new ();

  {
    g_autoptr (M42Matrix) kept = m42_matrix_new (m->rows, m->cols);
    guint count = 0;

    for (guint i = 0; i < m->rows; i++)
      {
        g_autofree double *w = g_new (double, m->cols);
        double norm = 0;

        for (guint j = 0; j < m->cols; j++)
          w[j] = *m42_matrix_at ((M42Matrix *) m, i, j);

        for (guint k = 0; k < count; k++)
          {
            double dot = 0;

            for (guint j = 0; j < m->cols; j++)
              dot += w[j] * *m42_matrix_at (kept, k, j);
            for (guint j = 0; j < m->cols; j++)
              w[j] -= dot * *m42_matrix_at (kept, k, j);
          }
        for (guint j = 0; j < m->cols; j++)
          norm += w[j] * w[j];
        norm = sqrt (norm);
        if (norm < 1e-10)
          continue;              /* it was already spanned */

        {
          M42Value *row = m42_value_list_new ();

          for (guint j = 0; j < m->cols; j++)
            {
              double x = w[j] / norm;

              *m42_matrix_at (kept, count, j) = x;
              m42_value_list_append (row, m42_value_real (fabs (x) < 1e-12 ? 0 : x));
            }
          m42_value_list_append (out, row);
          count++;
        }
      }
  }
  return out;
}

/* The line that fits best, through the normal equations. */
M42Value *
m42_value_least_squares (const M42Value *a, const M42Value *b)
{
  g_autoptr (M42Matrix) ma = m42_matrix_from_value (a, FALSE);
  g_autoptr (M42Matrix) mb = m42_matrix_from_value (b, TRUE);
  g_autoptr (M42Matrix) at = NULL;
  g_autoptr (M42Matrix) ata = NULL;
  g_autoptr (M42Matrix) atb = NULL;
  g_autoptr (M42Matrix) x = NULL;

  if (ma == NULL || mb == NULL || ma->rows != mb->rows)
    return m42_value_error ("LeastSquares expects a matrix and a column of the same height");
  at = m42_matrix_transpose (ma);
  ata = m42_matrix_multiply (at, ma);
  atb = m42_matrix_multiply (at, mb);
  if (ata == NULL || atb == NULL)
    return m42_value_error ("LeastSquares: those shapes do not fit");
  x = m42_matrix_solve (ata, atb);
  if (x == NULL)
    return m42_value_error ("LeastSquares: the columns are not independent");
  return m42_matrix_to_value (x, TRUE);
}

/* The coefficients of det(A - x I), lowest power first, by the method
 * of Faddeev and LeVerrier. */
M42Value *
m42_value_characteristic (const M42Value *v)
{
  g_autoptr (M42Matrix) a = m42_matrix_from_value (v, FALSE);
  guint n;
  g_autofree double *c = NULL;
  g_autoptr (M42Matrix) m = NULL;
  M42Value *out;

  if (a == NULL || a->rows != a->cols)
    return m42_value_error ("CharacteristicPolynomial expects a square matrix");
  n = a->rows;
  c = g_new0 (double, n + 1);
  c[n] = 1;
  m = m42_matrix_new (n, n);

  for (guint k = 1; k <= n; k++)
    {
      g_autoptr (M42Matrix) product = NULL;
      double trace = 0;

      /* M <- A M + c[n-k+1] I */
      if (k == 1)
        {
          memcpy (m->a, a->a, sizeof (double) * n * n);
        }
      else
        {
          product = m42_matrix_multiply (a, m);
          memcpy (m->a, product->a, sizeof (double) * n * n);
        }
      for (guint i = 0; i < n; i++)
        trace += *m42_matrix_at (m, i, i);
      c[n - k] = -trace / k;
      for (guint i = 0; i < n; i++)
        *m42_matrix_at (m, i, i) += c[n - k];
    }

  out = m42_value_list_new ();
  for (guint i = 0; i <= n; i++)
    {
      double x = c[i];

      if (fabs (x - round (x)) < 1e-9)
        x = round (x);
      m42_value_list_append (out, m42_value_number (x));
    }
  return out;
}

/* The exponential of a matrix, by squaring away the size and summing
 * what is left. */
M42Value *
m42_value_matrix_exp (const M42Value *v)
{
  g_autoptr (M42Matrix) a = m42_matrix_from_value (v, FALSE);
  g_autoptr (M42Matrix) scaled = NULL;
  g_autoptr (M42Matrix) sum = NULL;
  g_autoptr (M42Matrix) term = NULL;
  guint n;
  double norm = 0;
  int squarings = 0;

  if (a == NULL || a->rows != a->cols)
    return m42_value_error ("MatrixExp expects a square matrix");
  n = a->rows;

  for (guint i = 0; i < n * n; i++)
    norm = MAX (norm, fabs (a->a[i]));
  while (norm > 0.5)
    {
      norm /= 2;
      squarings++;
    }

  scaled = m42_matrix_new (n, n);
  for (guint i = 0; i < n * n; i++)
    scaled->a[i] = a->a[i] / pow (2, squarings);

  sum = m42_matrix_new (n, n);
  term = m42_matrix_new (n, n);
  for (guint i = 0; i < n; i++)
    {
      *m42_matrix_at (sum, i, i) = 1;
      *m42_matrix_at (term, i, i) = 1;
    }
  for (int k = 1; k <= 24; k++)
    {
      M42Matrix *next = m42_matrix_multiply (term, scaled);

      for (guint i = 0; i < n * n; i++)
        next->a[i] /= k;
      m42_matrix_free (term);
      term = next;
      for (guint i = 0; i < n * n; i++)
        sum->a[i] += term->a[i];
    }
  for (int k = 0; k < squarings; k++)
    {
      M42Matrix *squared = m42_matrix_multiply (sum, sum);

      m42_matrix_free (sum);
      sum = squared;
    }
  for (guint i = 0; i < n * n; i++)
    if (fabs (sum->a[i] - round (sum->a[i])) < 1e-9)
      sum->a[i] = round (sum->a[i]);
  return m42_matrix_to_value (sum, FALSE);
}

/* --- the decompositions ---------------------------------------------------
 *
 * LU with partial pivoting, QR by Gram-Schmidt, and the singular values
 * through the eigenvalues of A transpose A -- which is how they are
 * found by hand, and good enough for matrices of the size anyone types.
 */

static void
tidy_matrix (M42Matrix *m)
{
  for (guint i = 0; i < m->rows * m->cols; i++)
    {
      double x = m->a[i];

      if (fabs (x - round (x)) < 1e-10)
        m->a[i] = round (x);
      else if (fabs (x) < 1e-12)
        m->a[i] = 0;
    }
}

/* {L, U, p}: L below the diagonal with ones on it, U above, and the
 * order the rows were taken in. */
M42Value *
m42_value_lu (const M42Value *v)
{
  g_autoptr (M42Matrix) a = m42_matrix_from_value (v, FALSE);
  guint n;
  g_autoptr (M42Matrix) lower = NULL;
  g_autoptr (M42Matrix) upper = NULL;
  g_autofree guint *order = NULL;
  M42Value *out, *permutation;

  if (a == NULL || a->rows != a->cols)
    return m42_value_error ("LUDecomposition expects a square matrix");
  n = a->rows;
  lower = m42_matrix_new (n, n);
  upper = m42_matrix_new (n, n);
  memcpy (upper->a, a->a, sizeof (double) * n * n);
  order = g_new (guint, n);
  for (guint i = 0; i < n; i++)
    order[i] = i;

  for (guint col = 0; col < n; col++)
    {
      guint best = col;

      for (guint i = col + 1; i < n; i++)
        if (fabs (*m42_matrix_at (upper, i, col)) > fabs (*m42_matrix_at (upper, best, col)))
          best = i;
      if (fabs (*m42_matrix_at (upper, best, col)) < 1e-14)
        continue;

      if (best != col)
        {
          guint t = order[col];

          order[col] = order[best];
          order[best] = t;
          for (guint j = 0; j < n; j++)
            {
              double x = *m42_matrix_at (upper, col, j);

              *m42_matrix_at (upper, col, j) = *m42_matrix_at (upper, best, j);
              *m42_matrix_at (upper, best, j) = x;

              x = *m42_matrix_at (lower, col, j);
              *m42_matrix_at (lower, col, j) = *m42_matrix_at (lower, best, j);
              *m42_matrix_at (lower, best, j) = x;
            }
        }

      for (guint i = col + 1; i < n; i++)
        {
          double factor = *m42_matrix_at (upper, i, col) / *m42_matrix_at (upper, col, col);

          *m42_matrix_at (lower, i, col) = factor;
          for (guint j = 0; j < n; j++)
            *m42_matrix_at (upper, i, j) -= factor * *m42_matrix_at (upper, col, j);
        }
    }
  for (guint i = 0; i < n; i++)
    *m42_matrix_at (lower, i, i) = 1;

  tidy_matrix (lower);
  tidy_matrix (upper);
  out = m42_value_list_new ();
  m42_value_list_append (out, m42_matrix_to_value (lower, FALSE));
  m42_value_list_append (out, m42_matrix_to_value (upper, FALSE));
  permutation = m42_value_list_new ();
  for (guint i = 0; i < n; i++)
    m42_value_list_append (permutation, m42_value_number (order[i] + 1));
  m42_value_list_append (out, permutation);
  return out;
}

/* {Q, R} with A = Q R: Gram-Schmidt down the columns. */
M42Value *
m42_value_qr (const M42Value *v)
{
  g_autoptr (M42Matrix) a = m42_matrix_from_value (v, FALSE);
  guint rows, cols;
  g_autoptr (M42Matrix) q = NULL;
  g_autoptr (M42Matrix) r = NULL;
  M42Value *out;

  if (a == NULL)
    return m42_value_error ("QRDecomposition expects a matrix");
  rows = a->rows;
  cols = a->cols;
  q = m42_matrix_new (rows, cols);
  r = m42_matrix_new (cols, cols);

  for (guint j = 0; j < cols; j++)
    {
      g_autofree double *w = g_new (double, rows);
      double norm = 0;

      for (guint i = 0; i < rows; i++)
        w[i] = *m42_matrix_at (a, i, j);
      for (guint k = 0; k < j; k++)
        {
          double dot = 0;

          for (guint i = 0; i < rows; i++)
            dot += *m42_matrix_at (q, i, k) * *m42_matrix_at (a, i, j);
          *m42_matrix_at (r, k, j) = dot;
          for (guint i = 0; i < rows; i++)
            w[i] -= dot * *m42_matrix_at (q, i, k);
        }
      for (guint i = 0; i < rows; i++)
        norm += w[i] * w[i];
      norm = sqrt (norm);
      *m42_matrix_at (r, j, j) = norm;
      if (norm > 1e-12)
        for (guint i = 0; i < rows; i++)
          *m42_matrix_at (q, i, j) = w[i] / norm;
    }

  tidy_matrix (q);
  tidy_matrix (r);
  out = m42_value_list_new ();
  m42_value_list_append (out, m42_matrix_to_value (q, FALSE));
  m42_value_list_append (out, m42_matrix_to_value (r, FALSE));
  return out;
}

/* The singular values, largest first, and if wanted the two matrices
 * that go with them. */
static gboolean
singular_values (const M42Matrix *a, GArray *values, M42Matrix **u_out, M42Matrix **v_out)
{
  guint rows = a->rows, cols = a->cols;
  g_autoptr (M42Matrix) at = m42_matrix_transpose (a);
  g_autoptr (M42Matrix) ata = m42_matrix_multiply (at, a);
  g_autoptr (M42Matrix) vectors = NULL;

  if (ata == NULL)
    return FALSE;
  vectors = m42_matrix_new (cols, cols);
  jacobi (ata, vectors);

  /* Largest first: the order the eigenvalues are put in. */
  {
    g_autofree guint *order = g_new (guint, cols);
    g_autofree gboolean *taken = g_new0 (gboolean, cols);

    for (guint k = 0; k < cols; k++)
      {
        guint best = 0;
        double largest = -INFINITY;

        for (guint i = 0; i < cols; i++)
          if (!taken[i] && *m42_matrix_at (ata, i, i) > largest)
            {
              largest = *m42_matrix_at (ata, i, i);
              best = i;
            }
        taken[best] = TRUE;
        order[k] = best;
      }

    for (guint k = 0; k < cols; k++)
      {
        double lambda = *m42_matrix_at (ata, order[k], order[k]);
        double sigma = lambda > 0 ? sqrt (lambda) : 0;

        if (fabs (sigma - round (sigma)) < 1e-9)
          sigma = round (sigma);
        g_array_append_val (values, sigma);
      }

    if (v_out != NULL)
      {
        M42Matrix *vm = m42_matrix_new (cols, cols);

        for (guint k = 0; k < cols; k++)
          for (guint i = 0; i < cols; i++)
            *m42_matrix_at (vm, i, k) = *m42_matrix_at (vectors, i, order[k]);
        tidy_matrix (vm);
        *v_out = vm;
      }
    if (u_out != NULL)
      {
        M42Matrix *um = m42_matrix_new (rows, cols);

        for (guint k = 0; k < cols; k++)
          {
            double sigma = g_array_index (values, double, k);

            if (sigma < 1e-12)
              continue;
            for (guint i = 0; i < rows; i++)
              {
                double sum = 0;

                for (guint j = 0; j < cols; j++)
                  sum += *m42_matrix_at ((M42Matrix *) a, i, j) *
                         *m42_matrix_at (vectors, j, order[k]);
                *m42_matrix_at (um, i, k) = sum / sigma;
              }
          }
        tidy_matrix (um);
        *u_out = um;
      }
  }
  return TRUE;
}

M42Value *
m42_value_singular_values (const M42Value *v)
{
  g_autoptr (M42Matrix) a = m42_matrix_from_value (v, FALSE);
  g_autoptr (GArray) values = g_array_new (FALSE, FALSE, sizeof (double));
  M42Value *out;

  if (a == NULL)
    return m42_value_error ("SingularValueList expects a matrix");
  if (!singular_values (a, values, NULL, NULL))
    return m42_value_error ("SingularValueList: that matrix will not decompose");
  out = m42_value_list_new ();
  for (guint i = 0; i < values->len; i++)
    m42_value_list_append (out, m42_value_number (g_array_index (values, double, i)));
  return out;
}

M42Value *
m42_value_svd (const M42Value *v)
{
  g_autoptr (M42Matrix) a = m42_matrix_from_value (v, FALSE);
  g_autoptr (GArray) values = g_array_new (FALSE, FALSE, sizeof (double));
  M42Matrix *u = NULL, *vm = NULL;
  g_autoptr (M42Matrix) sigma = NULL;
  M42Value *out;

  if (a == NULL)
    return m42_value_error ("SingularValueDecomposition expects a matrix");
  if (!singular_values (a, values, &u, &vm))
    return m42_value_error ("SingularValueDecomposition: that matrix will not decompose");

  sigma = m42_matrix_new (values->len, values->len);
  for (guint i = 0; i < values->len; i++)
    *m42_matrix_at (sigma, i, i) = g_array_index (values, double, i);

  out = m42_value_list_new ();
  m42_value_list_append (out, m42_matrix_to_value (u, FALSE));
  m42_value_list_append (out, m42_matrix_to_value (sigma, FALSE));
  m42_value_list_append (out, m42_matrix_to_value (vm, FALSE));
  m42_matrix_free (u);
  m42_matrix_free (vm);
  return out;
}

/* The inverse where there is one, and the nearest thing to it where
 * there is not: V, the singular values turned over, U transposed. */
M42Value *
m42_value_pseudo_inverse (const M42Value *v)
{
  g_autoptr (M42Matrix) a = m42_matrix_from_value (v, FALSE);
  g_autoptr (GArray) values = g_array_new (FALSE, FALSE, sizeof (double));
  M42Matrix *u = NULL, *vm = NULL;
  g_autoptr (M42Matrix) result = NULL;

  if (a == NULL)
    return m42_value_error ("PseudoInverse expects a matrix");
  if (!singular_values (a, values, &u, &vm))
    return m42_value_error ("PseudoInverse: that matrix will not decompose");

  result = m42_matrix_new (a->cols, a->rows);
  for (guint i = 0; i < a->cols; i++)
    for (guint j = 0; j < a->rows; j++)
      {
        double sum = 0;

        for (guint k = 0; k < values->len; k++)
          {
            double sigma = g_array_index (values, double, k);

            if (sigma < 1e-10)
              continue;
            sum += *m42_matrix_at (vm, i, k) * *m42_matrix_at (u, j, k) / sigma;
          }
        *m42_matrix_at (result, i, j) = sum;
      }
  m42_matrix_free (u);
  m42_matrix_free (vm);
  tidy_matrix (result);
  return m42_matrix_to_value (result, FALSE);
}

/* How badly a matrix magnifies a small change: the largest singular
 * value over the smallest. */
M42Value *
m42_value_condition (const M42Value *v)
{
  g_autoptr (M42Matrix) a = m42_matrix_from_value (v, FALSE);
  g_autoptr (GArray) values = g_array_new (FALSE, FALSE, sizeof (double));
  double largest, smallest;

  if (a == NULL)
    return m42_value_error ("Cond expects a matrix");
  if (!singular_values (a, values, NULL, NULL) || values->len == 0)
    return m42_value_error ("Cond: that matrix will not decompose");
  largest = g_array_index (values, double, 0);
  smallest = g_array_index (values, double, values->len - 1);
  if (smallest < 1e-14)
    return m42_value_real (INFINITY);
  return m42_value_real (largest / smallest);
}

/* {values, vectors}, the pair Mathematica calls Eigensystem. */
M42Value *
m42_value_eigensystem (const M42Value *v)
{
  M42Value *values = m42_value_eigenvalues (v);
  M42Value *vectors;
  M42Value *out;

  if (values->kind == M42_VALUE_ERROR)
    return values;
  vectors = m42_value_eigenvectors (v);
  if (vectors->kind == M42_VALUE_ERROR)
    {
      m42_value_unref (values);
      return vectors;
    }
  out = m42_value_list_new ();
  m42_value_list_append (out, values);
  m42_value_list_append (out, vectors);
  return out;
}
