/* m42-value.c - see m42-value.h
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m42-value.h"
#include "m42-symbolic.h"

#include <math.h>
#include <stdarg.h>

static M42Value *
value_alloc (M42ValueKind kind)
{
  M42Value *v = g_new0 (M42Value, 1);
  v->kind = kind;
  v->ref_count = 1;
  return v;
}

M42Value *
m42_value_real (double x)
{
  M42Value *v = value_alloc (M42_VALUE_NUMBER);
  v->u.number = x;
  return v;
}

M42Value *
m42_value_number (double x)
{
  M42Value *v = m42_value_real (x);

  /* Whole numbers are exact, which is what makes 6/4 come out as 3/2
   * rather than 1.5. */
  if (x == floor (x) && fabs (x) <= 1e15)
    {
      v->exact = TRUE;
      v->num = (gint64) x;
      v->den = 1;
    }
  return v;
}

M42Value *
m42_value_complex (double re, double im)
{
  M42Value *v;

  if (im == 0)
    return m42_value_number (re);
  v = value_alloc (M42_VALUE_COMPLEX);
  v->u.cx.re = re;
  v->u.cx.im = im;
  return v;
}

M42Value *
m42_value_exact_int (gint64 x)
{
  M42Value *v = m42_value_real ((double) x);

  v->exact = TRUE;
  v->num = x;
  v->den = 1;
  return v;
}

M42Value *
m42_value_bigint (M42Big *big)
{
  M42Value *v;
  gint64 small;

  if (big == NULL)
    return m42_value_error ("That number is larger than math42 will hold");
  if (m42_big_fits_int64 (big, &small))
    {
      m42_big_free (big);
      return m42_value_exact_int (small);
    }
  v = value_alloc (M42_VALUE_BIGINT);
  v->u.big = big;
  v->exact = TRUE;
  return v;
}

M42Value *
m42_value_string (const char *text)
{
  M42Value *v = value_alloc (M42_VALUE_STRING);
  v->u.string = g_strdup (text);
  return v;
}

static gint64
gcd64 (gint64 a, gint64 b)
{
  a = ABS (a);
  b = ABS (b);
  while (b != 0)
    {
      gint64 t = a % b;
      a = b;
      b = t;
    }
  return a == 0 ? 1 : a;
}

M42Value *
m42_value_rational (gint64 num, gint64 den)
{
  gint64 g;
  M42Value *v;

  if (den == 0)
    return m42_value_real (num == 0 ? NAN : (num > 0 ? INFINITY : -INFINITY));
  if (den < 0)
    {
      num = -num;
      den = -den;
    }
  g = gcd64 (num, den);
  num /= g;
  den /= g;

  v = m42_value_real ((double) num / (double) den);
  v->exact = TRUE;
  v->num = num;
  v->den = den;
  return v;
}

M42Value *
m42_value_list_new (void)
{
  M42Value *v = value_alloc (M42_VALUE_LIST);
  v->u.list = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
  return v;
}

M42Value *
m42_value_expr (M42Node *node)
{
  M42Value *v = value_alloc (M42_VALUE_EXPR);
  v->u.expr = node;
  return v;
}

M42Value *
m42_value_func (GStrv params, M42Node *body)
{
  M42Value *v = value_alloc (M42_VALUE_FUNC);
  v->u.func.params = params;
  v->u.func.body = body;
  return v;
}

M42Value *
m42_value_null (void)
{
  return value_alloc (M42_VALUE_NULL);
}

static void
series_free (M42Series *s)
{
  g_array_unref (s->points);
  g_free (s);
}

static void
contour_free (M42Contour *c)
{
  g_array_unref (c->segments);
  g_free (c);
}

static void
plot_free (M42Plot *p)
{
  g_clear_pointer (&p->contours, g_ptr_array_unref);
  g_clear_pointer (&p->curves, g_ptr_array_unref);
  g_clear_pointer (&p->arrows, g_array_unref);
  if (p->surface != NULL)
    {
      g_free (p->surface->z);
      g_free (p->surface);
    }
  g_ptr_array_unref (p->series);
  g_free (p->title);
  g_free (p->xlabel);
  g_free (p->ylabel);
  g_free (p);
}

