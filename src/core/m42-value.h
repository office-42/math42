/* m42-value.h - what an expression evaluates to
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

#include "m42-parser.h"
#include "m42-bigint.h"

G_BEGIN_DECLS

typedef enum {
  M42_VALUE_NUMBER,   /* a real number; booleans are 1 and 0 */
  M42_VALUE_COMPLEX,  /* a + b I, with b never zero */
  M42_VALUE_BIGINT,   /* a whole number too large for a gint64 */
  M42_VALUE_LIST,     /* an ordered list of values: {1, 2, 3}; a matrix is a list of rows */
  M42_VALUE_STRING,   /* "a string" */
  M42_VALUE_EXPR,     /* a symbolic expression: 2 x + Sin[y] */
  M42_VALUE_FUNC,     /* a user-defined function */
  M42_VALUE_PLOT,     /* a graph, drawn by the notebook */
  M42_VALUE_NULL,     /* nothing to show: a statement ending in ';' */
  M42_VALUE_ERROR,    /* a message from the evaluator */
} M42ValueKind;

/* How a set of points is drawn. */
typedef enum {
  M42_SERIES_LINE,     /* joined up */
  M42_SERIES_POINTS,   /* a dot at each */
  M42_SERIES_BARS,     /* a bar from the axis; the x is the bar's middle */
  M42_SERIES_STEM,     /* a stalk from the axis with a dot on top */
  M42_SERIES_STAIRS,   /* held flat, then stepped */
} M42SeriesKind;

/* One set of points on a graph. */
typedef struct {
  GArray       *points;    /* of double, x then y */
  M42SeriesKind kind;
  double        width;     /* bars: how wide, in x */
  double        r, g, b;   /* its colour */
} M42Series;

/* A surface over a grid: z[i * ny + j] at the ith x and jth y.  Drawn
 * in projection with its mesh, or -- when flat is set, which is what
 * DensityPlot asks for -- looked down on, as a patch of colour for
 * each cell. */
typedef struct {
  guint    nx, ny;
  double  *z;
  double   xmin, xmax, ymin, ymax, zmin, zmax;
  gboolean flat;
} M42Surface;

/* One contour: the level it stands for, and the segments that draw it. */
typedef struct {
  double  level;
  GArray *segments;    /* of double: x1 y1 x2 y2, one segment after another */
  double  r, g, b;
} M42Contour;

/* A curve through space: the points it passes through, one after
 * another, and the box they live in so that it can be scaled to the
 * room it has. */
typedef struct {
  GArray *points;      /* of double: x, y, z, one point after another */
  double  r, g, b;
  double  xmin, xmax, ymin, ymax, zmin, zmax;
} M42Curve3D;

typedef struct {
  GPtrArray *series;   /* of M42Series* */
  GPtrArray *curves;   /* of M42Curve3D*, when it is a curve through space */
  /* A field of arrows: x, y, dx, dy and how long the arrow was before
   * it was cut down to fit, four and a bit doubles at a time.  What is
   * drawn is the direction; the length says how strong. */
  GArray    *arrows;
  M42Surface *surface; /* set when this is a graph of two variables */
  GPtrArray *contours; /* of M42Contour*, when it is a contour plot */
  double xmin, xmax, ymin, ymax;
  gboolean log_x, log_y;   /* the axis carries powers of ten */
  char *title, *xlabel, *ylabel;
} M42Plot;

typedef struct _M42Value M42Value;

struct _M42Value {
  M42ValueKind kind;
  int ref_count;
  /* An exact number carries the fraction it really is beside the double
   * that stands in for it, so that 1/3 stays a third and 2/4 prints as
   * 1/2.  Every integer result is exact; a decimal never is. */
  gboolean exact;
  gint64   num, den;
  union {
    double     number;
    struct { double re, im; } cx;
    M42Big    *big;     /* owned */
    GPtrArray *list;    /* of M42Value*, owned */
    char      *string;  /* owned */
    M42Node   *expr;    /* owned */
    struct {
      GStrv    params;
      M42Node *body;
    } func;
    M42Plot   *plot;
    char      *error;   /* owned */
  } u;
};

/* A number.  A value that is a whole number within reach is marked
 * exact; anything else is the double it is. */
M42Value *m42_value_number (double x);
/* An exact fraction, reduced; a zero denominator gives Infinity as a
 * plain double. */
M42Value *m42_value_rational (gint64 num, gint64 den);
/* An exact whole number, whatever its size within a gint64: the double
 * beside it may be rounded, but the number itself is not. */
M42Value *m42_value_exact_int (gint64 x);
/* An inexact number, whatever it is: what N[] returns. */
M42Value *m42_value_real (double x);
/* a + b I.  A zero imaginary part gives a plain number back, so that
 * nothing downstream has to think about complex numbers it never
 * asked for. */
M42Value *m42_value_complex (double re, double im);
/* A big whole number.  It takes the number, and hands back an ordinary
 * exact one when it turns out to fit in a gint64 after all. */
M42Value *m42_value_bigint (M42Big *big);
M42Value *m42_value_list_new (void);
M42Value *m42_value_expr (M42Node *node);              /* takes the node */
M42Value *m42_value_func (GStrv params, M42Node *body); /* takes both */
M42Value *m42_value_plot_new (void);
M42Value *m42_value_null (void);
M42Value *m42_value_error (const char *fmt, ...) G_GNUC_PRINTF (1, 2);

M42Value *m42_value_ref (M42Value *v);
void      m42_value_unref (M42Value *v);

void      m42_value_list_append (M42Value *list, M42Value *item);  /* takes the item */
guint     m42_value_list_length (const M42Value *list);
M42Value *m42_value_list_nth (const M42Value *list, guint i);       /* borrowed */

/* TRUE for a non-empty list whose items are all numbers. */
gboolean  m42_value_is_vector (const M42Value *v);
/* TRUE for a non-empty list of equally long vectors; sets rows and cols. */
gboolean  m42_value_is_matrix (const M42Value *v, guint *rows, guint *cols);

M42Value *m42_value_string (const char *text);
/* A plot that holds a surface rather than lines; the grid is filled in
 * by the caller and its range worked out by m42_surface_autoscale. */
M42Surface *m42_plot_add_surface (M42Plot *plot, guint nx, guint ny);
M42Contour *m42_plot_add_contour (M42Plot *plot, double level, guint which, guint of);
/* A curve through space; its box is worked out as points are added. */
M42Curve3D *m42_plot_add_curve3d (M42Plot *plot);
/* One arrow of a field: where it starts, which way it goes once it has
 * been cut down to fit, and how strong it was, from 0 to 1. */
void        m42_plot_add_arrow (M42Plot *plot, double x, double y,
                                double dx, double dy, double strength);
void        m42_curve3d_add_point (M42Curve3D *c, double x, double y, double z);
void        m42_contour_add_segment (M42Contour *c, double x1, double y1, double x2, double y2);
void        m42_surface_autoscale (M42Surface *s);
M42Series *m42_plot_add_series (M42Plot *plot, M42SeriesKind kind);
void       m42_series_add_point (M42Series *s, double x, double y);
/* Fits the plot's ranges to its points, with a little margin. */
void       m42_plot_autoscale (M42Plot *plot);

/* How many items of a long list are shown before it is summarised. */
#define M42_LIST_SHOWN 40

/* The value as the user reads it: 3.5, {1, 2}, 2 x + 1, or the message. */
char *m42_value_to_string (const M42Value *v);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (M42Value, m42_value_unref)

G_END_DECLS