M42Value *
m42_value_plot_new (void)
{
  M42Value *v = value_alloc (M42_VALUE_PLOT);
  v->u.plot = g_new0 (M42Plot, 1);
  v->u.plot->series = g_ptr_array_new_with_free_func ((GDestroyNotify) series_free);
  return v;
}

M42Value *
m42_value_error (const char *fmt, ...)
{
  M42Value *v = value_alloc (M42_VALUE_ERROR);
  va_list ap;

  va_start (ap, fmt);
  v->u.error = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  return v;
}

M42Value *
m42_value_ref (M42Value *v)
{
  v->ref_count++;
  return v;
}

void
m42_value_unref (M42Value *v)
{
  if (v == NULL || --v->ref_count > 0)
    return;

  switch (v->kind)
    {
    case M42_VALUE_LIST:  g_ptr_array_unref (v->u.list); break;
    case M42_VALUE_EXPR:  m42_node_free (v->u.expr); break;
    case M42_VALUE_FUNC:
      g_strfreev (v->u.func.params);
      m42_node_free (v->u.func.body);
      break;
    case M42_VALUE_PLOT:  plot_free (v->u.plot); break;
    case M42_VALUE_STRING: g_free (v->u.string); break;
    case M42_VALUE_BIGINT: m42_big_free (v->u.big); break;
    case M42_VALUE_ERROR: g_free (v->u.error); break;
    case M42_VALUE_NUMBER:
    case M42_VALUE_COMPLEX:
    case M42_VALUE_NULL:  break;
    }
  g_free (v);
}

void
m42_value_list_append (M42Value *list, M42Value *item)
{
  g_return_if_fail (list->kind == M42_VALUE_LIST);
  g_ptr_array_add (list->u.list, item);
}

guint
m42_value_list_length (const M42Value *list)
{
  g_return_val_if_fail (list->kind == M42_VALUE_LIST, 0);
  return list->u.list->len;
}

M42Value *
m42_value_list_nth (const M42Value *list, guint i)
{
  g_return_val_if_fail (list->kind == M42_VALUE_LIST, NULL);
  g_return_val_if_fail (i < list->u.list->len, NULL);
  return g_ptr_array_index (list->u.list, i);
}

gboolean
m42_value_is_vector (const M42Value *v)
{
  if (v->kind != M42_VALUE_LIST || v->u.list->len == 0)
    return FALSE;
  for (guint i = 0; i < v->u.list->len; i++)
    if (((M42Value *) g_ptr_array_index (v->u.list, i))->kind != M42_VALUE_NUMBER)
      return FALSE;
  return TRUE;
}

gboolean
m42_value_is_matrix (const M42Value *v, guint *rows, guint *cols)
{
  guint n;

  if (v->kind != M42_VALUE_LIST || v->u.list->len == 0)
    return FALSE;
  {
    M42Value *first = g_ptr_array_index (v->u.list, 0);
    if (!m42_value_is_vector (first))
      return FALSE;
    n = first->u.list->len;
  }
  for (guint i = 1; i < v->u.list->len; i++)
    {
      M42Value *row = g_ptr_array_index (v->u.list, i);
      if (!m42_value_is_vector (row) || row->u.list->len != n)
        return FALSE;
    }
  if (rows != NULL)
    *rows = v->u.list->len;
  if (cols != NULL)
    *cols = n;
  return TRUE;
}

/* --- plots ------------------------------------------------------------ */

static const double PALETTE[][3] = {
  { 0.37, 0.51, 0.71 }, { 0.88, 0.61, 0.14 }, { 0.56, 0.69, 0.19 },
  { 0.92, 0.39, 0.29 }, { 0.53, 0.38, 0.72 }, { 0.32, 0.65, 0.66 },
};

M42Series *
m42_plot_add_series (M42Plot *plot, M42SeriesKind kind)
{
  M42Series *s = g_new0 (M42Series, 1);
  const double *c = PALETTE[plot->series->len % G_N_ELEMENTS (PALETTE)];

  s->points = g_array_new (FALSE, FALSE, sizeof (double));
  s->kind = kind;
  s->r = c[0]; s->g = c[1]; s->b = c[2];
  g_ptr_array_add (plot->series, s);
  return s;
}

void
m42_plot_add_arrow (M42Plot *plot, double x, double y, double dx, double dy,
                    double strength)
{
  double one[5] = { x, y, dx, dy, strength };

  if (plot->arrows == NULL)
    plot->arrows = g_array_new (FALSE, FALSE, sizeof (double));
  g_array_append_vals (plot->arrows, one, 5);
}

static void
curve3d_free (M42Curve3D *c)
{
  g_array_unref (c->points);
  g_free (c);
}

M42Curve3D *
m42_plot_add_curve3d (M42Plot *plot)
{
  M42Curve3D *c = g_new0 (M42Curve3D, 1);

  c->points = g_array_new (FALSE, FALSE, sizeof (double));
  c->r = 0.20;
  c->g = 0.40;
  c->b = 0.75;
  c->xmin = c->ymin = c->zmin = INFINITY;
  c->xmax = c->ymax = c->zmax = -INFINITY;
  if (plot->curves == NULL)
    plot->curves = g_ptr_array_new_with_free_func ((GDestroyNotify) curve3d_free);
  g_ptr_array_add (plot->curves, c);
  return c;
}

void
m42_curve3d_add_point (M42Curve3D *c, double x, double y, double z)
{
  double xyz[3] = { x, y, z };

  if (!isfinite (x) || !isfinite (y) || !isfinite (z))
    return;
  g_array_append_vals (c->points, xyz, 3);
  c->xmin = MIN (c->xmin, x);  c->xmax = MAX (c->xmax, x);
  c->ymin = MIN (c->ymin, y);  c->ymax = MAX (c->ymax, y);
  c->zmin = MIN (c->zmin, z);  c->zmax = MAX (c->zmax, z);
}

/* A contour, coloured by where its level falls among the others. */
M42Contour *
m42_plot_add_contour (M42Plot *plot, double level, guint which, guint of)
{
  M42Contour *c = g_new0 (M42Contour, 1);
  double t = of > 1 ? (double) which / (of - 1) : 0.5;

  c->level = level;
  c->segments = g_array_new (FALSE, FALSE, sizeof (double));
  /* Cool below, warm above, the way a map is coloured. */
  c->r = 0.25 + 0.65 * t;
  c->g = 0.40 + 0.22 * (t < 0.5 ? t * 2 : (1 - t) * 2);
  c->b = 0.70 - 0.46 * t;

  if (plot->contours == NULL)
    plot->contours = g_ptr_array_new_with_free_func ((GDestroyNotify) contour_free);
  g_ptr_array_add (plot->contours, c);
  return c;
}

void
m42_contour_add_segment (M42Contour *c, double x1, double y1, double x2, double y2)
{
  g_array_append_val (c->segments, x1);
  g_array_append_val (c->segments, y1);
  g_array_append_val (c->segments, x2);
  g_array_append_val (c->segments, y2);
}

M42Surface *
m42_plot_add_surface (M42Plot *plot, guint nx, guint ny)
{
  M42Surface *s = g_new0 (M42Surface, 1);

  s->nx = nx;
  s->ny = ny;
  s->z = g_new0 (double, (gsize) nx * ny);
  /* A second surface on the same plot replaces the first, whose grid
   * has to go with it. */
  if (plot->surface != NULL)
    g_free (plot->surface->z);
  g_free (plot->surface);
  plot->surface = s;
  return s;
}

void
m42_surface_autoscale (M42Surface *s)
{
  double lo = INFINITY, hi = -INFINITY;

  for (guint i = 0; i < s->nx * s->ny; i++)
    {
      double z = s->z[i];
      if (!isfinite (z))
        continue;
      lo = MIN (lo, z);
      hi = MAX (hi, z);
    }
  if (!isfinite (lo))
    {
      lo = 0;
      hi = 1;
    }
  if (hi == lo)
    {
      lo -= 1;
      hi += 1;
    }
  s->zmin = lo;
  s->zmax = hi;
}

void
m42_series_add_point (M42Series *s, double x, double y)
{
  g_array_append_val (s->points, x);
  g_array_append_val (s->points, y);
}

void
m42_plot_autoscale (M42Plot *plot)
{
  double xmin = INFINITY, xmax = -INFINITY, ymin = INFINITY, ymax = -INFINITY;

  for (guint i = 0; i < plot->series->len; i++)
    {
      M42Series *s = g_ptr_array_index (plot->series, i);
      for (guint j = 0; j + 1 < s->points->len; j += 2)
        {
          double x = g_array_index (s->points, double, j);
          double y = g_array_index (s->points, double, j + 1);
          if (!isfinite (x) || !isfinite (y))
            continue;
          xmin = MIN (xmin, x); xmax = MAX (xmax, x);
          ymin = MIN (ymin, y); ymax = MAX (ymax, y);
        }
    }
  if (!isfinite (xmin))
    {
      xmin = ymin = 0;
      xmax = ymax = 1;
    }
  if (xmax == xmin) { xmin -= 1; xmax += 1; }
  if (ymax == ymin) { ymin -= 1; ymax += 1; }
  plot->xmin = xmin;
  plot->xmax = xmax;
  plot->ymin = ymin - (ymax - ymin) * 0.05;
  plot->ymax = ymax + (ymax - ymin) * 0.05;
}

/* --- printing ---------------------------------------------------------- */

void
m42_number_to_string (GString *out, double x)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  if (isnan (x))
    g_string_append (out, "Indeterminate");
  else if (isinf (x))
    g_string_append (out, x > 0 ? "Infinity" : "-Infinity");
  else if (x == floor (x) && fabs (x) < 1e15)
    g_string_append_printf (out, "%.0f", x == 0 ? 0.0 : x);
  else
    g_string_append (out, g_ascii_formatd (buf, sizeof buf, "%.15g", x));
}

static void
value_to_string (GString *out, const M42Value *v)
{
  switch (v->kind)
    {
    case M42_VALUE_NUMBER:
      if (v->exact && v->den != 1)
        g_string_append_printf (out, "%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT, v->num, v->den);
      else if (v->exact)
        /* Every digit of it: an exact whole number is not written in
         * powers of ten however large it is. */
        g_string_append_printf (out, "%" G_GINT64_FORMAT, v->num);
      else
        m42_number_to_string (out, v->u.number);
      break;
    case M42_VALUE_BIGINT:
      {
        g_autofree char *digits = m42_big_to_string (v->u.big);

        g_string_append (out, digits);
      }
      break;

    case M42_VALUE_COMPLEX:
      /* 3 + 4 I, -2 I, I: the way it is written, not a pair. */
      if (v->u.cx.re != 0)
        {
          m42_number_to_string (out, v->u.cx.re);
          g_string_append (out, v->u.cx.im < 0 ? " - " : " + ");
        }
      else if (v->u.cx.im < 0)
        g_string_append_c (out, '-');
      if (fabs (v->u.cx.im) != 1)
        {
          m42_number_to_string (out, fabs (v->u.cx.im));
          g_string_append_c (out, ' ');
        }
      g_string_append_c (out, 'I');
      break;

    case M42_VALUE_STRING:
      g_string_append_c (out, '"');
      g_string_append (out, v->u.string);
      g_string_append_c (out, '"');
      break;

    case M42_VALUE_LIST:
      /* A very long list is shown at both ends with a count in the
       * middle: a solver hands back hundreds of points, and nobody
       * reads them all. */
      g_string_append_c (out, '{');
      for (guint i = 0; i < v->u.list->len; i++)
        {
          if (v->u.list->len > M42_LIST_SHOWN && i == M42_LIST_SHOWN - 4)
            {
              g_string_append_printf (out, ", ... %u more ... ",
                                      v->u.list->len - M42_LIST_SHOWN);
              i = v->u.list->len - 4;
            }
          if (i > 0)
            g_string_append (out, ", ");
          value_to_string (out, g_ptr_array_index (v->u.list, i));
        }
      g_string_append_c (out, '}');
      break;
    case M42_VALUE_EXPR:
      m42_node_to_string (out, v->u.expr);
      break;
    case M42_VALUE_FUNC:
      g_string_append (out, "Function[{");
      for (guint i = 0; v->u.func.params[i] != NULL; i++)
        {
          if (i > 0)
            g_string_append (out, ", ");
          g_string_append (out, v->u.func.params[i]);
        }
      g_string_append (out, "}, ");
      m42_node_to_string (out, v->u.func.body);
      g_string_append_c (out, ']');
      break;
    case M42_VALUE_PLOT:
      g_string_append (out, "-Graphics-");
      break;
    case M42_VALUE_NULL:
      break;
    case M42_VALUE_ERROR:
      g_string_append (out, v->u.error);
      break;
    }
}

char *
m42_value_to_string (const M42Value *v)
{
  GString *out = g_string_new (NULL);
  value_to_string (out, v);
  return g_string_free (out, FALSE);
}
