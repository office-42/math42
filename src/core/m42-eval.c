/* m42-eval.c - see m42-eval.h
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Numbers are doubles; lists are lists of values, and a matrix is a
 * list of equally long rows.  Arithmetic on a list maps over its
 * elements, as it does in both Mathematica and MATLAB, so {1, 2, 3} * 2
 * is {2, 4, 6}; matrix multiplication is Dot, or the . operator.
 *
 * A name with no value is a symbol, and arithmetic on a symbol builds
 * an expression instead of a number: 2 x + 1 stays 2 x + 1, D takes
 * its derivative, and /. puts numbers in.  Names are looked up in the
 * scopes of the functions being applied, then among the user's
 * variables, then the constants.
 */

#include "m42-eval.h"
#include "m42-parser.h"
#include "m42-lexer.h"
#include "m42-symbolic.h"
#include "m42-matrix.h"
#include "m42-help.h"
#include "m42-pattern.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Break[], Continue[] and Return[] have to walk back out of whatever
 * they are inside.  Every step of the evaluator already stops at an
 * error, so the way out is an error value with the session saying what
 * it really was; the loop or the function that was meant to catch it
 * clears the flag, and anything that reaches the top is reported as
 * the mistake it is. */
typedef enum {
  M42_UNWIND_NONE = 0,
  M42_UNWIND_BREAK,
  M42_UNWIND_CONTINUE,
  M42_UNWIND_RETURN,
} M42Unwind;

struct _M42Session {
  GHashTable *globals;   /* name -> M42Value* */
  M42Unwind   unwind;    /* what is walking back out, if anything */
  M42Value   *returned;  /* what Return[] was given */
  GHashTable *defined;   /* name -> GPtrArray of definitions, as RULE nodes */
  GPtrArray  *scopes;    /* of GHashTable*, innermost last */
  M42Value   *last;      /* % */
  GString    *printed;   /* what Print has written since the last line */
  int         line;
  int         depth;     /* recursion guard for user functions */
};

M42Session *
m42_session_new (void)
{
  M42Session *s = g_new0 (M42Session, 1);
  s->globals = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify) m42_value_unref);
  s->defined = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify) g_ptr_array_unref);
  s->scopes = g_ptr_array_new_with_free_func ((GDestroyNotify) g_hash_table_unref);
  s->printed = g_string_new (NULL);
  s->line = 1;
  return s;
}

void
m42_session_free (M42Session *s)
{
  if (s == NULL)
    return;
  g_hash_table_unref (s->globals);
  g_hash_table_unref (s->defined);
  g_clear_pointer (&s->returned, m42_value_unref);
  g_ptr_array_unref (s->scopes);
  g_string_free (s->printed, TRUE);
  g_clear_pointer (&s->last, m42_value_unref);
  g_free (s);
}

char *
m42_session_take_printed (M42Session *s)
{
  char *text;

  if (s->printed->len == 0)
    return NULL;
  text = g_strdup (s->printed->str);
  g_string_truncate (s->printed, 0);
  return text;
}

void
m42_session_clear (M42Session *s)
{
  g_hash_table_remove_all (s->globals);
  g_clear_pointer (&s->last, m42_value_unref);
}

int
m42_session_next_line (M42Session *s)
{
  return s->line;
}

/* --- constants and functions --------------------------------------- */

/* The named numbers.  Those written the way Mathematica writes them --
 * Pi, E, Degree -- stay as themselves until a number is actually
 * wanted, so that 2 Pi is 2 pi and prints as one; the lower-case
 * MATLAB spellings are the numbers straight away, as MATLAB has them. */
static const struct {
  const char *name;
  double      value;
  gboolean    symbolic;
} CONSTANTS[] = {
  { "Pi", G_PI, TRUE },   { "pi", G_PI, FALSE },
  { "E", G_E, TRUE },
  { "Degree", G_PI / 180.0, TRUE },
  { "GoldenRatio", 1.618033988749894848, TRUE },
  { "EulerGamma", 0.577215664901532861, TRUE },
  { "Infinity", INFINITY, FALSE }, { "Inf", INFINITY, FALSE }, { "inf", INFINITY, FALSE },
  { "True", 1, FALSE }, { "true", 1, FALSE },
  { "False", 0, FALSE }, { "false", 0, FALSE },
};

/* The number a constant stands for, or NAN if the name is not one. */
static double
constant_number (const char *name)
{
  for (guint i = 0; i < G_N_ELEMENTS (CONSTANTS); i++)
    if (strcmp (CONSTANTS[i].name, name) == 0)
      return CONSTANTS[i].value;
  return NAN;
}

static double factorial (double n) { return tgamma (n + 1.0); }
/* --- The special functions ---------------------------------------------- */

/* The ones a course reaches for after Gamma: the error function and its
 * inverse, the Bessel functions, the orthogonal polynomials and Riemann's
 * zeta.  Each is worked out from its own series or recurrence -- there is
 * no table of fitted coefficients anywhere in here. */

/* erf and erfc come from libm; their inverse does not.  Newton on erf,
 * started from Winitzki's approximation, which is good to about 1e-3
 * and lands the iteration in three steps. */
static double
m42_inverse_erf (double y)
{
  double a = 0.147, ln, first, x;

  if (y <= -1 || y >= 1)
    return y == 1 ? INFINITY : y == -1 ? -INFINITY : NAN;
  if (y == 0)
    return 0;
  ln = log (1 - y * y);
  first = 2 / (G_PI * a) + ln / 2;
  x = copysign (sqrt (sqrt (first * first - ln / a) - first), y);
  for (int i = 0; i < 4; i++)
    {
      double e = erf (x) - y;

      x -= e / (2 / sqrt (G_PI) * exp (-x * x));
    }
  return x;
}

static double
m42_inverse_erfc (double y)
{
  return m42_inverse_erf (1 - y);
}

/* The Bessel functions of the first and second kind, and the modified
 * pair, for a real order and a real argument.  Small arguments take the
 * ascending series; large ones the asymptotic form.  J and I of an
 * integer order are also reached by recurrence downwards, which is what
 * keeps them accurate in the middle. */
static double
bessel_series_j (double order, double x)
{
  /* J(v, x) = sum (-1)^k (x/2)^(2k+v) / (k! Gamma(v+k+1)) */
  double half = x / 2, term, sum = 0;
  double sign = 1;

  if (x == 0)
    return order == 0 ? 1 : 0;
  term = pow (half, order) / tgamma (order + 1);
  for (int k = 0; k < 200; k++)
    {
      sum += sign * term;
      term *= half * half / ((k + 1) * (order + k + 1));
      sign = -sign;
      if (term < 1e-18 * fabs (sum) + 1e-300)
        break;
    }
  return sum;
}

static double
bessel_series_i (double order, double x)
{
  double half = x / 2, term, sum = 0;

  if (x == 0)
    return order == 0 ? 1 : 0;
  term = pow (half, order) / tgamma (order + 1);
  for (int k = 0; k < 300; k++)
    {
      sum += term;
      term *= half * half / ((k + 1) * (order + k + 1));
      if (term < 1e-18 * sum + 1e-300)
        break;
    }
  return sum;
}

/* Y and K are built from J and I where the order is not a whole number,
 * and by the limit of that formula where it is -- taken as the average
 * of the two sides, which is what the limit comes to. */
static void bessel_pq (double order, double x, double *p, double *q);

/* Y from J, which is the definition -- except at a whole order, where
 * that is nothing over nothing.  There the two sides of the limit are
 * averaged at two step sizes and the square term taken out by
 * Richardson, which is what turns 1e-9 into 1e-12. */
static double
bessel_y_at (double order, double x)
{
  return (bessel_series_j (order, x) * cos (order * G_PI) -
          bessel_series_j (-order, x)) / sin (order * G_PI);
}

static double
bessel_y_near (double order, double x, double e)
{
  return (bessel_y_at (order + e, x) + bessel_y_at (order - e, x)) / 2;
}

static double
bessel_y (double order, double x)
{
  if (x <= 0)
    return NAN;
  if (x > 40)
    {
      double chi = x - (order / 2 + 0.25) * G_PI, p, q;

      bessel_pq (order, x, &p, &q);
      return sqrt (2 / (G_PI * x)) * (p * sin (chi) + q * cos (chi));
    }
  if (order == floor (order))
    {
      double coarse = bessel_y_near (order, x, 1e-3);
      double fine = bessel_y_near (order, x, 5e-4);

      return (4 * fine - coarse) / 3;
    }
  return bessel_y_at (order, x);
}

/* K by its integral, K(v, x) = INT[0, inf] e^(-x cosh t) cosh(v t) dt,
 * which is what keeps it from being the difference of two large numbers
 * -- the series for it loses four digits by x = 5.  The integrand falls
 * double-exponentially, so Simpson to t = 18 is the whole of it. */
static double
bessel_k (double order, double x)
{
  const double top = 18.0;
  const int steps = 20000;
  const double h = top / steps;
  double total = 0;

  if (x <= 0)
    return NAN;
  for (int i = 0; i <= steps; i++)
    {
      double t = i * h;
      double weight = (i == 0 || i == steps) ? 1 : (i % 2 ? 4 : 2);
      double e = -x * cosh (t);

      if (e > -700)
        total += weight * exp (e) * cosh (order * t);
    }
  return total * h / 3;
}

/* Past x = 40 the ascending series loses too many digits to cancellation,
 * and the asymptotic expansion takes over: J and Y share the same P and
 * Q, two terms of each being enough that far out. */
static void
bessel_pq (double order, double x, double *p, double *q)
{
  double mu = 4 * order * order;
  double x8 = 8 * x;

  *p = 1 - (mu - 1) * (mu - 9) / (2 * x8 * x8);
  *q = (mu - 1) / x8 * (1 - (mu - 9) * (mu - 25) / (6 * x8 * x8));
}

static double
bessel_j (double order, double x)
{
  if (x < 0)
    return order == floor (order)
           ? (fmod (fabs (order), 2) == 0 ? bessel_j (order, -x) : -bessel_j (order, -x))
           : NAN;
  if (x > 40)
    {
      double chi = x - (order / 2 + 0.25) * G_PI, p, q;

      bessel_pq (order, x, &p, &q);
      return sqrt (2 / (G_PI * x)) * (p * cos (chi) - q * sin (chi));
    }
  return bessel_series_j (order, x);
}

static double
bessel_i (double order, double x)
{
  if (x < 0)
    return order == floor (order)
           ? (fmod (fabs (order), 2) == 0 ? bessel_i (order, -x) : -bessel_i (order, -x))
           : NAN;
  return bessel_series_i (order, x);
}

/* The orthogonal polynomials, each by the three-term recurrence that
 * defines it -- which is exact in whole numbers for a whole argument. */
static double
orthogonal (const char *which, int n, double x)
{
  double previous = 1, current, next;

  if (n < 0)
    return NAN;
  switch (which[0])
    {
    case 'P':   /* Legendre */
      if (n == 0) return 1;
      current = x;
      for (int k = 1; k < n; k++)
        {
          next = ((2 * k + 1) * x * current - k * previous) / (k + 1);
          previous = current;
          current = next;
        }
      return current;
    case 'T':   /* Chebyshev of the first kind */
      if (n == 0) return 1;
      current = x;
      for (int k = 1; k < n; k++)
        { next = 2 * x * current - previous; previous = current; current = next; }
      return current;
    case 'U':   /* Chebyshev of the second kind */
      if (n == 0) return 1;
      current = 2 * x;
      for (int k = 1; k < n; k++)
        { next = 2 * x * current - previous; previous = current; current = next; }
      return current;
    case 'H':   /* Hermite, the physicists' */
      if (n == 0) return 1;
      current = 2 * x;
      for (int k = 1; k < n; k++)
        { next = 2 * x * current - 2 * k * previous; previous = current; current = next; }
      return current;
    case 'L':   /* Laguerre */
      if (n == 0) return 1;
      current = 1 - x;
      for (int k = 1; k < n; k++)
        {
          next = ((2 * k + 1 - x) * current - k * previous) / (k + 1);
          previous = current;
          current = next;
        }
      return current;
    default:
      return NAN;
    }
}

/* Riemann's zeta, by Borwein's alternating series: the weights d_k make
 * the error of an n-term sum fall like 8^-n, so forty terms are more
 * than a double can hold.  Below a half the functional equation
 * reflects the argument to where the series is at its best. */
static double
m42_zeta (double s)
{
  const int n = 40;
  double d[41], eta = 0;

  if (s == 1)
    return INFINITY;
  if (s == 0)
    return -0.5;
  if (s < 0.5)
    return pow (2, s) * pow (G_PI, s - 1) * sin (G_PI * s / 2) *
           tgamma (1 - s) * m42_zeta (1 - s);

  /* d_k = n sum_{i=0}^{k} (n+i-1)! 4^i / ((n-i)! (2i)!), each term from
   * the one before it. */
  for (int k = 0; k <= n; k++)
    {
      double term = 1, sum = 1;

      for (int i = 1; i <= k; i++)
        {
          term *= 4.0 * (n + i - 1) * (n - i + 1) / ((2.0 * i) * (2.0 * i - 1));
          sum += term;
        }
      d[k] = sum;
    }

  for (int k = 0; k < n; k++)
    eta += (k % 2 ? -1 : 1) * (d[k] - d[n]) / pow (k + 1, s);
  eta = -eta / d[n];
  return eta / (1 - pow (2, 1 - s));
}

static double m42_cot (double x) { return 1.0 / tan (x); }
static double m42_sec (double x) { return 1.0 / cos (x); }
static double m42_csc (double x) { return 1.0 / sin (x); }
static double m42_sign (double x) { return x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0; }
static double m42_not (double x) { return x == 0 ? 1.0 : 0.0; }

/* Functions of one number, under both spellings.  Mathematica's are
 * capitalised, MATLAB's are not; both are accepted everywhere, and the
 * canonical name is what a symbolic result prints. */
static const struct {
  const char *name;
  const char *canon;
  double (*fn) (double);
} UNARY_FUNCS[] = {
  { "Sin", "Sin", sin },        { "sin", "Sin", sin },
  { "Cos", "Cos", cos },        { "cos", "Cos", cos },
  { "Tan", "Tan", tan },        { "tan", "Tan", tan },
  { "Cot", "Cot", m42_cot },    { "cot", "Cot", m42_cot },
  { "Sec", "Sec", m42_sec },    { "sec", "Sec", m42_sec },
  { "Csc", "Csc", m42_csc },    { "csc", "Csc", m42_csc },
  { "ArcSin", "ArcSin", asin }, { "asin", "ArcSin", asin },
  { "ArcCos", "ArcCos", acos }, { "acos", "ArcCos", acos },
  { "ArcTan", "ArcTan", atan }, { "atan", "ArcTan", atan },
  { "Sinh", "Sinh", sinh },     { "sinh", "Sinh", sinh },
  { "Cosh", "Cosh", cosh },     { "cosh", "Cosh", cosh },
  { "Tanh", "Tanh", tanh },     { "tanh", "Tanh", tanh },
  { "Exp", "Exp", exp },        { "exp", "Exp", exp },
  { "Log", "Log", log },        { "log", "Log", log },
  { "Log10", "Log10", log10 },  { "log10", "Log10", log10 },
  { "Log2", "Log2", log2 },     { "log2", "Log2", log2 },
  { "Sqrt", "Sqrt", sqrt },     { "sqrt", "Sqrt", sqrt },
  { "Abs", "Abs", fabs },       { "abs", "Abs", fabs },
  { "Floor", "Floor", floor },  { "floor", "Floor", floor },
  { "Ceiling", "Ceiling", ceil },{ "ceil", "Ceiling", ceil },
  { "Round", "Round", round },  { "round", "Round", round },
  { "Sign", "Sign", m42_sign }, { "sign", "Sign", m42_sign },
  { "Factorial", "Factorial", factorial }, { "factorial", "Factorial", factorial },
  { "Gamma", "Gamma", tgamma }, { "gamma", "Gamma", tgamma },
  { "Not", "Not", m42_not },    { "not", "Not", m42_not },
  { "Erf", "Erf", erf },        { "erf", "Erf", erf },
  { "Erfc", "Erfc", erfc },     { "erfc", "Erfc", erfc },
  { "InverseErf", "InverseErf", m42_inverse_erf },
  { "erfinv", "InverseErf", m42_inverse_erf },
  { "InverseErfc", "InverseErfc", m42_inverse_erfc },
  { "erfcinv", "InverseErfc", m42_inverse_erfc },
  { "LogGamma", "LogGamma", lgamma }, { "gammaln", "LogGamma", lgamma },
  { "Zeta", "Zeta", m42_zeta }, { "zeta", "Zeta", m42_zeta },
};

/* --- helpers ----------------------------------------------------------- */

static M42Value *eval (M42Session *s, const M42Node *n);
static gboolean name_is (const char *name, const char *a, const char *b);
static gboolean collect_numbers (GPtrArray *args, GArray *out);
static M42Value *plot_matlab (GPtrArray *args);
static M42Value *call_builtin (M42Session *s, const char *name, GPtrArray *args);
static M42Value *index_value (M42Value *v, GPtrArray *indices, guint from);
static M42Value *lookup (M42Session *s, const char *name);
static M42Value *interpolation_function (M42Session *s, const M42Value *data);
static M42Value *import_file (const char *path, const char *what);
static void add_contours (M42Plot *p, const double *z, guint n, double x0, double x1,
                          double y0, double y1, double lowest, double highest,
                          guint levels);
static M42Value *export_file (const char *path, const M42Value *v, const char *what);
static M42Value *closed_form_sum (M42Session *s, const M42Node *call, const char *upper_name);
static gboolean value_matches (M42Session *s, const M42Value *v, const M42Value *pattern);
static M42Node *symbolic_argument (M42Session *s, const M42Node *raw, const char *var);
static gboolean constant_fold (const M42Node *n, double *out);
static gboolean value_number (const M42Value *v, double *out);
static gboolean value_is_constant (const M42Value *v);
static M42Node *coefficient_node (double c);
static double _Complex as_complex (const M42Value *v);
static M42Value *from_complex (double _Complex z);
static double number_at (M42Session *s, const M42Node *raw, const char *var, double x);
static gboolean polynomial_roots (GArray *coeffs, GArray *roots);
static int compare_roots (gconstpointer a, gconstpointer b);
static M42Node *value_to_node (const M42Value *v);
static gboolean is_numeric (const M42Value *v);
static double as_double (const M42Value *v);

/* One evaluated argument of the call being made. */
#define ARG(i) ((M42Value *) g_ptr_array_index (args, (i)))

static gboolean
is_error (const M42Value *v)
{
  return v->kind == M42_VALUE_ERROR;
}

static gboolean
is_num (const M42Value *v)
{
  return v->kind == M42_VALUE_NUMBER;
}

/* A number of either sort, as one C complex. */
static gboolean
is_numeric (const M42Value *v)
{
  return v->kind == M42_VALUE_NUMBER || v->kind == M42_VALUE_COMPLEX;
}

static double _Complex
as_complex (const M42Value *v)
{
  double x;

  if (v->kind == M42_VALUE_COMPLEX)
    return v->u.cx.re + v->u.cx.im * I;
  return value_number (v, &x) ? x : NAN;
}

/* An expression that is really a number in disguise: 2 Pi, Sqrt[2]. */
static gboolean
value_is_constant (const M42Value *v)
{
  double x;

  return v->kind == M42_VALUE_EXPR && value_number (v, &x);
}

/* One conversion of sprintf, done through the C library so that the
 * widths and precisions behave exactly as they do in MATLAB.  The
 * format is built here rather than written out, which is the one place
 * a non-literal format is the point rather than a mistake. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
static char *
format_one (const char *flags, gsize len, char kind, M42Value *v)
{
  g_autofree char *format = NULL;

  switch (kind)
    {
    case 'd': case 'i':
      {
        double x = as_double (v);
        format = g_strdup_printf ("%%%.*s.0f", (int) len, flags);
        return g_strdup_printf (format, isnan (x) ? 0.0 : floor (x + 0.5));
      }
    case 'f': case 'e': case 'E': case 'g': case 'G':
      {
        double x = as_double (v);
        format = g_strdup_printf ("%%%.*s%c", (int) len, flags, kind);
        return g_strdup_printf (format, isnan (x) ? 0.0 : x);
      }
    case 's':
      {
        g_autofree char *text = v->kind == M42_VALUE_STRING
          ? g_strdup (v->u.string) : m42_value_to_string (v);
        format = g_strdup_printf ("%%%.*ss", (int) len, flags);
        return g_strdup_printf (format, text);
      }
    default:
      return g_strdup_printf ("%%%c", kind);
    }
}
#pragma GCC diagnostic pop

/* An expression with no free names in it -- 2 Pi, Sqrt[2], E^2 -- as
 * the number it stands for.  It is what lets a symbolic constant be
 * used wherever a number is wanted: Plot[Sin[x], {x, 0, 2 Pi}] asks
 * this of its bounds. */
static gboolean
constant_fold (const M42Node *n, double *out)
{
  double a, b;

  switch (n->kind)
    {
    case M42_NODE_NUMBER:
      *out = n->number;
      return TRUE;

    case M42_NODE_IDENT:
      {
        double v = constant_number (n->name);
        if (isnan (v) && strcmp (n->name, "Indeterminate") != 0)
          return FALSE;
        *out = v;
        return TRUE;
      }

    case M42_NODE_UNARY:
      if (n->op != M42_TOK_MINUS || !constant_fold (m42_node_child (n, 0), &a))
        return FALSE;
      *out = -a;
      return TRUE;

    case M42_NODE_BINARY:
      if (!constant_fold (m42_node_child (n, 0), &a) ||
          !constant_fold (m42_node_child (n, 1), &b))
        return FALSE;
      switch (n->op)
        {
        case M42_TOK_PLUS:    *out = a + b; return TRUE;
        case M42_TOK_MINUS:   *out = a - b; return TRUE;
        case M42_TOK_STAR:    *out = a * b; return TRUE;
        case M42_TOK_SLASH:   *out = a / b; return TRUE;
        case M42_TOK_PERCENT: *out = fmod (a, b); return TRUE;
        case M42_TOK_CARET:   *out = pow (a, b); return TRUE;
        default: return FALSE;
        }

    case M42_NODE_CALL:
      if (n->children->len != 1 || !constant_fold (m42_node_child (n, 0), &a))
        return FALSE;
      for (guint i = 0; i < G_N_ELEMENTS (UNARY_FUNCS); i++)
        if (strcmp (UNARY_FUNCS[i].name, n->name) == 0)
          {
            *out = UNARY_FUNCS[i].fn (a);
            return TRUE;
          }
      return FALSE;

    default:
      return FALSE;
    }
}

/* The number a value is, following a constant expression to its own. */
static gboolean
value_number (const M42Value *v, double *out)
{
  if (v->kind == M42_VALUE_NUMBER)
    {
      *out = v->u.number;
      return TRUE;
    }
  if (v->kind == M42_VALUE_BIGINT)
    {
      *out = m42_big_to_double (v->u.big);
      return TRUE;
    }
  if (v->kind == M42_VALUE_EXPR)
    return constant_fold (v->u.expr, out);
  return FALSE;
}

/* The plain double behind a number, real or complex (its real part). */
static double
as_double (const M42Value *v)
{
  double x;

  if (v->kind == M42_VALUE_COMPLEX)
    return v->u.cx.re;
  return value_number (v, &x) ? x : NAN;
}

/* Rounds away the last bit of noise, so that (1 + I)^2 is exactly 2 I
 * and Sqrt[-4] is exactly 2 I. */
static M42Value *
from_complex (double _Complex z)
{
  double re = creal (z), im = cimag (z);
  double scale = MAX (fabs (re), fabs (im));

  if (scale > 0 && fabs (im) < 1e-13 * scale)
    im = 0;
  if (scale > 0 && fabs (re) < 1e-13 * scale)
    re = 0;
  if (fabs (re - round (re)) < 1e-12 * MAX (1.0, fabs (re)))
    re = round (re);
  if (fabs (im - round (im)) < 1e-12 * MAX (1.0, fabs (im)))
    im = round (im);
  return m42_value_complex (re, im);
}

static M42Value *
lookup (M42Session *s, const char *name)
{
  M42Value *v;

  for (guint i = s->scopes->len; i > 0; i--)
    {
      v = g_hash_table_lookup (g_ptr_array_index (s->scopes, i - 1), name);
      if (v != NULL)
        return v;
    }
  return g_hash_table_lookup (s->globals, name);
}

static void
push_scope (M42Session *s)
{
  g_ptr_array_add (s->scopes, g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                     (GDestroyNotify) m42_value_unref));
}

static void
bind (M42Session *s, const char *name, M42Value *v)   /* takes v */
{
  GHashTable *scope = s->scopes->len > 0
    ? g_ptr_array_index (s->scopes, s->scopes->len - 1) : s->globals;
  g_hash_table_insert (scope, g_strdup (name), v);
}

/* x = 2 inside a Module or a function changes the x that is already
 * there, wherever it was made; a name that is nowhere yet becomes a
 * global.  Without this a local would be read from the block and
 * written to the top, and Module[{t = 2}, t = t + 1; t] would answer
 * 2. */
static void
assign (M42Session *s, const char *name, M42Value *v)   /* takes v */
{
  for (guint i = s->scopes->len; i > 0; i--)
    {
      GHashTable *scope = g_ptr_array_index (s->scopes, i - 1);

      if (g_hash_table_contains (scope, name))
        {
          g_hash_table_insert (scope, g_strdup (name), v);
          return;
        }
    }
  g_hash_table_insert (s->globals, g_strdup (name), v);
}

static void
pop_scope (M42Session *s)
{
  g_ptr_array_remove_index (s->scopes, s->scopes->len - 1);
}

/* A value as a tree, for building expressions; NULL for the kinds
 * that have no written form. */
static M42Node *
value_to_node (const M42Value *v)
{
  switch (v->kind)
    {
    case M42_VALUE_NUMBER:
      /* An exact fraction goes into an expression as a fraction, so
       * that x/3 does not become 0.333333 x. */
      if (v->exact && v->den != 1)
        return m42_node_binary (M42_TOK_SLASH, m42_node_number ((double) v->num),
                                m42_node_number ((double) v->den));
      return m42_node_number (v->u.number);
    case M42_VALUE_EXPR:
      return m42_node_copy (v->u.expr);
    case M42_VALUE_STRING:
      {
        M42Node *n = m42_node_new (M42_NODE_STRING);
        n->name = g_strdup (v->u.string);
        return n;
      }
    case M42_VALUE_COMPLEX:
      {
        /* a + b I, as it is written. */
        M42Node *imaginary = m42_node_binary (M42_TOK_STAR,
                                              m42_node_number (v->u.cx.im),
                                              m42_node_ident ("I"));
        if (v->u.cx.re == 0)
          return imaginary;
        return m42_node_binary (M42_TOK_PLUS, m42_node_number (v->u.cx.re), imaginary);
      }
    case M42_VALUE_LIST:
      {
        M42Node *n = m42_node_new (M42_NODE_LIST);
        for (guint i = 0; i < v->u.list->len; i++)
          {
            M42Node *c = value_to_node (g_ptr_array_index (v->u.list, i));
            if (c == NULL)
              {
                m42_node_free (n);
                return NULL;
              }
            g_ptr_array_add (n->children, c);
          }
        return n;
      }
    default:
      return NULL;
    }
}

/* A simplified tree as a value: a number when it folded to one. */
static M42Value *
expr_result (M42Node *n)
{
  M42Node *simple = m42_node_simplify (n);

  m42_node_free (n);
  if (simple->kind == M42_NODE_NUMBER)
    {
      double x = simple->number;
      m42_node_free (simple);
      return m42_value_number (x);
    }
  if (simple->kind == M42_NODE_LIST)
    {
      /* A list of numbers is a list, not an expression. */
      gboolean all_numbers = TRUE;
      for (guint i = 0; i < simple->children->len; i++)
        if (m42_node_child (simple, i)->kind != M42_NODE_NUMBER)
          all_numbers = FALSE;
      if (all_numbers)
        {
          M42Value *out = m42_value_list_new ();
          for (guint i = 0; i < simple->children->len; i++)
            m42_value_list_append (out, m42_value_number (m42_node_child (simple, i)->number));
          m42_node_free (simple);
          return out;
        }
    }
  return m42_value_expr (simple);
}

/* The functions that mean something for a complex argument, by their
 * canonical name.  Returns NULL for one that does not. */
static M42Value *
complex_function (const char *canon, double _Complex z)
{
#define CFN(name, fn) if (strcmp (canon, name) == 0) return from_complex (fn (z))
  CFN ("Sin", csin);   CFN ("Cos", ccos);   CFN ("Tan", ctan);
  CFN ("Sinh", csinh); CFN ("Cosh", ccosh); CFN ("Tanh", ctanh);
  CFN ("Exp", cexp);   CFN ("Log", clog);   CFN ("Sqrt", csqrt);
  CFN ("ArcSin", casin); CFN ("ArcCos", cacos); CFN ("ArcTan", catan);
#undef CFN
  if (strcmp (canon, "Abs") == 0)
    return m42_value_number (cabs (z));
  if (strcmp (canon, "Arg") == 0)
    return m42_value_number (carg (z));
  if (strcmp (canon, "Re") == 0)
    return m42_value_number (creal (z));
  if (strcmp (canon, "Im") == 0)
    return m42_value_number (cimag (z));
  if (strcmp (canon, "Conjugate") == 0)
    return from_complex (conj (z));
  return NULL;
}

/* A number as an exact value where mathematics has one for it: the
 * whole numbers, the simple fractions, and the handful of radicals the
 * trigonometric functions land on at the usual angles.  Anything else
 * comes back NULL and stays as it was written, which is what
 * Mathematica does with Sin[1] or Log[Pi]. */
static M42Value *
exact_value_of (double x)
{
  static const struct { double value; int num, den, root; } NICE[] = {
    /* num/den times the square root of root */
    { 0.707106781186547524, 1, 2, 2 },     /* Sqrt[2]/2 */
    { 0.866025403784438647, 1, 2, 3 },     /* Sqrt[3]/2 */
    { 1.732050807568877294, 1, 1, 3 },     /* Sqrt[3]   */
    { 0.577350269189625764, 1, 3, 3 },     /* Sqrt[3]/3 */
    { 1.414213562373095049, 1, 1, 2 },     /* Sqrt[2]   */
  };
  double magnitude = fabs (x);
  int sign = x < 0 ? -1 : 1;

  if (!isfinite (x))
    return NULL;

  /* A whole number, or a fraction with a small denominator. */
  if (fabs (x - round (x)) < 1e-12)
    return m42_value_number (round (x));
  for (int den = 2; den <= 12; den++)
    {
      double scaled = x * den;
      if (fabs (scaled - round (scaled)) < 1e-12 * MAX (1.0, fabs (scaled)))
        return m42_value_rational ((gint64) round (scaled), den);
    }

  for (guint i = 0; i < G_N_ELEMENTS (NICE); i++)
    if (fabs (magnitude - NICE[i].value) < 1e-12)
      {
        M42Node *root = m42_node_call1 ("Sqrt", m42_node_number (NICE[i].root));
        M42Node *whole = NICE[i].num == 1 ? root
          : m42_node_binary (M42_TOK_STAR, m42_node_number (NICE[i].num), root);

        if (NICE[i].den != 1)
          whole = m42_node_binary (M42_TOK_SLASH, whole, m42_node_number (NICE[i].den));
        if (sign < 0)
          whole = m42_node_unary (M42_TOK_MINUS, whole);
        return m42_value_expr (whole);
      }
  return NULL;
}

/* Sqrt of a whole number, with whatever square factors it has taken
 * out: Sqrt[8] is 2 Sqrt[2], and Sqrt[9] is plain 3. */
static M42Value *
exact_square_root (gint64 n)
{
  gint64 outside = 1, inside = n;

  if (n < 0)
    return NULL;
  for (gint64 d = 2; d * d <= inside; d++)
    while (inside % (d * d) == 0)
      {
        outside *= d;
        inside /= d * d;
      }
  if (inside == 1)
    return m42_value_number ((double) outside);      /* a perfect square */
  {
    M42Node *root = m42_node_call1 ("Sqrt", m42_node_number ((double) inside));

    if (outside != 1)
      root = m42_node_binary (M42_TOK_STAR, m42_node_number ((double) outside), root);
    return m42_value_expr (root);
  }
}

/* Applies f element-wise: f(number), f({...}), or f[symbol].  exact
 * says whether the caller wrote the name the way Mathematica does, in
 * which case an answer that has an exact form is given in it; the
 * lower-case MATLAB spellings always come back as decimals. */
static M42Value *
map1_full (M42Value *a, const char *canon, double (*fn) (double), gboolean exact)
{
  if (exact && strcmp (canon, "Sqrt") == 0 && is_num (a) && a->exact && a->den == 1 && a->num >= 0)
    {
      M42Value *root = exact_square_root (a->num);
      if (root != NULL)
        return root;
    }

  if (is_num (a))
    {
      /* The square root and the logarithm of a negative number are
       * complex, as they are in Mathematica -- not an error. */
      if (a->u.number < 0 && (strcmp (canon, "Sqrt") == 0 || strcmp (canon, "Log") == 0))
        return complex_function (canon, a->u.number);
      if (!exact)
        return m42_value_real (fn (a->u.number));

      /* An exact number in, an exact number out where there is one:
       * Abs[-3/4] is 3/4 rather than 0.75. */
      if (a->exact)
        {
          M42Value *known = exact_value_of (fn (a->u.number));
          if (known != NULL)
            return known;
        }
      return m42_value_number (fn (a->u.number));
    }

  if (a->kind == M42_VALUE_COMPLEX)
    {
      M42Value *r = complex_function (canon, as_complex (a));
      return r != NULL ? r : m42_value_error ("%s of a complex number is not supported yet", canon);
    }
  if (a->kind == M42_VALUE_EXPR)
    {
      double folded;

      /* Sin[Pi] is 0 and Sin[Pi/3] is Sqrt[3]/2: when the argument is
       * a constant, the answer is worked out and kept if mathematics
       * has an exact name for it. */
      if (constant_fold (a->u.expr, &folded) && isfinite (folded))
        {
          M42Value *known;

          /* A MATLAB name wants the number, whatever it is. */
          if (!exact)
            return m42_value_real (fn (folded));
          known = exact_value_of (fn (folded));
          if (known != NULL)
            return known;
        }
      return expr_result (m42_node_call1 (canon, m42_node_copy (a->u.expr)));
    }
  if (a->kind != M42_VALUE_LIST)
    return m42_value_error ("%s expects a number", canon);

  {
    M42Value *out = m42_value_list_new ();
    for (guint i = 0; i < m42_value_list_length (a); i++)
      {
        M42Value *r = map1_full (m42_value_list_nth (a, i), canon, fn, exact);
        if (is_error (r))
          {
            m42_value_unref (out);
            return r;
          }
        m42_value_list_append (out, r);
      }
    return out;
  }
}

/* The old shape, for the operators: exact where an exact answer is to
 * be had, since ! and ' are written the same in both languages. */
static M42Value *
map1 (M42Value *a, const char *canon, double (*fn) (double))
{
  return map1_full (a, canon, fn, TRUE);
}

static double
apply_op (int op, double a, double b)
{
  switch (op)
    {
    case M42_TOK_PLUS:    return a + b;
    case M42_TOK_MINUS:   return a - b;
    case M42_TOK_STAR:    return a * b;
    case M42_TOK_SLASH:   return a / b;
    case M42_TOK_PERCENT: return fmod (a, b);
    case M42_TOK_CARET:   return pow (a, b);
    case M42_TOK_EQ:      return a == b;
    case M42_TOK_NE:      return a != b;
    case M42_TOK_LT:      return a < b;
    case M42_TOK_LE:      return a <= b;
    case M42_TOK_GT:      return a > b;
    case M42_TOK_GE:      return a >= b;
    case M42_TOK_AND:     return a != 0 && b != 0;
    case M42_TOK_OR:      return a != 0 || b != 0;
    default:              return NAN;
    }
}

/* Exact arithmetic on fractions, in the width of a gint64 with the
 * products taken twice as wide, so an overflow is seen rather than
 * wrapped.  Returns NULL when the answer will not fit or the operation
 * has no exact form, and the doubles take over. */
static M42Value *
exact_op (int op, const M42Value *a, const M42Value *b)
{
  __int128 num, den;

  if (!a->exact || !b->exact)
    return NULL;

  switch (op)
    {
    case M42_TOK_PLUS:
      num = (__int128) a->num * b->den + (__int128) b->num * a->den;
      den = (__int128) a->den * b->den;
      break;
    case M42_TOK_MINUS:
      num = (__int128) a->num * b->den - (__int128) b->num * a->den;
      den = (__int128) a->den * b->den;
      break;
    case M42_TOK_STAR:
      num = (__int128) a->num * b->num;
      den = (__int128) a->den * b->den;
      break;
    case M42_TOK_SLASH:
      if (b->num == 0)
        return NULL;
      num = (__int128) a->num * b->den;
      den = (__int128) a->den * b->num;
      break;
    case M42_TOK_PERCENT:
      if (a->den != 1 || b->den != 1 || b->num == 0)
        return NULL;
      num = a->num % b->num;
      den = 1;
      break;
    case M42_TOK_CARET:
      {
        /* A whole exponent, small enough that the powers stay in range. */
        gint64 k;

        if (b->den != 1 || ABS (b->num) > 64)
          return NULL;
        k = ABS (b->num);
        num = 1;
        den = 1;
        for (gint64 i = 0; i < k; i++)
          {
            num *= a->num;
            den *= a->den;
            if (num > G_MAXINT64 || num < G_MININT64 || den > G_MAXINT64)
              return NULL;
          }
        if (b->num < 0)
          {
            __int128 t = num;
            num = den;
            den = t;
          }
        break;
      }
    default:
      return NULL;
    }

  if (num > G_MAXINT64 || num < G_MININT64 || den > G_MAXINT64 || den < G_MININT64 || den == 0)
    return NULL;
  return m42_value_rational ((gint64) num, (gint64) den);
}

/* A whole number of either size, as a big one. */
static M42Big *
as_big (const M42Value *v)
{
  if (v->kind == M42_VALUE_BIGINT)
    return m42_big_copy (v->u.big);
  if (v->kind == M42_VALUE_NUMBER && v->exact && v->den == 1)
    return m42_big_from_int64 (v->num);
  return NULL;
}

/* TRUE when a value is a whole number, however large. */
static gboolean
is_whole (const M42Value *v)
{
  return v->kind == M42_VALUE_BIGINT ||
         (v->kind == M42_VALUE_NUMBER && v->exact && v->den == 1);
}

/* Arithmetic when at least one side has outgrown a gint64.  Returns
 * NULL for anything that is not whole-number work. */
static M42Value *
big_op (int op, const M42Value *a, const M42Value *b)
{
  g_autoptr (M42Big) x = NULL;
  g_autoptr (M42Big) y = NULL;

  if (!is_whole (a) || !is_whole (b))
    return NULL;
  x = as_big (a);
  y = as_big (b);
  if (x == NULL || y == NULL)
    return NULL;

  switch (op)
    {
    case M42_TOK_PLUS:  return m42_value_bigint (m42_big_add (x, y));
    case M42_TOK_MINUS: return m42_value_bigint (m42_big_subtract (x, y));
    case M42_TOK_STAR:  return m42_value_bigint (m42_big_multiply (x, y));
    case M42_TOK_CARET:
      {
        gint64 e;

        if (!m42_big_fits_int64 (y, &e) || e < 0 || e > 1000000)
          return NULL;
        return m42_value_bigint (m42_big_power (x, (guint64) e));
      }
    case M42_TOK_SLASH:
    case M42_TOK_PERCENT:
      {
        gint64 divisor, remainder;
        g_autoptr (M42Big) quotient = NULL;

        if (!m42_big_fits_int64 (y, &divisor) || divisor == 0)
          return NULL;
        quotient = m42_big_divide_small (x, divisor, &remainder);
        if (op == M42_TOK_PERCENT)
          return m42_value_number ((double) remainder);
        if (remainder != 0)
          return NULL;              /* not a whole answer: the doubles take it */
        return m42_value_bigint (m42_big_copy (quotient));
      }
    case M42_TOK_EQ:  return m42_value_number (m42_big_compare (x, y) == 0);
    case M42_TOK_NE:  return m42_value_number (m42_big_compare (x, y) != 0);
    case M42_TOK_LT:  return m42_value_number (m42_big_compare (x, y) < 0);
    case M42_TOK_LE:  return m42_value_number (m42_big_compare (x, y) <= 0);
    case M42_TOK_GT:  return m42_value_number (m42_big_compare (x, y) > 0);
    case M42_TOK_GE:  return m42_value_number (m42_big_compare (x, y) >= 0);
    default:          return NULL;
    }
}

/* Applies op element-wise, broadcasting a number over a list, and
 * building an expression when a symbol is involved. */
static M42Value *
map2 (int op, M42Value *a, M42Value *b)
{
  if (a->kind == M42_VALUE_BIGINT || b->kind == M42_VALUE_BIGINT)
    {
      M42Value *big = big_op (op, a, b);

      if (big != NULL)
        return big;
      /* Not whole-number work: the doubles finish it, as far as they
       * can reach. */
      if (is_numeric (a) || a->kind == M42_VALUE_BIGINT)
        {
          double x = a->kind == M42_VALUE_BIGINT ? m42_big_to_double (a->u.big)
                                                 : as_double (a);
          double y = b->kind == M42_VALUE_BIGINT ? m42_big_to_double (b->u.big)
                                                 : as_double (b);

          if (isfinite (x) && isfinite (y))
            return m42_value_number (apply_op (op, x, y));
        }
    }

  if (is_num (a) && is_num (b))
    {
      M42Value *exact = exact_op (op, a, b);
      if (exact != NULL)
        return exact;

      /* Exact whole numbers that outgrow a gint64 go on exactly, in
       * the big arithmetic, rather than falling back to a double. */
      if (is_whole (a) && is_whole (b))
        {
          M42Value *big = big_op (op, a, b);

          if (big != NULL)
            return big;
        }
      return m42_value_number (apply_op (op, a->u.number, b->u.number));
    }

  /* Complex numbers, which C knows how to add and multiply.  A
   * constant on the other side -- Exp[I Pi] -- is worked out first. */
  if ((a->kind == M42_VALUE_COMPLEX || b->kind == M42_VALUE_COMPLEX) &&
      (is_numeric (a) || value_is_constant (a)) &&
      (is_numeric (b) || value_is_constant (b)))
    {
      double _Complex x = as_complex (a), y = as_complex (b);

      switch (op)
        {
        case M42_TOK_PLUS:  return from_complex (x + y);
        case M42_TOK_MINUS: return from_complex (x - y);
        case M42_TOK_STAR:  return from_complex (x * y);
        case M42_TOK_SLASH: return from_complex (x / y);
        case M42_TOK_CARET: return from_complex (cpow (x, y));
        case M42_TOK_EQ:    return m42_value_number (x == y);
        case M42_TOK_NE:    return m42_value_number (x != y);
        default:
          return m42_value_error ("A complex number cannot be compared that way");
        }
    }

  if (a->kind == M42_VALUE_LIST || b->kind == M42_VALUE_LIST)
    {
      M42Value *out;
      guint n;

      if (a->kind == M42_VALUE_LIST && b->kind == M42_VALUE_LIST &&
          m42_value_list_length (a) != m42_value_list_length (b))
        return m42_value_error ("Lists of lengths %u and %u cannot be combined",
                                m42_value_list_length (a), m42_value_list_length (b));
      n = a->kind == M42_VALUE_LIST ? m42_value_list_length (a) : m42_value_list_length (b);
      out = m42_value_list_new ();
      for (guint i = 0; i < n; i++)
        {
          M42Value *ai = a->kind == M42_VALUE_LIST ? m42_value_list_nth (a, i) : a;
          M42Value *bi = b->kind == M42_VALUE_LIST ? m42_value_list_nth (b, i) : b;
          M42Value *r = map2 (op, ai, bi);

          if (is_error (r))
            {
              m42_value_unref (out);
              return r;
            }
          m42_value_list_append (out, r);
        }
      return out;
    }

  if ((a->kind == M42_VALUE_EXPR || b->kind == M42_VALUE_EXPR) &&
      (a->kind == M42_VALUE_EXPR || is_numeric (a)) &&
      (b->kind == M42_VALUE_EXPR || is_numeric (b)))
    {
      /* x == 2 is an equation, not a question -- as it is in
       * Mathematica, where SameQ is the one that answers.  Anything
       * that cannot be worked out is kept as it was written. */
      return expr_result (m42_node_binary (op, value_to_node (a), value_to_node (b)));
    }

  if (is_error (a)) return m42_value_ref (a);
  if (is_error (b)) return m42_value_ref (b);
  return m42_value_error ("Cannot combine these values");
}

static M42Value *
negate (M42Value *a)
{
  g_autoptr (M42Value) zero = m42_value_number (0);
  if (a->kind == M42_VALUE_EXPR)
    return expr_result (m42_node_unary (M42_TOK_MINUS, m42_node_copy (a->u.expr)));
  return map2 (M42_TOK_MINUS, zero, a);
}

/* A value that must be a number, else an error. */
static gboolean
need_number (M42Value *v, const char *who, double *out, M42Value **err)
{
  if (!value_number (v, out))
    {
      *err = is_error (v) ? m42_value_ref (v) : m42_value_error ("%s expects a number", who);
      return FALSE;
    }
  return TRUE;
}

/* --- user functions ----------------------------------------------------- */

static M42Value *
apply_function (M42Session *s, M42Value *f, GPtrArray *args)
{
  guint n = g_strv_length (f->u.func.params);
  M42Value *r;

  if (args->len != n)
    return m42_value_error ("Function of %u argument%s called with %u", n, n == 1 ? "" : "s", args->len);
  if (s->depth > 200)
    return m42_value_error ("Recursion too deep");

  push_scope (s);
  for (guint i = 0; i < n; i++)
    bind (s, f->u.func.params[i], m42_value_ref (g_ptr_array_index (args, i)));
  s->depth++;
  r = eval (s, f->u.func.body);
  s->depth--;
  pop_scope (s);

  /* A Return[] on its way out stops here, and hands over what it was
   * given. */
  if (s->unwind == M42_UNWIND_RETURN)
    {
      s->unwind = M42_UNWIND_NONE;
      m42_value_unref (r);
      r = s->returned != NULL ? g_steal_pointer (&s->returned) : m42_value_null ();
    }
  return r;
}

/* Anything that can be called: a function value, or the name of one --
 * so that Map[Sqrt, list] and Sqrt /@ list work as well as a pure
 * function does. */
static M42Value *
apply_callable (M42Session *s, M42Value *f, GPtrArray *args)
{
  if (f->kind == M42_VALUE_FUNC)
    return apply_function (s, f, args);

  if (f->kind == M42_VALUE_EXPR && f->u.expr->kind == M42_NODE_IDENT)
    {
      const char *name = f->u.expr->name;
      M42Value *bound = lookup (s, name);

      if (bound != NULL && bound->kind == M42_VALUE_FUNC)
        return apply_function (s, bound, args);
      return call_builtin (s, name, args);
    }

  if (f->kind == M42_VALUE_LIST)
    return index_value (f, args, 0);

  return m42_value_error ("That is not something that can be applied");
}

/* Evaluates raw with var bound to x, following a variable that holds
 * an expression or a function: what Plot, NIntegrate and FindRoot do
 * at every sample. */
static M42Value *
eval_at (M42Session *s, const M42Node *raw, const char *var, double x)
{
  M42Value *v, *r;

  push_scope (s);
  bind (s, var, m42_value_number (x));
  v = eval (s, raw);
  if (v->kind == M42_VALUE_EXPR)
    {
      r = eval (s, v->u.expr);
      m42_value_unref (v);
      v = r;
    }
  else if (v->kind == M42_VALUE_FUNC)
    {
      GPtrArray *args = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
      g_ptr_array_add (args, m42_value_number (x));
      r = apply_function (s, v, args);
      g_ptr_array_unref (args);
      m42_value_unref (v);
      v = r;
    }
  pop_scope (s);
  return v;
}

static double
number_at (M42Session *s, const M42Node *raw, const char *var, double x)
{
  g_autoptr (M42Value) v = eval_at (s, raw, var, x);
  double r;

  return value_number (v, &r) ? r : NAN;
}

/* --- iterators: {i, n}, {i, a, b}, {i, a, b, step}, {i, list} ---------- */

typedef struct {
  const char *var;
  M42Value   *values;    /* the list to walk, owned */
} Iterator;

static M42Value *
iterator_parse (M42Session *s, const M42Node *spec, Iterator *it, const char *who)
{
  double a = 1, b, step = 1;
  M42Value *out;

  /* {5} on its own is five turns with no name to count them by, which
   * is how Do[something, {5}] is written. */
  if (spec->kind == M42_NODE_LIST && spec->children->len == 1)
    {
      g_autoptr (M42Value) v = eval (s, m42_node_child (spec, 0));
      double turns;

      if (!value_number (v, &turns) || turns < 0 || turns > 1e7)
        return is_error (v) ? m42_value_ref (v)
                            : m42_value_error ("%s: {n} means n turns", who);
      it->var = "$turn";
      out = m42_value_list_new ();
      for (int i = 1; i <= (int) turns; i++)
        m42_value_list_append (out, m42_value_number (i));
      it->values = out;
      return NULL;
    }

  if (spec->kind != M42_NODE_LIST || spec->children->len < 2 || spec->children->len > 4 ||
      m42_node_child (spec, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("%s expects an iterator like {i, 1, 10}", who);
  it->var = m42_node_child (spec, 0)->name;

  if (spec->children->len == 2)
    {
      g_autoptr (M42Value) v = eval (s, m42_node_child (spec, 1));
      if (v->kind == M42_VALUE_LIST)
        {
          it->values = m42_value_ref (v);
          return NULL;
        }
      if (!is_num (v))
        return is_error (v) ? m42_value_ref (v) : m42_value_error ("%s: bad iterator bound", who);
      b = v->u.number;
    }
  else
    {
      g_autoptr (M42Value) va = eval (s, m42_node_child (spec, 1));
      g_autoptr (M42Value) vb = eval (s, m42_node_child (spec, 2));
      M42Value *err;
      if (!need_number (va, who, &a, &err) || !need_number (vb, who, &b, &err))
        return err;
      if (spec->children->len == 4)
        {
          g_autoptr (M42Value) vs = eval (s, m42_node_child (spec, 3));
          if (!need_number (vs, who, &step, &err))
            return err;
        }
    }
  if (step == 0 || (b - a) / step > 1e6)
    return m42_value_error ("%s: iterator too long", who);

  out = m42_value_list_new ();
  for (double x = a; step > 0 ? x <= b + 1e-9 * fabs (step) : x >= b - 1e-9 * fabs (step); x += step)
    m42_value_list_append (out, m42_value_number (x));
  it->values = out;
  return NULL;
}

/* Table[expr, it1, it2...]: nested lists, one level per iterator. */
static M42Value *
table (M42Session *s, const M42Node *call, guint which)
{
  Iterator it = { 0 };
  M42Value *err = iterator_parse (s, m42_node_child (call, which), &it, "Table");
  M42Value *out;

  if (err != NULL)
    return err;
  out = m42_value_list_new ();
  push_scope (s);
  for (guint i = 0; i < m42_value_list_length (it.values); i++)
    {
      M42Value *r;
      bind (s, it.var, m42_value_ref (m42_value_list_nth (it.values, i)));
      r = which + 1 < call->children->len ? table (s, call, which + 1)
                                          : eval (s, m42_node_child (call, 0));
      if (is_error (r))
        {
          m42_value_unref (out);
          out = r;
          break;
        }
      m42_value_list_append (out, r);
    }
  pop_scope (s);
  m42_value_unref (it.values);
  return out;
}

/* --- sums that run for ever -----------------------------------------------
 *
 * A geometric series has the closed form everyone knows; 1/n^2, 1/n^4
 * and 1/n^6 have the ones Euler found; and anything else whose terms
 * die away fast enough is added up until the tail no longer tells.
 * Anything that does not settle is left as it was written, which is
 * the honest answer for a sum that does not converge.
 */

/* The term at k, worked out with the index bound to it. */
static M42Value *
term_at (M42Session *s, const M42Node *body, const char *var, double k)
{
  M42Value *v;

  push_scope (s);
  bind (s, var, m42_value_number (k));
  v = eval (s, body);
  pop_scope (s);
  return v;
}

static M42Value *
infinite_sum (M42Session *s, const M42Node *call, const char *var, double from)
{
  const M42Node *body = m42_node_child (call, 0);
  g_autoptr (M42Value) first = term_at (s, body, var, from);
  double f0, f1, f2, ratio;

  if (is_error (first) || !value_number (first, &f0))
    return NULL;

  {
    g_autoptr (M42Value) second = term_at (s, body, var, from + 1);
    g_autoptr (M42Value) third = term_at (s, body, var, from + 2);

    if (is_error (second) || is_error (third) ||
        !value_number (second, &f1) || !value_number (third, &f2))
      return NULL;

    /* The same ratio twice over, and less than one: a geometric
     * series, whose sum is its first term over one less the ratio.
     * Worked out as values, so 1/2^n comes out as the exact 2. */
    if (f0 != 0 && f1 != 0 && fabs (f1 / f0) < 1 - 1e-12 &&
        fabs (f2 / f1 - f1 / f0) < 1e-9 * MAX (1.0, fabs (f1 / f0)))
      {
        g_autoptr (M42Value) r = map2 (M42_TOK_SLASH, second, first);
        g_autoptr (M42Value) one = m42_value_number (1);
        g_autoptr (M42Value) rest = map2 (M42_TOK_MINUS, one, r);

        if (!is_error (rest))
          return map2 (M42_TOK_SLASH, first, rest);
      }
    ratio = f0 != 0 ? f1 / f0 : 0;
  }

  /* The three sums of reciprocal powers with a closed form. */
  if (from == 1)
    {
      static const struct { int power; int over; } EULER[] = {
        { 2, 6 }, { 4, 90 }, { 6, 945 },
      };

      for (guint i = 0; i < G_N_ELEMENTS (EULER); i++)
        {
          double want0 = 1, want1 = 1 / pow (2, EULER[i].power);
          double want2 = 1 / pow (3, EULER[i].power);

          if (fabs (f0 - want0) < 1e-12 && fabs (f1 - want1) < 1e-12 &&
              fabs (f2 - want2) < 1e-12)
            {
              M42Node *closed =
                m42_node_binary (M42_TOK_SLASH,
                                 m42_node_binary (M42_TOK_CARET, m42_node_ident ("Pi"),
                                                  m42_node_number (EULER[i].power)),
                                 m42_node_number (EULER[i].over));

              return expr_result (closed);
            }
        }
    }

  /* Otherwise: add it up, as long as the terms die away fast enough
   * for the tail to be past telling.  A series that only creeps
   * towards its sum -- 1/(n^2 + 1) and its like -- is left alone
   * rather than answered badly, and one that does not converge at all
   * is left alone for the same reason.  1/n! settles in twenty terms
   * although its first two are the same size, which is why the ratio
   * at the front is not what decides. */
  (void) ratio;
  {
    double total = 0;
    gboolean settled = FALSE;

    for (int k = 0; k < 20000; k++)
      {
        g_autoptr (M42Value) term = term_at (s, body, var, from + k);
        double x;

        if (is_error (term) || !value_number (term, &x) || !isfinite (x))
          return NULL;
        total += x;
        if (fabs (x) <= 1e-17 * MAX (1.0, fabs (total)))
          {
            settled = TRUE;
            break;
          }
        /* Growing rather than dying: there is nothing to add up. */
        if (k > 4 && fabs (x) > 1e6 * MAX (1.0, fabs (f0)))
          return NULL;
      }
    if (!settled)
      return NULL;
    return m42_value_real (total);
  }
}

static M42Value *
sum_or_product (M42Session *s, const M42Node *call, gboolean product)
{
  const char *who = product ? "Product" : "Sum";
  Iterator it = { 0 };
  M42Value *err, *acc;

  if (call->children->len != 2)
    return m42_value_error ("%s expects an expression and an iterator", who);
  err = iterator_parse (s, m42_node_child (call, 1), &it, who);
  if (err != NULL)
    {
      /* Bounds that are not numbers -- Sum[1/i^2, {i, 1, n}] -- leave
       * the sum as it was written, for the page to set with a sign. */
      const M42Node *spec = m42_node_child (call, 1);
      M42Node *unevaluated;

      if (spec->kind != M42_NODE_LIST || spec->children->len < 2)
        return err;

      /* A sum with no end to it. */
      if (!product && spec->children->len == 3 &&
          m42_node_child (spec, 0)->kind == M42_NODE_IDENT)
        {
          g_autoptr (M42Value) top = eval (s, m42_node_child (spec, 2));
          g_autoptr (M42Value) bottom = eval (s, m42_node_child (spec, 1));
          double high, low;

          if (value_number (top, &high) && isinf (high) && high > 0 &&
              value_number (bottom, &low) && low == floor (low))
            {
              M42Value *added = infinite_sum (s, call, m42_node_child (spec, 0)->name, low);

              if (added != NULL)
                {
                  m42_value_unref (err);
                  return added;
                }
            }
        }

      /* Sum[i, {i, 1, n}] has a closed form: the sum of a polynomial
       * is a polynomial one degree higher. */
      if (!product && spec->children->len == 3 &&
          m42_node_child (spec, 2)->kind == M42_NODE_IDENT &&
          strcmp (m42_node_child (spec, 2)->name, "Infinity") != 0)
        {
          M42Value *closed = closed_form_sum (s, call, m42_node_child (spec, 2)->name);

          if (closed != NULL)
            {
              m42_value_unref (err);
              return closed;
            }
        }
      m42_value_unref (err);

      unevaluated = m42_node_new (M42_NODE_CALL);
      unevaluated->name = g_strdup (who);
      g_ptr_array_add (unevaluated->children,
                       symbolic_argument (s, m42_node_child (call, 0),
                                          m42_node_child (spec, 0)->kind == M42_NODE_IDENT
                                          ? m42_node_child (spec, 0)->name : "i"));
      g_ptr_array_add (unevaluated->children, m42_node_copy (spec));
      return m42_value_expr (unevaluated);
    }

  acc = m42_value_number (product ? 1 : 0);
  push_scope (s);
  for (guint i = 0; i < m42_value_list_length (it.values); i++)
    {
      g_autoptr (M42Value) term = NULL;
      M42Value *next;
      bind (s, it.var, m42_value_ref (m42_value_list_nth (it.values, i)));
      term = eval (s, m42_node_child (call, 0));
      next = map2 (product ? M42_TOK_STAR : M42_TOK_PLUS, acc, term);
      m42_value_unref (acc);
      acc = next;
      if (is_error (acc))
        break;
    }
  pop_scope (s);
  m42_value_unref (it.values);
  return acc;
}

/* --- calculus ------------------------------------------------------------ */

static M42Value *
differentiate (M42Session *s, const M42Node *call)
{
  g_autoptr (M42Value) f = NULL;
  const M42Node *spec;
  const char *var;
  int order = 1;
  M42Node *tree, *d;

  if (call->children->len < 2)
    return m42_value_error ("D expects an expression and a variable");

  /* D[f, x, y] is the mixed partial: differentiate by the first, then
   * hand the answer back to D for the rest. */
  if (call->children->len > 2)
    {
      g_autoptr (M42Value) once = NULL;
      g_autoptr (M42Node) rest = NULL;
      M42Node *first = m42_node_new (M42_NODE_CALL);

      first->name = g_strdup ("D");
      g_ptr_array_add (first->children, m42_node_copy (m42_node_child (call, 0)));
      g_ptr_array_add (first->children, m42_node_copy (m42_node_child (call, 1)));
      once = differentiate (s, first);
      m42_node_free (first);
      if (is_error (once))
        return g_steal_pointer (&once);

      rest = m42_node_new (M42_NODE_CALL);
      rest->name = g_strdup ("D");
      {
        M42Node *had = value_to_node (once);

        if (had == NULL)
          return m42_value_error ("D expects an expression");
        g_ptr_array_add (rest->children, had);
      }
      for (guint i = 2; i < call->children->len; i++)
        g_ptr_array_add (rest->children, m42_node_copy (m42_node_child (call, i)));
      return differentiate (s, rest);
    }

  spec = m42_node_child (call, 1);
  if (spec->kind == M42_NODE_LIST && spec->children->len == 2 &&
      m42_node_child (spec, 0)->kind == M42_NODE_IDENT &&
      m42_node_child (spec, 1)->kind == M42_NODE_NUMBER)
    {
      var = m42_node_child (spec, 0)->name;
      order = (int) m42_node_child (spec, 1)->number;
    }
  else if (spec->kind == M42_NODE_IDENT)
    var = spec->name;
  else
    return m42_value_error ("D expects a variable as its second argument");

  f = eval (s, m42_node_child (call, 0));
  if (is_error (f))
    return g_steal_pointer (&f);
  tree = value_to_node (f);
  if (tree == NULL)
    return m42_value_error ("D expects an expression");

  for (int i = 0; i < order; i++)
    {
      d = m42_node_differentiate (tree, var);
      if (d == NULL)
        {
          /* Left as it was written, for the notebook to draw. */
          M42Node *unevaluated = m42_node_new (M42_NODE_CALL);

          unevaluated->name = g_strdup ("D");
          g_ptr_array_add (unevaluated->children, tree);
          g_ptr_array_add (unevaluated->children,
                           order == 1 ? m42_node_ident (var)
                                      : m42_node_copy (m42_node_child (call, 1)));
          return m42_value_expr (unevaluated);
        }
      m42_node_free (tree);
      tree = m42_node_simplify (d);
      m42_node_free (d);
    }
  return expr_result (tree);
}

/* Composite Simpson: what NIntegrate always does, and what Integrate
 * falls back on when it has no antiderivative.  It hands back the sum
 * on n panels, and through coarse the sum on n/2 of them, which is
 * what says whether n was enough. */
static double
simpson_over (M42Session *s, const M42Node *f, const char *var, double a, double b,
              int n, double *coarse)
{
  double h = (b - a) / n;
  double first = number_at (s, f, var, a), last = number_at (s, f, var, b);
  double fine = first + last;
  double half = first + last;

  /* The same samples give the answer on n panels and on n/2 of them:
   * every other node is a node of both.  Two answers that agree mean
   * the panels were fine enough, and two that do not mean there is
   * something in the function narrower than a panel. */
  for (int i = 1; i < n; i++)
    {
      double y = number_at (s, f, var, a + i * h);

      fine += (i % 2 == 1 ? 4 : 2) * y;
      if (i % 2 == 0)
        half += ((i / 2) % 2 == 1 ? 4 : 2) * y;
    }
  if (coarse != NULL)
    *coarse = half * (2 * h) / 3;
  return fine * h / 3;
}

/* Where the two disagree, the range is halved and each piece done
 * again, until they agree or the halving has gone deep enough. */
static double
simpson_refined (M42Session *s, const M42Node *f, const char *var, double a, double b,
                 int depth)
{
  double coarse, fine = simpson_over (s, f, var, a, b, 200, &coarse);
  double middle;

  /* Deep enough for a function that wanders -- Sin[1/x] near nothing
   * swings faster the closer it gets -- and strict enough that two
   * sums agreeing by luck do not end the halving too soon. */
  if (depth >= 12 || !isfinite (fine) ||
      fabs (fine - coarse) <= 1e-14 * MAX (1.0, fabs (fine)))
    return fine;
  middle = (a + b) / 2;
  return simpson_refined (s, f, var, a, middle, depth + 1) +
         simpson_refined (s, f, var, middle, b, depth + 1);
}

static M42Value *
simpson (M42Session *s, const M42Node *f, const char *var, double a, double b)
{
  double coarse, fine = simpson_over (s, f, var, a, b, 2000, &coarse);

  /* Two thousand panels are enough for almost everything, and the
   * answer they give is the one math42 has always given.  When the
   * coarser sum disagrees -- a spike a thousandth wide, which the
   * panels walked past -- the range is done again in pieces. */
  if (isfinite (fine) && fabs (fine - coarse) > 1e-11 * MAX (1.0, fabs (fine)))
    {
      double better = simpson_refined (s, f, var, a, b, 0);

      if (isfinite (better))
        return m42_value_number (better);
    }
  return m42_value_number (fine);
}

/* An antiderivative at one end of the range.  At infinity there is
 * nothing to put in, so it is asked further and further out instead
 * and taken when it settles: Exp[-x] (Sin[x] - Cos[x])/2 is nothing at
 * all out there, which is what makes the integral of Exp[-x] Sin[x]
 * from nothing to infinity come out as exactly a half. */
static gboolean
antiderivative_at (M42Session *s, const M42Node *anti, const char *var,
                   double end, double *out)
{
  double prev = NAN;

  if (isfinite (end))
    {
      *out = number_at (s, anti, var, end);
      return isfinite (*out);
    }
  for (double t = 1e3; t <= 1e7; t *= 10)
    {
      double x = number_at (s, anti, var, end > 0 ? t : -t);

      if (!isfinite (x))
        return FALSE;
      if (isfinite (prev) && fabs (x - prev) < 1e-9 * MAX (1.0, fabs (x)))
        {
          /* A value this small, still falling, is on its way to
           * nothing and is worth more as nothing: it is what leaves
           * the integral of 1/x^3 from one to infinity at a half
           * rather than a half less a whisker. */
          *out = fabs (x) < 1e-9 ? 0 : x;
          return TRUE;
        }
      prev = x;
    }
  return FALSE;
}

/* An integral with an end at infinity, pulled back into a finite one.
 *
 * With x = a + t/(1 - t) the far end comes in to t = 1, and dx is
 * dt/(1 - t)^2; the whole line is done as two halves, one each way
 * from zero.  The integrand is asked for at t just short of 1 rather
 * than at 1 itself, where the substitution would divide by nothing;
 * for anything that dies away as it must to be integrable at all, the
 * piece left out is smaller than the arithmetic can see.
 */
static M42Value *
simpson_to_infinity (M42Session *s, const M42Node *f, const char *var,
                     double from, int direction)
{
  const int n = 2000;
  const double edge = 1 - 1e-9;
  double h = edge / n;
  double total = 0;
  double near_max = 0, far_max = 0;

  for (int i = 0; i <= n; i++)
    {
      double t = i * h;
      double x = from + direction * t / (1 - t);
      double weight = 1 / ((1 - t) * (1 - t));
      double y = number_at (s, f, var, x) * weight;
      int rule = (i == 0 || i == n) ? 1 : (i % 2 == 1 ? 4 : 2);

      if (!isfinite (y))
        y = 0;               /* past where the function has anything left */
      total += rule * y;
      if (i <= n / 2)
        near_max = MAX (near_max, fabs (y));
      else if (i > n - n / 100)
        far_max = MAX (far_max, fabs (y));
    }

  /* The substitution only works for a function that dies away.  When
   * what it hands Simpson is larger at the far end than anywhere in
   * the first half -- Sin[x]/x, whose tail oscillates for ever and
   * whose weight grows -- the answer would be nonsense, and nonsense
   * is worse than no answer.  So there is none. */
  if (far_max > 1e3 * MAX (near_max, 1e-300))
    return NULL;
  return m42_value_number (direction * total * h / 3);
}

/* An integral whose function has no value at one of the ends.
 *
 * 1/Sqrt[x] from nothing to one is 2, but the first thing Simpson asks
 * for is the value at nothing, and there is none.  The double
 * exponential rule -- tanh-sinh -- puts
 *
 *   x = (a + b)/2 + (b - a)/2 tanh(pi/2 sinh t)
 *
 * which crowds its points towards the ends while its weights die away
 * there twice as fast, so an end the function cannot reach is never
 * asked about and contributes nothing.  It is the standard answer to
 * this and costs twenty lines.
 */
static M42Value *
tanh_sinh (M42Session *s, const M42Node *f, const char *var, double a, double b)
{
  const double half = (b - a) / 2, middle = (a + b) / 2;
  /* Far enough out that the weights have underflowed: a function that
   * climbs at the end is still being multiplied by something there,
   * and stopping at three would leave a part in ten million of it
   * behind. */
  const double limit = 4.2;
  double total = 0;
  int steps = 1000;
  double h = limit / steps;
  int used = 0;

  /* The two halves are walked together from the middle outwards, and
   * each point is given as its distance from the end rather than as
   * middle + half tanh(u): near the end those two are the same to
   * fifteen figures, and the subtraction would throw away every digit
   * that says how close to it we are -- which is the only thing that
   * matters when the function is climbing there. */
  for (int i = 0; i <= steps; i++)
    {
      double t = i * h;
      double u = G_PI_2 * sinh (t);
      double weight = half * G_PI_2 * cosh (t) / (cosh (u) * cosh (u));
      double away = half * 2 / (exp (2 * u) + 1);   /* half (1 - tanh u) */
      double y;

      if (weight == 0 || !isfinite (weight))
        continue;
      if (i == 0)
        {
          y = number_at (s, f, var, middle);
          if (isfinite (y))
            {
              total += weight * y;
              used++;
            }
          continue;
        }
      y = number_at (s, f, var, a + away);
      if (isfinite (y))
        {
          total += weight * y;       /* the end it cannot reach adds nothing */
          used++;
        }
      y = number_at (s, f, var, b - away);
      if (isfinite (y))
        {
          total += weight * y;
          used++;
        }
    }
  total *= h;
  /* Almost nothing came back as a number: there is no integral here to
   * find, and answering with the nothing that was added up would be a
   * lie. */
  if (used < steps || !isfinite (total))
    return NULL;
  /* An answer this close to a simple number is that number. */
  if (fabs (total - round (total)) < 1e-9 * MAX (1.0, fabs (total)))
    total = round (total);
  return m42_value_number (total);
}

/* The expression a call was given, with the variables that hold values
 * already put in, so that the symbolic rules see 2 x and not 2 y x. */
static M42Node *
symbolic_argument (M42Session *s, const M42Node *raw, const char *var)
{
  g_autoptr (M42Value) v = NULL;
  M42Node *tree;

  push_scope (s);
  bind (s, var, m42_value_expr (m42_node_ident (var)));
  v = eval (s, raw);
  pop_scope (s);

  tree = value_to_node (v);
  return tree != NULL ? tree : m42_node_copy (raw);
}

/* Integrate[f, x] is the antiderivative; Integrate[f, {x, a, b}] is
 * F(b) - F(a) when there is one, and Simpson when there is not.
 * NIntegrate is always Simpson. */
static M42Value *
integrate (M42Session *s, const M42Node *call, gboolean numeric_only)
{
  const M42Node *spec;
  const char *var;
  double a, b;
  M42Value *err;
  g_autoptr (M42Node) integrand = NULL;
  g_autoptr (M42Node) anti = NULL;

  if (call->children->len != 2)
    return m42_value_error ("%s expects an expression and a variable or {x, a, b}",
                            numeric_only ? "NIntegrate" : "Integrate");
  spec = m42_node_child (call, 1);

  /* The indefinite form. */
  if (spec->kind == M42_NODE_IDENT && !numeric_only)
    {
      var = spec->name;
      integrand = symbolic_argument (s, m42_node_child (call, 0), var);
      anti = m42_node_integrate (integrand, var);
      if (anti == NULL)
        {
          /* No rule fits: the integral stands as itself, which is what
           * Mathematica does and what the notebook draws with a sign. */
          M42Node *unevaluated = m42_node_new (M42_NODE_CALL);

          unevaluated->name = g_strdup ("Integrate");
          g_ptr_array_add (unevaluated->children, m42_node_copy (integrand));
          g_ptr_array_add (unevaluated->children, m42_node_ident (var));
          return m42_value_expr (unevaluated);
        }
      return expr_result (g_steal_pointer (&anti));
    }

  if (spec->kind != M42_NODE_LIST || spec->children->len != 3 ||
      m42_node_child (spec, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("Integrate expects a variable, or {x, a, b} between bounds");
  var = m42_node_child (spec, 0)->name;
  {
    g_autoptr (M42Value) va = eval (s, m42_node_child (spec, 1));
    g_autoptr (M42Value) vb = eval (s, m42_node_child (spec, 2));

    if (!need_number (va, "Integrate", &a, &err) || !need_number (vb, "Integrate", &b, &err))
      {
        /* Bounds that are not numbers leave the integral as it was
         * written, which the page sets with its sign and its limits. */
        M42Node *unevaluated = m42_node_new (M42_NODE_CALL);

        m42_value_unref (err);
        unevaluated->name = g_strdup (numeric_only ? "NIntegrate" : "Integrate");
        g_ptr_array_add (unevaluated->children,
                         symbolic_argument (s, m42_node_child (call, 0), var));
        g_ptr_array_add (unevaluated->children, m42_node_copy (spec));
        return m42_value_expr (unevaluated);
      }
  }

  if (!numeric_only)
    {
      integrand = symbolic_argument (s, m42_node_child (call, 0), var);
      anti = m42_node_integrate (integrand, var);
    }
  if (anti != NULL)
    {
      /* The antiderivative at both ends; if either is not a number --
       * a pole inside, or an end at infinity the antiderivative cannot
       * be asked about -- there is still the numeric way below. */
      double fa, fb;

      if (antiderivative_at (s, anti, var, a, &fa) &&
          antiderivative_at (s, anti, var, b, &fb))
        {
          double x = fb - fa;

          /* Exact answers should look exact: 9, not 8.999999999999998. */
          if (fabs (x - round (x)) < 1e-9 * MAX (1.0, fabs (x)))
            x = round (x);
          return m42_value_number (x);
        }
    }

  if (!isfinite (a) || !isfinite (b))
    {
      /* One end at infinity is pulled back into a finite range; both
       * ends are two of those, one each way from nothing. */
      M42Value *answer = NULL;

      if (isfinite (a) && isinf (b))
        answer = simpson_to_infinity (s, m42_node_child (call, 0), var, a, b > 0 ? 1 : -1);
      else if (isinf (a) && isfinite (b))
        {
          g_autoptr (M42Value) half =
            simpson_to_infinity (s, m42_node_child (call, 0), var, b, a > 0 ? 1 : -1);
          double x;

          if (value_number (half, &x))
            answer = m42_value_number (-x);
        }
      else if (isinf (a) && isinf (b) && a < b)
        {
          g_autoptr (M42Value) down =
            simpson_to_infinity (s, m42_node_child (call, 0), var, 0, -1);
          g_autoptr (M42Value) up =
            simpson_to_infinity (s, m42_node_child (call, 0), var, 0, 1);
          double x, y;

          if (value_number (down, &x) && value_number (up, &y))
            answer = m42_value_number (y - x);
        }

      if (answer != NULL && is_num (answer) && isfinite (answer->u.number))
        {
          double x = answer->u.number;

          if (fabs (x - round (x)) < 1e-7 * MAX (1.0, fabs (x)))
            {
              m42_value_unref (answer);
              return m42_value_number (round (x));
            }
          return answer;
        }
      g_clear_pointer (&answer, m42_value_unref);

      /* Nothing came of it: the integral is kept whole. */
      {
        M42Node *unevaluated = m42_node_new (M42_NODE_CALL);

        unevaluated->name = g_strdup (numeric_only ? "NIntegrate" : "Integrate");
        g_ptr_array_add (unevaluated->children,
                         symbolic_argument (s, m42_node_child (call, 0), var));
        g_ptr_array_add (unevaluated->children, m42_node_copy (spec));
        return m42_value_expr (unevaluated);
      }
    }

  {
    M42Value *numeric = simpson (s, m42_node_child (call, 0), var, a, b);
    double x;

    /* A number, but not a usable one: the function has no value at one
     * of the ends, or grew past what a double holds on the way.  The
     * rule that crowds its points towards the ends gets 1/Sqrt[x]
     * where Simpson cannot.  Anything that is not a number at all --
     * an inner integral still holding the outer variable -- is left
     * alone, since there is nothing for either rule to add up. */
    if (is_num (numeric) && !isfinite (numeric->u.number))
      {
        M42Value *crowded = tanh_sinh (s, m42_node_child (call, 0), var, a, b);

        if (crowded != NULL)
          {
            m42_value_unref (numeric);
            return crowded;
          }
      }

    /* Quadrature this good landing a whisker from a simple number
     * means the number, not the whisker. */
    if (is_num (numeric) && isfinite (numeric->u.number))
      {
        x = numeric->u.number;
        if (fabs (x - round (x)) < 1e-9 * MAX (1.0, fabs (x)))
          {
            m42_value_unref (numeric);
            return m42_value_number (round (x));
          }
        for (int den = 2; den <= 24; den++)
          {
            double scaled = x * den;

            if (fabs (scaled - round (scaled)) < 1e-9 * MAX (1.0, fabs (scaled)))
              {
                m42_value_unref (numeric);
                return m42_value_rational ((gint64) round (scaled), den);
              }
          }
      }
    return numeric;
  }
}

/* MATLAB: integral(f, a, b) with f a function value. */
static M42Value *
integral_matlab (M42Session *s, const M42Node *call)
{
  double a, b, h, sum;
  M42Value *err;
  const int n = 2000;

  if (call->children->len != 3)
    return m42_value_error ("integral expects a function and two bounds");
  {
    g_autoptr (M42Value) va = eval (s, m42_node_child (call, 1));
    g_autoptr (M42Value) vb = eval (s, m42_node_child (call, 2));
    if (!need_number (va, "integral", &a, &err) || !need_number (vb, "integral", &b, &err))
      return err;
  }
  h = (b - a) / n;
  sum = number_at (s, m42_node_child (call, 0), "x", a) +
        number_at (s, m42_node_child (call, 0), "x", b);
  for (int i = 1; i < n; i++)
    sum += (i % 2 == 1 ? 4 : 2) * number_at (s, m42_node_child (call, 0), "x", a + i * h);
  return m42_value_number (sum * h / 3);
}

/* A number as a series coefficient is written the way it would be by
 * hand: x^3/6, not 0.166666666666667 x^3.  Continued fractions find
 * the fraction it is, if it is a simple one. */
static M42Node *
coefficient_node (double c)
{
  double frac = fabs (c);
  gint64 p0 = 0, p1 = 1, q0 = 1, q1 = 0;

  if (c == floor (c) || !isfinite (c))
    return m42_node_number (c);

  for (int i = 0; i < 20; i++)
    {
      double whole = floor (frac);
      gint64 p2 = (gint64) whole * p1 + p0, q2 = (gint64) whole * q1 + q0;

      p0 = p1; p1 = p2;
      q0 = q1; q1 = q2;
      if (q1 > 0 && q1 <= 100000 &&
          fabs ((double) p1 / (double) q1 - fabs (c)) < 1e-12 * fabs (c))
        {
          /* The sign goes on the top, so that a term reads -x/2 rather
           * than -(1/2) x once it is multiplied out. */
          return m42_node_binary (M42_TOK_SLASH,
                                  m42_node_number (c < 0 ? -(double) p1 : (double) p1),
                                  m42_node_number ((double) q1));
        }
      frac -= whole;
      if (fabs (frac) < 1e-15)
        break;
      frac = 1 / frac;
    }
  return m42_node_number (c);
}

/* Limit[f, x -> a]: the value f closes in on, found by walking in from
 * both sides.  A one-sided limit is what comes back when the two sides
 * disagree only in sign of infinity. */
/* A number that is one of the constants everyone would rather see
 * written out: 2.7182818 is E, not itself.  Only used where an answer
 * was worked out numerically and would otherwise be printed as a row
 * of digits -- a limit, for one.
 *
 * The tolerance is a part in ten million, which is about as close as
 * sampling a function out towards infinity can get before the doubles
 * themselves start to wobble.  No two constants in the list are within
 * a hundred thousand times that of each other, so a match this close
 * is the constant and not a coincidence. */
static M42Node *
nice_constant (double x)
{
  static const struct { const char *how; double value; } KNOWN[] = {
    { "E",           2.718281828459045 },
    { "Pi",          3.141592653589793 },
    { "Pi/2",        1.570796326794897 },
    { "Pi/3",        1.047197551196598 },
    { "Pi/4",        0.785398163397448 },
    { "Pi/6",        0.523598775598299 },
    { "2 Pi",        6.283185307179586 },
    { "Sqrt[2]",     1.414213562373095 },
    { "Sqrt[3]",     1.732050807568877 },
    { "Log[2]",      0.693147180559945 },
    { "Pi^2/6",      1.644934066848226 },
    { "GoldenRatio", 1.618033988749895 },
  };

  for (guint i = 0; i < G_N_ELEMENTS (KNOWN); i++)
    for (int sign = 1; sign >= -1; sign -= 2)
      {
        double want = sign * KNOWN[i].value;

        if (fabs (x - want) < 1e-7 * fabs (want))
          {
            g_autofree char *complaint = NULL;
            M42Node *tree = m42_parse (KNOWN[i].how, &complaint);

            if (tree == NULL)
              return NULL;
            return sign > 0 ? tree : m42_node_unary (M42_TOK_MINUS, tree);
          }
      }
  return NULL;
}

static M42Value *
limit (M42Session *s, const M42Node *call)
{
  const M42Node *rule;
  const char *var;
  double a;
  double left = NAN, right = NAN;
  M42Value *err;

  if (call->children->len != 2)
    return m42_value_error ("Limit expects an expression and a rule like x -> 0");
  rule = m42_node_child (call, 1);
  if (rule->kind != M42_NODE_RULE || m42_node_child (rule, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("Limit expects a rule like x -> 0");
  var = m42_node_child (rule, 0)->name;
  {
    g_autoptr (M42Value) va = eval (s, m42_node_child (rule, 1));
    if (!need_number (va, "Limit", &a, &err))
      return err;
  }

  if (isinf (a))
    {
      /* Off to infinity: sample further and further out, and at each
       * distance take a second sample twice as far.  The error of such
       * a sample dies away like 1/t, so twice the far one less the
       * near one cancels that first order of it -- which is the
       * difference between answering 2.7182820 and 2.718281828459. */
      double prev = NAN;

      for (double t = 1e2; t <= 1e7; t *= 10)
        {
          double near = number_at (s, m42_node_child (call, 0), var, a > 0 ? t : -t);
          double far = number_at (s, m42_node_child (call, 0), var, a > 0 ? 2 * t : -2 * t);
          double x;

          if (isnan (near) || isnan (far))
            {
              M42Node *unevaluated = m42_node_new (M42_NODE_CALL);

              unevaluated->name = g_strdup ("Limit");
              g_ptr_array_add (unevaluated->children,
                               symbolic_argument (s, m42_node_child (call, 0), var));
              g_ptr_array_add (unevaluated->children, m42_node_copy (rule));
              return m42_value_expr (unevaluated);
            }
          if (!isfinite (near) || !isfinite (far))
            return m42_value_number (far);
          x = 2 * far - near;
          /* A value walking down to nothing has its limit at nothing. */
          if (fabs (x) < 1e-8)
            return m42_value_number (0);
          if (isfinite (prev) && fabs (x - prev) < 1e-10 * MAX (1.0, fabs (x)))
            {
              M42Node *known = nice_constant (x);

              if (known != NULL)
                return expr_result (known);
              return m42_value_number (fabs (x - round (x)) < 1e-9 ? round (x) : x);
            }
          prev = x;
        }
      {
        M42Node *known = nice_constant (prev);

        if (known != NULL)
          return expr_result (known);
      }
      return m42_value_number (fabs (prev - round (prev)) < 1e-6 ? round (prev) : prev);
    }

  /* Two steps, the second half the size, pulled together: the error of
   * a one-sided sample falls off with the step, and going any finer
   * than this only lets the subtraction eat the digits. */
  {
    const M42Node *f = m42_node_child (call, 0);
    double coarse_right = number_at (s, f, var, a + 1e-3);
    double fine_right = number_at (s, f, var, a + 5e-4);
    double coarse_left = number_at (s, f, var, a - 1e-3);
    double fine_left = number_at (s, f, var, a - 5e-4);

    right = 2 * fine_right - coarse_right;
    left = 2 * fine_left - coarse_left;
    if (!isfinite (right))
      right = fine_right;
    if (!isfinite (left))
      left = fine_left;
  }

  /* Nothing to sample -- the expression is symbolic -- so the limit is
   * kept as it was written, for the page to set under the word lim. */
  if (isnan (left) && isnan (right))
    {
      M42Node *unevaluated = m42_node_new (M42_NODE_CALL);

      unevaluated->name = g_strdup ("Limit");
      g_ptr_array_add (unevaluated->children,
                       symbolic_argument (s, m42_node_child (call, 0), var));
      g_ptr_array_add (unevaluated->children, m42_node_copy (rule));
      return m42_value_expr (unevaluated);
    }
  if (!isfinite (left) || !isfinite (right))
    return m42_value_number (left == right ? left : NAN);
  if (fabs (left - right) > 1e-4 * MAX (1.0, fabs (left) + fabs (right)))
    return m42_value_error ("Limit: the two sides do not agree (%g from the left, %g from the right)",
                            left, right);
  {
    double x = (left + right) / 2;

    /* A limit that lands on a round number, or a simple fraction,
     * should say so rather than show the last digits of the sampling. */
    if (fabs (x - round (x)) < 1e-6 * MAX (1.0, fabs (x)))
      return m42_value_number (round (x));
    for (int den = 2; den <= 24; den++)
      {
        double scaled = x * den;

        if (fabs (scaled - round (scaled)) < 1e-6)
          return m42_value_rational ((gint64) round (scaled), den);
      }
    return m42_value_number (x);
  }
}

/* Series[f, {x, a, n}]: the Taylor polynomial, its coefficients found
 * by differentiating symbolically and evaluating at a. */
static M42Value *
series (M42Session *s, const M42Node *call)
{
  const M42Node *spec;
  const char *var;
  double a, order;
  M42Value *err;
  g_autoptr (M42Node) f = NULL;
  M42Node *out = NULL;
  double factorial_k = 1;

  if (call->children->len != 2)
    return m42_value_error ("Series expects an expression and {x, a, n}");
  spec = m42_node_child (call, 1);
  if (spec->kind != M42_NODE_LIST || spec->children->len != 3 ||
      m42_node_child (spec, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("Series expects {x, a, n}");
  var = m42_node_child (spec, 0)->name;
  {
    g_autoptr (M42Value) va = eval (s, m42_node_child (spec, 1));
    g_autoptr (M42Value) vn = eval (s, m42_node_child (spec, 2));
    if (!need_number (va, "Series", &a, &err) || !need_number (vn, "Series", &order, &err))
      return err;
  }
  if (order < 0 || order > 20)
    return m42_value_error ("Series: the order must be between 0 and 20");

  f = symbolic_argument (s, m42_node_child (call, 0), var);

  for (int k = 0; k <= (int) order; k++)
    {
      double c;
      M42Node *term;

      if (k > 0)
        {
          M42Node *d = m42_node_differentiate (f, var);
          if (d == NULL)
            return m42_value_error ("Series: cannot differentiate that expression %d times", k);
          m42_node_free (f);
          f = m42_node_simplify (d);
          m42_node_free (d);
          factorial_k *= k;
        }

      c = number_at (s, f, var, a);
      if (!isfinite (c))
        return m42_value_error ("Series: the expression is not smooth at %g", a);
      c /= factorial_k;
      if (fabs (c) < 1e-12)
        continue;
      if (fabs (c - round (c)) < 1e-9 * MAX (1.0, fabs (c)))
        c = round (c);

      if (k == 0)
        term = coefficient_node (fabs (c));
      else
        {
          M42Node *base = a == 0 ? m42_node_ident (var)
                                 : m42_node_binary (M42_TOK_MINUS, m42_node_ident (var),
                                                    m42_node_number (a));
          if (k > 1)
            base = m42_node_binary (M42_TOK_CARET, base, m42_node_number (k));
          term = fabs (c) == 1 ? base
                               : m42_node_binary (M42_TOK_STAR, coefficient_node (fabs (c)), base);
        }
      /* A negative coefficient is a term taken away, not one added:
       * x - x^3/6 rather than x + -(1/6) x^3. */
      if (out == NULL)
        out = c < 0 ? m42_node_unary (M42_TOK_MINUS, term) : term;
      else
        out = m42_node_binary (c < 0 ? M42_TOK_MINUS : M42_TOK_PLUS, out, term);
    }

  return expr_result (out != NULL ? out : m42_node_number (0));
}

/* f(x, y) for a differential equation: the right-hand side evaluated
 * with both the free variable and the unknown bound. */
static double
number_at2 (M42Session *s, const M42Node *f, const char *xvar, double x,
            const char *yvar, double y)
{
  g_autoptr (M42Value) v = NULL;
  double r;

  push_scope (s);
  bind (s, xvar, m42_value_number (x));
  bind (s, yvar, m42_value_number (y));
  v = eval (s, f);
  if (!value_number (v, &r))
    r = NAN;
  pop_scope (s);
  return r;
}

/* NDSolve[f, {x, a, b}, y0] walks the equation y' = f(x, y) from a to
 * b by the classic fourth-order Runge-Kutta step, and hands back the
 * points it went through -- a list ListLinePlot can draw.  MATLAB's
 * ode45 is the same thing under its own name. */
static M42Value *
ndsolve (M42Session *s, const M42Node *call)
{
  const M42Node *f, *spec;
  const char *xvar, *yvar = "y";
  double a, b, y, h;
  M42Value *err, *out;
  const int steps = 400;

  if (call->children->len < 3)
    return m42_value_error ("NDSolve expects y' = f, {x, a, b} and the value at a");
  f = m42_node_child (call, 0);
  spec = m42_node_child (call, 1);
  if (spec->kind != M42_NODE_LIST || spec->children->len != 3 ||
      m42_node_child (spec, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("NDSolve expects {x, a, b} as its second argument");
  xvar = m42_node_child (spec, 0)->name;
  if (call->children->len == 4 && m42_node_child (call, 3)->kind == M42_NODE_IDENT)
    yvar = m42_node_child (call, 3)->name;
  {
    g_autoptr (M42Value) va = eval (s, m42_node_child (spec, 1));
    g_autoptr (M42Value) vb = eval (s, m42_node_child (spec, 2));
    g_autoptr (M42Value) vy = eval (s, m42_node_child (call, 2));
    if (!need_number (va, "NDSolve", &a, &err) || !need_number (vb, "NDSolve", &b, &err) ||
        !need_number (vy, "NDSolve", &y, &err))
      return err;
  }

  h = (b - a) / steps;
  out = m42_value_list_new ();
  for (int i = 0; i <= steps; i++)
    {
      double x = a + i * h;
      M42Value *pair = m42_value_list_new ();
      double k1, k2, k3, k4;

      m42_value_list_append (pair, m42_value_number (x));
      m42_value_list_append (pair, m42_value_number (y));
      m42_value_list_append (out, pair);

      if (i == steps)
        break;
      k1 = number_at2 (s, f, xvar, x, yvar, y);
      k2 = number_at2 (s, f, xvar, x + h / 2, yvar, y + h * k1 / 2);
      k3 = number_at2 (s, f, xvar, x + h / 2, yvar, y + h * k2 / 2);
      k4 = number_at2 (s, f, xvar, x + h, yvar, y + h * k3);
      y += h * (k1 + 2 * k2 + 2 * k3 + k4) / 6;
      if (!isfinite (y))
        break;
    }
  return out;
}




/* --- the calculus of several variables ----------------------------------
 *
 * Partial derivatives come for free: D differentiates with respect to
 * whichever name it is given, and the rest of a second course is what
 * you build out of that -- the gradient, the divergence, the curl, the
 * two matrices of second derivatives, and an integral done one
 * variable at a time.
 */

/* The names a vector operator was given, as a list of strings. */
static gboolean
variable_names (const M42Node *spec, GPtrArray *out)
{
  if (spec->kind == M42_NODE_IDENT)
    {
      g_ptr_array_add (out, (gpointer) spec->name);
      return TRUE;
    }
  if (spec->kind != M42_NODE_LIST || spec->children->len == 0)
    return FALSE;
  for (guint i = 0; i < spec->children->len; i++)
    {
      const M42Node *one = m42_node_child (spec, i);

      if (one->kind != M42_NODE_IDENT)
        return FALSE;
      g_ptr_array_add (out, (gpointer) one->name);
    }
  return TRUE;
}

/* One partial derivative, simplified. */
static M42Node *
partial (const M42Node *f, const char *var)
{
  M42Node *d = m42_node_differentiate (f, var);
  M42Node *simple;

  if (d == NULL)
    return NULL;
  simple = m42_node_simplify (d);
  m42_node_free (d);
  return simple;
}

/* Grad, Div, Curl, Laplacian, Hessian and Jacobian, which are all the
 * same handful of partial derivatives arranged differently. */
static M42Value *
vector_calculus (M42Session *s, const M42Node *call, const char *name)
{
  g_autoptr (GPtrArray) vars = g_ptr_array_new ();
  const M42Node *field;
  g_autoptr (M42Node) f = NULL;
  gboolean wants_field = name_is (name, "Div", "divergence") ||
                         name_is (name, "Curl", "curl") ||
                         name_is (name, "Jacobian", "jacobian");

  if (call->children->len != 2)
    return m42_value_error ("%s expects an expression and the variables", name);
  if (!variable_names (m42_node_child (call, 1), vars))
    return m42_value_error ("%s expects a list of names, as {x, y, z}", name);

  field = m42_node_child (call, 0);
  /* Everything that has a value already is put in, so that only the
   * variables of the field are left as names. */
  {
    M42Node *marked = m42_node_copy (field);

    push_scope (s);
    for (guint i = 0; i < vars->len; i++)
      bind (s, g_ptr_array_index (vars, i),
            m42_value_expr (m42_node_ident (g_ptr_array_index (vars, i))));
    {
      g_autoptr (M42Value) v = eval (s, marked);
      M42Node *tree = value_to_node (v);

      if (tree != NULL)
        {
          m42_node_free (marked);
          marked = tree;
        }
    }
    pop_scope (s);
    f = marked;
  }

  if (wants_field && f->kind != M42_NODE_LIST)
    return m42_value_error ("%s expects a list of expressions, one for each direction", name);

  /* The gradient, and the Laplacian which is the divergence of it. */
  if (name_is (name, "Grad", "gradient") || name_is (name, "Laplacian", "laplacian"))
    {
      gboolean sum_them = name[0] == 'L' || name[0] == 'l';
      M42Value *out = sum_them ? NULL : m42_value_list_new ();
      M42Node *total = NULL;

      for (guint i = 0; i < vars->len; i++)
        {
          const char *var = g_ptr_array_index (vars, i);
          M42Node *d = partial (f, var);
          M42Node *dd;

          if (d == NULL)
            {
              g_clear_pointer (&out, m42_value_unref);
              m42_node_free (total);
              return m42_value_error ("%s: cannot differentiate that", name);
            }
          if (!sum_them)
            {
              m42_value_list_append (out, expr_result (d));
              continue;
            }
          dd = partial (d, var);
          m42_node_free (d);
          if (dd == NULL)
            {
              m42_node_free (total);
              return m42_value_error ("%s: cannot differentiate that twice", name);
            }
          total = total == NULL ? dd : m42_node_binary (M42_TOK_PLUS, total, dd);
        }
      return sum_them ? expr_result (total) : out;
    }

  /* The divergence: the sum of the diagonal derivatives. */
  if (name_is (name, "Div", "divergence"))
    {
      M42Node *total = NULL;

      if (f->children->len != vars->len)
        return m42_value_error ("Div: the field needs one part for each variable");
      for (guint i = 0; i < vars->len; i++)
        {
          M42Node *d = partial (m42_node_child (f, i), g_ptr_array_index (vars, i));

          if (d == NULL)
            {
              m42_node_free (total);
              return m42_value_error ("Div: cannot differentiate that");
            }
          total = total == NULL ? d : m42_node_binary (M42_TOK_PLUS, total, d);
        }
      return expr_result (total);
    }

  /* The curl, in three dimensions. */
  if (name_is (name, "Curl", "curl"))
    {
      M42Value *out;
      M42Node *parts[3];
      static const int a[3] = { 1, 2, 0 };
      static const int b[3] = { 2, 0, 1 };

      if (vars->len != 3 || f->children->len != 3)
        return m42_value_error ("Curl wants three parts and three variables");
      for (int i = 0; i < 3; i++)
        {
          M42Node *first = partial (m42_node_child (f, b[i]), g_ptr_array_index (vars, a[i]));
          M42Node *second = first != NULL
            ? partial (m42_node_child (f, a[i]), g_ptr_array_index (vars, b[i])) : NULL;

          if (second == NULL)
            {
              m42_node_free (first);
              for (int k = 0; k < i; k++)
                m42_node_free (parts[k]);
              return m42_value_error ("Curl: cannot differentiate that");
            }
          parts[i] = m42_node_binary (M42_TOK_MINUS, first, second);
        }
      out = m42_value_list_new ();
      for (int i = 0; i < 3; i++)
        m42_value_list_append (out, expr_result (parts[i]));
      return out;
    }

  /* The Hessian: second derivatives, one row for each variable. */
  if (name_is (name, "Hessian", "hessian"))
    {
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < vars->len; i++)
        {
          M42Value *row = m42_value_list_new ();
          g_autoptr (M42Node) first = partial (f, g_ptr_array_index (vars, i));

          if (first == NULL)
            {
              m42_value_unref (row);
              m42_value_unref (out);
              return m42_value_error ("Hessian: cannot differentiate that");
            }
          for (guint j = 0; j < vars->len; j++)
            {
              M42Node *second = partial (first, g_ptr_array_index (vars, j));

              if (second == NULL)
                {
                  m42_value_unref (row);
                  m42_value_unref (out);
                  return m42_value_error ("Hessian: cannot differentiate that twice");
                }
              m42_value_list_append (row, expr_result (second));
            }
          m42_value_list_append (out, row);
        }
      return out;
    }

  /* The Jacobian: one row for each part of the field. */
  if (name_is (name, "Jacobian", "jacobian"))
    {
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < f->children->len; i++)
        {
          M42Value *row = m42_value_list_new ();

          for (guint j = 0; j < vars->len; j++)
            {
              M42Node *d = partial (m42_node_child (f, i), g_ptr_array_index (vars, j));

              if (d == NULL)
                {
                  m42_value_unref (row);
                  m42_value_unref (out);
                  return m42_value_error ("Jacobian: cannot differentiate that");
                }
              m42_value_list_append (row, expr_result (d));
            }
          m42_value_list_append (out, row);
        }
      return out;
    }

  return m42_value_error ("%s is not a vector operator math42 knows", name);
}

/* --- Fourier ------------------------------------------------------------
 *
 * The series of a function on an interval, its coefficients worked out
 * by quadrature; and the discrete transform of a list, under both
 * names, each with the convention its own language uses.
 */

/* One coefficient: the integral of f(x) times a wave, over 2L.  The
 * samples are taken at the middles of the strips rather than at their
 * edges, which keeps them symmetric about the centre of the interval
 * -- so a function that is odd there gives exactly nothing for its
 * cosines, jump and all, instead of a whisker of quadrature error. */
static double
fourier_midpoint (M42Session *s, const M42Node *f, const char *var,
                  double lo, double hi, int k, gboolean sine, int steps)
{
  double h = (hi - lo) / steps;
  double half = (hi - lo) / 2;
  double middle = (lo + hi) / 2;
  double sum = 0;

  for (int i = 0; i < steps; i++)
    {
      double x = lo + (i + 0.5) * h;
      double angle = k * G_PI * (x - middle) / half;

      sum += number_at (s, f, var, x) * (sine ? sin (angle) : cos (angle));
    }
  return sum * h / half;
}

static double
fourier_integral (M42Session *s, const M42Node *f, const char *var,
                  double lo, double hi, int k, gboolean sine)
{
  /* The midpoint rule twice over, and the two answers pulled together:
   * its error goes as the square of the step, so most of what is left
   * cancels between one count of strips and twice that many. */
  double coarse = fourier_midpoint (s, f, var, lo, hi, k, sine, 1500);
  double fine = fourier_midpoint (s, f, var, lo, hi, k, sine, 3000);

  {
    double c = fine + (fine - coarse) / 3;

    /* Numbers this small are the quadrature talking, not the function. */
    if (fabs (c) < 1e-9)
      return 0;
    if (fabs (c - round (c)) < 1e-9)
      return round (c);
    /* A coefficient that is a simple fraction is given as one: the
     * quadrature is good to eleven figures, which is enough to tell. */
    for (int den = 2; den <= 64; den++)
      {
        double scaled = c * den;

        if (fabs (scaled - round (scaled)) < 1e-8)
          return round (scaled) / den;
      }
    return c;
  }
}

/* FourierSeries[f, {x, a, b}, n]: the truncated series, as an
 * expression you can plot next to the function it came from. */
static M42Value *
fourier_series (M42Session *s, const M42Node *call, gboolean coefficients_only)
{
  const M42Node *spec;
  const char *var;
  double lo, hi, order;
  M42Value *err;
  M42Node *out = NULL;
  M42Value *pairs = NULL;
  double half, middle, a0;

  if (call->children->len != 3)
    return m42_value_error ("FourierSeries expects f, {x, a, b} and how many terms");
  spec = m42_node_child (call, 1);
  if (spec->kind != M42_NODE_LIST || spec->children->len != 3 ||
      m42_node_child (spec, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("FourierSeries expects {x, a, b} as its second argument");
  var = m42_node_child (spec, 0)->name;
  {
    g_autoptr (M42Value) va = eval (s, m42_node_child (spec, 1));
    g_autoptr (M42Value) vb = eval (s, m42_node_child (spec, 2));
    g_autoptr (M42Value) vn = eval (s, m42_node_child (call, 2));

    if (!need_number (va, "FourierSeries", &lo, &err) ||
        !need_number (vb, "FourierSeries", &hi, &err) ||
        !need_number (vn, "FourierSeries", &order, &err))
      return err;
  }
  if (hi <= lo || order < 0 || order > 60)
    return m42_value_error ("FourierSeries: the interval or the number of terms is out of range");

  half = (hi - lo) / 2;
  middle = (lo + hi) / 2;
  a0 = fourier_integral (s, m42_node_child (call, 0), var, lo, hi, 0, FALSE);

  if (coefficients_only)
    pairs = m42_value_list_new ();
  else
    out = coefficient_node (a0 / 2);

  for (int k = 1; k <= (int) order; k++)
    {
      double ak = fourier_integral (s, m42_node_child (call, 0), var, lo, hi, k, FALSE);
      double bk = fourier_integral (s, m42_node_child (call, 0), var, lo, hi, k, TRUE);

      if (fabs (ak) < 1e-9)
        ak = 0;
      if (fabs (bk) < 1e-9)
        bk = 0;

      if (coefficients_only)
        {
          M42Value *pair = m42_value_list_new ();

          m42_value_list_append (pair, m42_value_real (ak));
          m42_value_list_append (pair, m42_value_real (bk));
          m42_value_list_append (pairs, pair);
          continue;
        }

      /* cos(k pi (x - middle)/L) and its sine.  When the half-length is
       * itself a simple multiple of pi -- as it is on [-Pi, Pi] -- the
       * pi cancels and the wave is written without one. */
      {
        M42Node *inside;
        M42Node *frequency = NULL;
        double ratio = half / G_PI;

        if (middle == 0)
          inside = m42_node_ident (var);
        else
          inside = m42_node_binary (M42_TOK_MINUS, m42_node_ident (var),
                                    m42_node_number (middle));

        for (int den = 1; den <= 12 && frequency == NULL; den++)
          {
            double scaled = ratio * den;

            if (fabs (scaled - round (scaled)) < 1e-12 && fabs (scaled) >= 1)
              frequency = coefficient_node (k * den / round (scaled));
          }
        if (frequency == NULL)
          frequency = m42_node_binary (M42_TOK_SLASH,
                                       m42_node_binary (M42_TOK_STAR, m42_node_number (k),
                                                        m42_node_ident ("Pi")),
                                       m42_node_number (half));

        inside = m42_node_binary (M42_TOK_STAR, frequency, inside);

        if (ak != 0)
          {
            M42Node *term = m42_node_binary (M42_TOK_STAR, coefficient_node (ak),
                                             m42_node_call1 ("Cos", m42_node_copy (inside)));
            out = m42_node_binary (M42_TOK_PLUS, out, term);
          }
        if (bk != 0)
          {
            M42Node *term = m42_node_binary (M42_TOK_STAR, coefficient_node (bk),
                                             m42_node_call1 ("Sin", m42_node_copy (inside)));
            out = m42_node_binary (M42_TOK_PLUS, out, term);
          }
        m42_node_free (inside);
      }
    }

  if (coefficients_only)
    {
      M42Value *all = m42_value_list_new ();

      m42_value_list_append (all, m42_value_real (a0));
      m42_value_list_append (all, pairs);
      return all;
    }
  return expr_result (out);
}

/* The discrete transform of a list.  Mathematica's Fourier divides by
 * the square root of the length and turns the other way from MATLAB's
 * fft, so each name gets the convention its own language uses. */
static M42Value *
discrete_fourier (GPtrArray *args, gboolean inverse, gboolean matlab)
{
  M42Value *data;
  guint n;
  M42Value *out;
  double scale;
  double sign;

  if (args->len != 1 || ARG (0)->kind != M42_VALUE_LIST)
    return m42_value_error ("The transform wants one list");
  data = ARG (0);
  n = m42_value_list_length (data);
  if (n == 0 || n > 4096)
    return m42_value_error ("The transform wants between one and four thousand numbers");

  for (guint i = 0; i < n; i++)
    {
      M42Value *e = m42_value_list_nth (data, i);
      if (!is_numeric (e))
        return m42_value_error ("The transform wants numbers");
    }

  if (matlab)
    {
      scale = inverse ? 1.0 / n : 1.0;
      sign = inverse ? 1.0 : -1.0;
    }
  else
    {
      scale = 1.0 / sqrt ((double) n);
      sign = inverse ? -1.0 : 1.0;
    }

  out = m42_value_list_new ();
  for (guint k = 0; k < n; k++)
    {
      double _Complex acc = 0;

      for (guint j = 0; j < n; j++)
        {
          double angle = sign * 2 * G_PI * (double) j * (double) k / (double) n;
          acc += as_complex (m42_value_list_nth (data, j)) * (cos (angle) + I * sin (angle));
        }
      acc *= scale;
      m42_value_list_append (out, from_complex (acc));
    }
  return out;
}


/* --- finding the bottom of a curve ---------------------------------------
 *
 * FindMinimum[f, {x, x0}] walks downhill from where it is told to
 * start; fminbnd looks between two ends by the golden section, which
 * needs no derivative and cannot overshoot.
 */

/* The golden section: the interval closed in on from both sides. */
static double
golden_section (M42Session *s, const M42Node *f, const char *var,
                double lo, double hi, gboolean maximise)
{
  const double phi = 0.6180339887498949;
  double a = lo, b = hi;
  double c = b - phi * (b - a), d = a + phi * (b - a);
  double fc = number_at (s, f, var, c), fd = number_at (s, f, var, d);

  if (maximise)
    {
      fc = -fc;
      fd = -fd;
    }
  for (int i = 0; i < 200 && fabs (b - a) > 1e-12 * (1 + fabs (a) + fabs (b)); i++)
    {
      if (fc < fd)
        {
          b = d;
          d = c;
          fd = fc;
          c = b - phi * (b - a);
          fc = number_at (s, f, var, c) * (maximise ? -1 : 1);
        }
      else
        {
          a = c;
          c = d;
          fc = fd;
          d = a + phi * (b - a);
          fd = number_at (s, f, var, d) * (maximise ? -1 : 1);
        }
    }
  return (a + b) / 2;
}

/* Downhill from a starting point, by steps that shrink when they
 * overshoot -- enough for the well-behaved functions anyone hands it. */
static double
walk_downhill (M42Session *s, const M42Node *f, const char *var, double start,
               gboolean maximise)
{
  double x = start;
  double step = 0.1 * (1 + fabs (start));
  double here = number_at (s, f, var, x) * (maximise ? -1 : 1);

  for (int i = 0; i < 500 && step > 1e-13; i++)
    {
      double left = number_at (s, f, var, x - step) * (maximise ? -1 : 1);
      double right = number_at (s, f, var, x + step) * (maximise ? -1 : 1);

      if (isfinite (left) && left < here)
        {
          x -= step;
          here = left;
        }
      else if (isfinite (right) && right < here)
        {
          x += step;
          here = right;
        }
      else
        step /= 2;
    }
  return x;
}

static M42Value *
find_extremum (M42Session *s, const M42Node *call, const char *name)
{
  const M42Node *spec;
  const char *var;
  gboolean maximise = strstr (name, "Max") != NULL || strstr (name, "max") != NULL;
  gboolean bounded = name_is (name, "fminbnd", NULL) || name_is (name, "fmaxbnd", NULL);
  double where, value;
  M42Value *err, *out, *pair;

  if (call->children->len < 2)
    return m42_value_error ("%s expects a function and where to look", name);
  spec = m42_node_child (call, 1);

  if (bounded || (spec->kind == M42_NODE_LIST && spec->children->len == 3))
    {
      /* Between two ends: FindMinimum[f, {x, a, b}] or fminbnd(f, a, b). */
      double lo, hi;
      const M42Node *low, *high;

      if (bounded && call->children->len == 3)
        {
          var = "x";
          low = m42_node_child (call, 1);
          high = m42_node_child (call, 2);
        }
      else if (spec->kind == M42_NODE_LIST && spec->children->len == 3 &&
               m42_node_child (spec, 0)->kind == M42_NODE_IDENT)
        {
          var = m42_node_child (spec, 0)->name;
          low = m42_node_child (spec, 1);
          high = m42_node_child (spec, 2);
        }
      else
        return m42_value_error ("%s expects {x, a, b} or two ends", name);

      {
        g_autoptr (M42Value) a = eval (s, low);
        g_autoptr (M42Value) b = eval (s, high);

        if (!need_number (a, name, &lo, &err) || !need_number (b, name, &hi, &err))
          return err;
      }
      if (hi <= lo)
        return m42_value_error ("%s: the second end must be past the first", name);
      where = golden_section (s, m42_node_child (call, 0), var, lo, hi, maximise);
    }
  else
    {
      /* From a starting point: FindMinimum[f, {x, x0}]. */
      double start;

      if (spec->kind != M42_NODE_LIST || spec->children->len != 2 ||
          m42_node_child (spec, 0)->kind != M42_NODE_IDENT)
        return m42_value_error ("%s expects {x, x0} or {x, a, b}", name);
      var = m42_node_child (spec, 0)->name;
      {
        g_autoptr (M42Value) v = eval (s, m42_node_child (spec, 1));

        if (!need_number (v, name, &start, &err))
          return err;
      }
      where = walk_downhill (s, m42_node_child (call, 0), var, start, maximise);
    }

  value = number_at (s, m42_node_child (call, 0), var, where);
  if (fabs (where - round (where)) < 1e-7)
    where = round (where);
  if (fabs (value - round (value)) < 1e-7)
    value = round (value);

  /* MATLAB gives the place; Mathematica gives {value, {x -> place}}. */
  if (bounded)
    return m42_value_number (where);

  pair = m42_value_list_new ();
  {
    M42Node *rule = m42_node_new (M42_NODE_RULE);

    g_ptr_array_add (rule->children, m42_node_ident (var));
    g_ptr_array_add (rule->children, coefficient_node (where));
    m42_value_list_append (pair, m42_value_expr (m42_node_simplify (rule)));
    m42_node_free (rule);
  }
  out = m42_value_list_new ();
  m42_value_list_append (out, m42_value_number (value));
  m42_value_list_append (out, pair);
  return out;
}

/* --- sums with a name for their end ---------------------------------------
 *
 * Sum[i, {i, 1, n}] is n(n+1)/2, and the same for any polynomial in i:
 * the sum of a polynomial is a polynomial one degree higher, and its
 * coefficients follow from enough known values.
 */
static M42Value *
closed_form_sum (M42Session *s, const M42Node *call, const char *upper_name)
{
  const M42Node *spec = m42_node_child (call, 1);
  const char *index;
  g_autoptr (M42Node) body = NULL;
  g_autoptr (GArray) coefficients = g_array_new (FALSE, TRUE, sizeof (double));
  guint degree;
  double lower;

  if (spec->children->len != 3 || m42_node_child (spec, 0)->kind != M42_NODE_IDENT)
    return NULL;
  index = m42_node_child (spec, 0)->name;
  {
    g_autoptr (M42Value) v = eval (s, m42_node_child (spec, 1));

    if (!value_number (v, &lower) || lower != floor (lower))
      return NULL;
  }

  body = symbolic_argument (s, m42_node_child (call, 0), index);
  if (!m42_node_polynomial (body, index, coefficients))
    return NULL;
  degree = coefficients->len - 1;
  if (degree > 6)
    return NULL;

  /* The answer is a polynomial of one degree higher; it is settled by
   * its values at enough points, which are sums we can just add up. */
  {
    guint unknowns = degree + 2;
    g_autoptr (M42Matrix) m = m42_matrix_new (unknowns, unknowns);
    g_autoptr (M42Matrix) rhs = m42_matrix_new (unknowns, 1);
    g_autoptr (M42Matrix) answer = NULL;
    g_autoptr (GArray) found = g_array_new (FALSE, TRUE, sizeof (double));

    for (guint row = 0; row < unknowns; row++)
      {
        double top = lower + row;      /* the sum up to this point */
        double total = 0;

        for (double i = lower; i <= top + 0.5; i += 1)
          {
            double term = 0;

            for (guint k = coefficients->len; k > 0; k--)
              term = term * i + g_array_index (coefficients, double, k - 1);
            total += term;
          }
        for (guint col = 0; col < unknowns; col++)
          *m42_matrix_at (m, row, col) = pow (top, (double) col);
        *m42_matrix_at (rhs, row, 0) = total;
      }

    answer = m42_matrix_solve (m, rhs);
    if (answer == NULL)
      return NULL;

    g_array_set_size (found, unknowns);
    for (guint i = 0; i < unknowns; i++)
      {
        double c = *m42_matrix_at (answer, i, 0);

        /* The coefficients of such a sum are simple fractions. */
        for (int den = 1; den <= 5040; den++)
          {
            double scaled = c * den;

            if (fabs (scaled - round (scaled)) < 1e-7)
              {
                c = round (scaled) / den;
                break;
              }
          }
        g_array_index (found, double, i) = fabs (c) < 1e-12 ? 0 : c;
      }
    {
      M42Node *polynomial = m42_node_from_polynomial (found, upper_name);

      return expr_result (polynomial);
    }
  }
}

/* --- differential equations ---------------------------------------------
 *
 * DSolve[a y'' + b y' + c y == f, y, x] with numbers for a, b and c:
 * the equations a first and third course set, solved the way they are
 * solved by hand.  The primes are written as they are in Mathematica --
 * y'' -- which the parser reads as a postfix operator and this reads
 * back as derivatives.  Initial conditions may follow in the list:
 * DSolve[{y'' + y == 0, y[0] == 1, y'[0] == 0}, y, x].
 */

/* y, y' or y'' -- how many primes, or -1 if this is not the unknown. */
static int
prime_count (const M42Node *n, const char *unknown)
{
  int primes = 0;

  /* y'[0] is the prime chain applied to a place. */
  if (n->kind == M42_NODE_APPLYFN && n->children->len >= 1)
    n = m42_node_child (n, 0);

  while (n->kind == M42_NODE_UNARY && n->op == M42_TOK_QUOTE)
    {
      primes++;
      n = m42_node_child (n, 0);
    }
  /* y and y[x] both name the unknown. */
  if (n->kind == M42_NODE_IDENT && strcmp (n->name, unknown) == 0)
    return primes;
  if (n->kind == M42_NODE_CALL && strcmp (n->name, unknown) == 0 && n->children->len == 1)
    return primes;
  return -1;
}

/* Every y, y' and y'' in the tree swapped for a name of its own, so
 * that the equation becomes an ordinary expression in three unknowns. */
static M42Node *
mark_derivatives (const M42Node *n, const char *unknown)
{
  int primes = prime_count (n, unknown);
  M42Node *out;

  if (primes >= 0 && primes <= 2)
    {
      g_autofree char *name = g_strdup_printf ("$y%d", primes);
      return m42_node_ident (name);
    }

  out = m42_node_new (n->kind);
  out->op = n->op;
  out->number = n->number;
  out->name = g_strdup (n->name);
  for (guint i = 0; i < n->children->len; i++)
    g_ptr_array_add (out->children, mark_derivatives (m42_node_child (n, i), unknown));
  return out;
}

/* The number in front of one of those names, which must be a constant
 * for the solver to know what to do with it. */
static gboolean
coefficient_of (const M42Node *expr, const char *name, double *out)
{
  g_autoptr (M42Node) d = m42_node_differentiate (expr, name);
  g_autoptr (M42Node) simple = NULL;

  if (d == NULL)
    return FALSE;
  simple = m42_node_simplify (d);
  return constant_fold (simple, out);
}

/* Abs[u] written as u, everywhere in a tree.  The integrating factor
 * comes out of Exp[Log[Abs[x]]] and every textbook drops the bars at
 * that step: the constant in front takes care of the sign, and
 * carrying them further only makes the answer harder to read. */
static M42Node *
without_bars (const M42Node *n)
{
  M42Node *out;

  if (n->kind == M42_NODE_CALL && n->children->len == 1 &&
      (strcmp (n->name, "Abs") == 0 || strcmp (n->name, "abs") == 0))
    return without_bars (m42_node_child (n, 0));

  out = m42_node_new (n->kind);
  out->op = n->op;
  out->number = n->number;
  out->name = g_strdup (n->name);
  for (guint i = 0; i < n->children->len; i++)
    g_ptr_array_add (out->children, without_bars (m42_node_child (n, i)));
  return out;
}

/* y' + p(x) y == q(x), by the integrating factor Exp[INT p dx]:
 *
 *   y = (INT mu q dx + C1)/mu
 *
 * which is the method every first course teaches for a first order
 * equation, and takes in the separable y' == f(x) y as the case where
 * q is nothing at all.  NULL when the equation is not of that shape or
 * one of the two integrals is not one math42 can do.
 */
static M42Node *
first_order_linear (const M42Node *marked, const char *var)
{
  g_autoptr (M42Node) da = m42_node_differentiate (marked, "$y1");
  g_autoptr (M42Node) db = m42_node_differentiate (marked, "$y0");
  g_autoptr (M42Node) a = NULL, b = NULL, c = NULL;
  g_autoptr (M42Node) p = NULL, q = NULL;
  g_autoptr (M42Node) integral_p = NULL, mu = NULL, inner = NULL, integral_q = NULL;
  double nothing;

  if (da == NULL || db == NULL)
    return NULL;
  a = m42_node_simplify (da);
  b = m42_node_simplify (db);
  {
    g_autoptr (M42Node) zero = m42_node_number (0);
    g_autoptr (M42Node) without1 = m42_node_substitute (marked, "$y1", zero);
    g_autoptr (M42Node) without0 = m42_node_substitute (without1, "$y0", zero);

    c = m42_node_simplify (without0);
  }

  /* Linear means the coefficients hold no y of their own, and first
   * order means there is no second derivative anywhere in it. */
  {
    static const char *const MARKS[] = { "$y0", "$y1", "$y2" };

    for (guint i = 0; i < G_N_ELEMENTS (MARKS); i++)
      if (m42_node_depends_on (a, MARKS[i]) || m42_node_depends_on (b, MARKS[i]) ||
          m42_node_depends_on (c, MARKS[i]))
        return NULL;
  }
  if (m42_node_depends_on (marked, "$y2"))
    return NULL;
  if (constant_fold (a, &nothing) && nothing == 0)
    return NULL;

  {
    g_autoptr (M42Node) ratio = m42_node_binary (M42_TOK_SLASH, m42_node_copy (b),
                                                 m42_node_copy (a));
    g_autoptr (M42Node) minus_c = m42_node_unary (M42_TOK_MINUS, m42_node_copy (c));
    g_autoptr (M42Node) other = m42_node_binary (M42_TOK_SLASH, g_steal_pointer (&minus_c),
                                                 m42_node_copy (a));

    p = m42_node_simplify (ratio);
    q = m42_node_simplify (other);
  }
  if (p == NULL || q == NULL)
    return NULL;

  integral_p = m42_node_integrate (p, var);
  if (integral_p == NULL)
    return NULL;
  {
    g_autoptr (M42Node) plain = without_bars (integral_p);
    g_autoptr (M42Node) raised = m42_node_call1 ("Exp", g_steal_pointer (&plain));

    mu = m42_node_simplify (raised);
  }
  {
    g_autoptr (M42Node) product = m42_node_binary (M42_TOK_STAR, m42_node_copy (mu),
                                                   m42_node_copy (q));

    inner = m42_node_simplify (product);
  }
  integral_q = m42_node_integrate (inner, var);
  if (integral_q == NULL)
    return NULL;
  {
    g_autoptr (M42Node) top = m42_node_binary (M42_TOK_PLUS, m42_node_copy (integral_q),
                                               m42_node_ident ("C1"));
    g_autoptr (M42Node) whole = m42_node_binary (M42_TOK_SLASH, g_steal_pointer (&top),
                                                 m42_node_copy (mu));

    return m42_node_simplify (whole);
  }
}

/* An answer with one C1 in it, and a condition like y[0] == 3: the
 * answer is a line in C1, so its value at nothing and at one settle
 * which C1 it must be. */
static M42Node *
fit_one_constant (M42Session *s, const M42Node *answer, const char *var,
                  const char *unknown, GPtrArray *conditions)
{
  const M42Node *cond;
  double at, want, with_zero, with_one;

  if (conditions->len != 1)
    return NULL;
  cond = g_ptr_array_index (conditions, 0);
  if (cond->kind != M42_NODE_BINARY || cond->op != M42_TOK_EQ ||
      m42_node_child (cond, 0)->kind != M42_NODE_CALL ||
      strcmp (m42_node_child (cond, 0)->name, unknown) != 0 ||
      m42_node_child (cond, 0)->children->len != 1 ||
      !constant_fold (m42_node_child (m42_node_child (cond, 0), 0), &at) ||
      !constant_fold (m42_node_child (cond, 1), &want))
    return NULL;

  {
    g_autoptr (M42Node) zero = m42_node_number (0);
    g_autoptr (M42Node) one = m42_node_number (1);
    g_autoptr (M42Node) at_zero = m42_node_substitute (answer, "C1", zero);
    g_autoptr (M42Node) at_one = m42_node_substitute (answer, "C1", one);

    with_zero = number_at (s, at_zero, var, at);
    with_one = number_at (s, at_one, var, at);
  }
  if (!isfinite (with_zero) || !isfinite (with_one) ||
      fabs (with_one - with_zero) < 1e-14)
    return NULL;
  {
    double c = (want - with_zero) / (with_one - with_zero);
    g_autoptr (M42Node) found = coefficient_node (c);
    g_autoptr (M42Node) filled = m42_node_substitute (answer, "C1", found);

    return m42_node_simplify (filled);
  }
}

/* The two solutions of the homogeneous equation, and one of the whole
 * equation, as trees in x. */
typedef struct {
  M42Node *basis[2];      /* owned */
  M42Node *particular;    /* owned, may be NULL */
  guint    count;
} Solution;

static void
solution_clear (Solution *sol)
{
  for (guint i = 0; i < 2; i++)
    g_clear_pointer (&sol->basis[i], m42_node_free);
  g_clear_pointer (&sol->particular, m42_node_free);
}

static M42Node *
exponential (double r, const char *var)
{
  if (r == 0)
    return m42_node_number (1);
  if (r == 1)
    return m42_node_call1 ("Exp", m42_node_ident (var));
  return m42_node_call1 ("Exp", m42_node_binary (M42_TOK_STAR, m42_node_number (r),
                                                 m42_node_ident (var)));
}

/* Builds the general solution of a y'' + b y' + c y = f, with f a
 * constant.  Returns FALSE when the coefficients are not ones it can
 * work with. */
static gboolean
solve_linear_ode (double a, double b, double c, double f, const char *var, Solution *sol)
{
  memset (sol, 0, sizeof *sol);

  if (fabs (a) < 1e-14)
    {
      /* First order: b y' + c y = f. */
      if (fabs (b) < 1e-14)
        {
          if (fabs (c) < 1e-14)
            return FALSE;
          sol->count = 0;
          sol->particular = m42_node_number (f / c);
          return TRUE;
        }
      sol->count = 1;
      sol->basis[0] = exponential (-c / b, var);
      if (fabs (c) > 1e-14)
        sol->particular = m42_node_number (f / c);
      else if (fabs (f) > 1e-14)
        sol->particular = m42_node_binary (M42_TOK_STAR, m42_node_number (f / b),
                                           m42_node_ident (var));
      return TRUE;
    }

  {
    double disc = b * b - 4 * a * c;

    sol->count = 2;
    if (disc > 1e-12)
      {
        sol->basis[0] = exponential ((-b + sqrt (disc)) / (2 * a), var);
        sol->basis[1] = exponential ((-b - sqrt (disc)) / (2 * a), var);
      }
    else if (fabs (disc) <= 1e-12)
      {
        double r = -b / (2 * a);

        sol->basis[0] = exponential (r, var);
        sol->basis[1] = m42_node_binary (M42_TOK_STAR, m42_node_ident (var),
                                         exponential (r, var));
      }
    else
      {
        double alpha = -b / (2 * a), beta = sqrt (-disc) / (2 * a);
        M42Node *inside = beta == 1 ? m42_node_ident (var)
          : m42_node_binary (M42_TOK_STAR, m42_node_number (beta), m42_node_ident (var));

        sol->basis[0] = m42_node_call1 ("Cos", inside);
        sol->basis[1] = m42_node_call1 ("Sin", m42_node_copy (inside));
        if (alpha != 0)
          {
            sol->basis[0] = m42_node_binary (M42_TOK_STAR, exponential (alpha, var), sol->basis[0]);
            sol->basis[1] = m42_node_binary (M42_TOK_STAR, exponential (alpha, var), sol->basis[1]);
          }
      }

    /* A constant on the right needs a constant answer, unless the
     * equation has no y in it to balance one. */
    if (fabs (f) > 1e-14)
      {
        if (fabs (c) > 1e-14)
          sol->particular = m42_node_number (f / c);
        else if (fabs (b) > 1e-14)
          sol->particular = m42_node_binary (M42_TOK_STAR, m42_node_number (f / b),
                                             m42_node_ident (var));
        else
          sol->particular = m42_node_binary (M42_TOK_STAR, m42_node_number (f / (2 * a)),
                                             m42_node_binary (M42_TOK_CARET,
                                                              m42_node_ident (var),
                                                              m42_node_number (2)));
      }
    return TRUE;
  }
}

/* A tree in one name, at a number. */
static gboolean
value_at (const M42Node *n, const char *var, double x, double *out)
{
  g_autoptr (M42Node) point = m42_node_number (x);
  g_autoptr (M42Node) put = m42_node_substitute (n, var, point);
  g_autoptr (M42Node) simple = m42_node_simplify (put);

  return constant_fold (simple, out);
}

/* C1 u1 + C2 u2 + yp, with the constants either left as names or
 * worked out from the conditions given. */
static M42Node *
assemble_solution (const Solution *sol, const double *constants)
{
  M42Node *out = NULL;

  for (guint i = 0; i < sol->count; i++)
    {
      M42Node *term;

      if (constants != NULL)
        {
          double k = constants[i];

          if (fabs (k) < 1e-12)
            continue;
          term = fabs (k - 1) < 1e-12 ? m42_node_copy (sol->basis[i])
            : m42_node_binary (M42_TOK_STAR, m42_node_number (k),
                               m42_node_copy (sol->basis[i]));
        }
      else
        {
          g_autofree char *name = g_strdup_printf ("C%u", i + 1);
          term = m42_node_binary (M42_TOK_STAR, m42_node_ident (name),
                                  m42_node_copy (sol->basis[i]));
        }
      out = out == NULL ? term : m42_node_binary (M42_TOK_PLUS, out, term);
    }

  if (sol->particular != NULL)
    {
      M42Node *p = m42_node_copy (sol->particular);
      out = out == NULL ? p : m42_node_binary (M42_TOK_PLUS, out, p);
    }
  return out != NULL ? out : m42_node_number (0);
}

static M42Value *
dsolve (M42Session *s, const M42Node *call)
{
  const M42Node *eqns, *unknown_node, *var_node;
  const char *unknown, *var;
  g_autoptr (M42Node) marked = NULL;
  g_autoptr (M42Node) left = NULL;
  double a, b, c, f;
  Solution sol;
  M42Value *answer;
  g_autoptr (GPtrArray) conditions = g_ptr_array_new ();

  if (call->children->len != 3)
    return m42_value_error ("DSolve expects an equation, the unknown and the variable");
  eqns = m42_node_child (call, 0);
  unknown_node = m42_node_child (call, 1);
  var_node = m42_node_child (call, 2);
  if (unknown_node->kind != M42_NODE_IDENT || var_node->kind != M42_NODE_IDENT)
    return m42_value_error ("DSolve expects names for the unknown and the variable");
  unknown = unknown_node->name;
  var = var_node->name;

  /* The equation itself, and any conditions after it. */
  if (eqns->kind == M42_NODE_LIST)
    {
      if (eqns->children->len == 0)
        return m42_value_error ("DSolve: there is no equation there");
      for (guint i = 1; i < eqns->children->len; i++)
        g_ptr_array_add (conditions, (gpointer) m42_node_child (eqns, i));
      eqns = m42_node_child (eqns, 0);
    }

  /* Everything on one side. */
  if (eqns->kind == M42_NODE_BINARY && eqns->op == M42_TOK_EQ)
    left = m42_node_binary (M42_TOK_MINUS, m42_node_copy (m42_node_child (eqns, 0)),
                            m42_node_copy (m42_node_child (eqns, 1)));
  else
    left = m42_node_copy (eqns);

  marked = mark_derivatives (left, unknown);
  if (!coefficient_of (marked, "$y2", &a) || !coefficient_of (marked, "$y1", &b) ||
      !coefficient_of (marked, "$y0", &c))
    {
      /* The coefficients are not numbers.  A first order equation can
       * still be solved by its integrating factor, which is what a
       * course reaches for when x appears in front of the y. */
      M42Node *by_factor = first_order_linear (marked, var);

      if (by_factor != NULL)
        {
          M42Node *fitted = fit_one_constant (s, by_factor, var, unknown, conditions);

          if (fitted != NULL)
            {
              m42_node_free (by_factor);
              return expr_result (fitted);
            }
          return expr_result (by_factor);
        }
      return m42_value_error ("DSolve: the equation must be linear, and of the first "
                              "order when its coefficients are not numbers");
    }

  /* What is left when the unknown is taken out is the forcing term. */
  {
    g_autoptr (M42Node) zero = m42_node_number (0);
    g_autoptr (M42Node) a2 = m42_node_substitute (marked, "$y2", zero);
    g_autoptr (M42Node) a1 = m42_node_substitute (a2, "$y1", zero);
    g_autoptr (M42Node) a0 = m42_node_substitute (a1, "$y0", zero);
    g_autoptr (M42Node) simple = m42_node_simplify (a0);
    double rest;

    if (!constant_fold (simple, &rest))
      {
        /* Something of x on the right: the first order equation still
         * comes out by its integrating factor. */
        M42Node *by_factor = first_order_linear (marked, var);

        if (by_factor != NULL)
          {
            M42Node *fitted = fit_one_constant (s, by_factor, var, unknown, conditions);

            if (fitted != NULL)
              {
                m42_node_free (by_factor);
                return expr_result (fitted);
              }
            return expr_result (by_factor);
          }
        return m42_value_error ("DSolve: only a constant is allowed on the right of an "
                                "equation of the second order");
      }
    f = -rest;
  }

  if (!solve_linear_ode (a, b, c, f, var, &sol))
    return m42_value_error ("DSolve: there is no equation left once the unknown is gone");

  /* Conditions, if any: two numbers to find, from two equations. */
  if (conditions->len > 0 && sol.count > 0)
    {
      g_autoptr (M42Matrix) m = m42_matrix_new (sol.count, sol.count);
      g_autoptr (M42Matrix) rhs = m42_matrix_new (sol.count, 1);
      g_autoptr (M42Matrix) found = NULL;
      double constants[2] = { 0, 0 };
      guint row = 0;

      for (guint i = 0; i < conditions->len && row < sol.count; i++)
        {
          const M42Node *cond = g_ptr_array_index (conditions, i);
          const M42Node *lhs, *rhs_node;
          int primes;
          double at, want, base;

          if (cond->kind != M42_NODE_BINARY || cond->op != M42_TOK_EQ)
            return m42_value_error ("DSolve: a condition looks like y[0] == 1");
          lhs = m42_node_child (cond, 0);
          rhs_node = m42_node_child (cond, 1);

          primes = prime_count (lhs, unknown);
          if (primes < 0)
            return m42_value_error ("DSolve: a condition must be about %s", unknown);
          {
            /* Where the condition is set: y[0] or y'[0]. */
            const M42Node *where = NULL;

            if (lhs->kind == M42_NODE_APPLYFN && lhs->children->len == 2)
              where = m42_node_child (lhs, 1);
            else
              {
                const M42Node *inner = lhs;

                while (inner->kind == M42_NODE_UNARY)
                  inner = m42_node_child (inner, 0);
                if (inner->kind == M42_NODE_CALL && inner->children->len == 1)
                  where = m42_node_child (inner, 0);
              }
            if (where == NULL || !constant_fold (where, &at))
              return m42_value_error ("DSolve: a condition must say where, as y[0] == 1");
          }
          if (!constant_fold (rhs_node, &want))
            return m42_value_error ("DSolve: a condition must give a number");

          for (guint k = 0; k < sol.count; k++)
            {
              g_autoptr (M42Node) piece = m42_node_copy (sol.basis[k]);
              double v;

              for (int d = 0; d < primes; d++)
                {
                  M42Node *next = m42_node_differentiate (piece, var);
                  if (next == NULL)
                    return m42_value_error ("DSolve: cannot differentiate the solution");
                  m42_node_free (piece);
                  piece = m42_node_simplify (next);
                  m42_node_free (next);
                }
              if (!value_at (piece, var, at, &v))
                return m42_value_error ("DSolve: cannot work the solution out at %g", at);
              *m42_matrix_at (m, row, k) = v;
            }

          base = 0;
          if (sol.particular != NULL)
            {
              g_autoptr (M42Node) piece = m42_node_copy (sol.particular);

              for (int d = 0; d < primes; d++)
                {
                  M42Node *next = m42_node_differentiate (piece, var);
                  if (next == NULL)
                    break;
                  m42_node_free (piece);
                  piece = m42_node_simplify (next);
                  m42_node_free (next);
                }
              if (!value_at (piece, var, at, &base))
                base = 0;
            }
          *m42_matrix_at (rhs, row, 0) = want - base;
          row++;
        }

      if (row < sol.count)
        return m42_value_error ("DSolve: %u conditions are needed, %u were given",
                                sol.count, row);
      found = m42_matrix_solve (m, rhs);
      if (found == NULL)
        return m42_value_error ("DSolve: those conditions do not settle the constants");
      for (guint k = 0; k < sol.count; k++)
        {
          double v = *m42_matrix_at (found, k, 0);
          constants[k] = fabs (v - round (v)) < 1e-9 ? round (v) : v;
        }
      answer = expr_result (assemble_solution (&sol, constants));
      solution_clear (&sol);
      return answer;
    }

  answer = expr_result (assemble_solution (&sol, NULL));
  solution_clear (&sol);
  return answer;
}



/* The test after /; is true when it comes out true with the names
 * standing for whatever they have just matched. */
static gboolean
pattern_test (const M42Node *test, GHashTable *names, gpointer user_data)
{
  M42Session *s = user_data;
  g_autoptr (M42Node) filled = m42_node_bind (test, names);
  g_autoptr (M42Value) answer = eval (s, filled);

  return is_num (answer) && answer->u.number != 0;
}

/* expr /. rules, when the replacing is asked for by name rather than
 * written with the operator.  passes is 1 for ReplaceAll and more for
 * ReplaceRepeated, which goes round until nothing changes. */
static M42Value *
replace_by_rules (M42Session *s, const M42Value *subject, const M42Value *rules,
                  int passes)
{
  g_autoptr (M42Node) tree = value_to_node (subject);
  g_autoptr (GPtrArray) list = g_ptr_array_new ();

  if (tree == NULL)
    return m42_value_error ("ReplaceAll wants an expression to work on");
  if (rules->kind == M42_VALUE_LIST)
    for (guint i = 0; i < m42_value_list_length (rules); i++)
      g_ptr_array_add (list, m42_value_list_nth (rules, i));
  else
    g_ptr_array_add (list, (gpointer) rules);

  for (int pass = 0; pass < passes; pass++)
    {
      gboolean changed = FALSE;

      for (guint k = 0; k < list->len; k++)
        {
          const M42Value *rule = g_ptr_array_index (list, k);
          const M42Node *from, *to;
          M42Node *next;

          if (rule->kind != M42_VALUE_EXPR || rule->u.expr->kind != M42_NODE_RULE)
            return m42_value_error ("ReplaceAll wants rules like x -> 2");
          from = m42_node_child (rule->u.expr, 0);
          to = m42_node_child (rule->u.expr, 1);
          if (from->kind == M42_NODE_IDENT)
            next = m42_node_substitute (tree, from->name, to);
          else
            next = m42_node_replace_all (tree, from, to, pattern_test, s, NULL);
          if (!m42_node_same (next, tree))
            changed = TRUE;
          m42_node_free (tree);
          tree = next;
        }
      if (!changed)
        break;
    }
  return eval (s, tree);
}

/* Whether a value answers to a pattern.  A blank on its own is asked of
 * the value, which knows whether it is exact in a way its written form
 * does not; anything with a shape to it goes to the matcher. */
static gboolean
value_matches (M42Session *s, const M42Value *v, const M42Value *pattern)
{
  const M42Node *shape = pattern->kind == M42_VALUE_EXPR ? pattern->u.expr : NULL;

  if (shape != NULL && shape->kind == M42_NODE_PATTERN && shape->op == M42_BLANK)
    {
      const char *head;
      g_autofree char *actual = NULL;
      GPtrArray *one;
      g_autoptr (M42Value) kind = NULL;

      if (shape->children->len == 0)
        return TRUE;                 /* _ stands for anything */
      head = m42_node_child (shape, 0)->name;

      one = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
      g_ptr_array_add (one, m42_value_ref ((M42Value *) v));
      kind = call_builtin (s, "Head", one);
      g_ptr_array_unref (one);
      if (kind->kind != M42_VALUE_STRING)
        return FALSE;
      actual = g_strdup (kind->u.string);
      /* An integer is a rational and a real as far as a pattern goes,
       * which is how a first look at patterns is usually explained. */
      if (strcmp (actual, head) == 0)
        return TRUE;
      if (strcmp (actual, "Integer") == 0 &&
          (strcmp (head, "Real") == 0 || strcmp (head, "Rational") == 0))
        return TRUE;
      return FALSE;
    }

  if (shape != NULL && m42_node_has_pattern (shape))
    {
      g_autoptr (M42Node) subject = value_to_node (v);
      g_autoptr (GHashTable) names = m42_pattern_names_new ();

      return subject != NULL &&
             m42_node_match (shape, subject, names, pattern_test, s);
    }

  {
    g_autofree char *a = m42_value_to_string (v);
    g_autofree char *b = m42_value_to_string (pattern);

    return strcmp (a, b) == 0;
  }
}

/* Euclid, with polynomials in place of numbers, made monic at the end
 * as a greatest common divisor is given. */
static void
polynomial_gcd (const GArray *first, const GArray *second, GArray *out)
{
  g_autoptr (GArray) a = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) b = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) q = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) r = g_array_new (FALSE, TRUE, sizeof (double));
  double lead;

  g_array_set_size (a, first->len);
  memcpy (a->data, first->data, sizeof (double) * first->len);
  g_array_set_size (b, second->len);
  memcpy (b->data, second->data, sizeof (double) * second->len);

  /* Euclid runs until the remainder is nothing at all, not until it
   * is a constant.  Stopping at a constant answers gcd(x, 8) with x,
   * and then Cancel divides 8 by x and reports that x/8 is infinite,
   * which is what it used to do. */
  for (int guard = 0; guard < 64; guard++)
    {
      gboolean nothing = TRUE;

      for (guint i = 0; i < b->len && nothing; i++)
        if (fabs (g_array_index (b, double, i)) > 1e-12)
          nothing = FALSE;
      if (nothing)
        break;
      m42_polynomial_divide (a, b, q, r);
      g_array_set_size (a, b->len);
      memcpy (a->data, b->data, sizeof (double) * b->len);
      g_array_set_size (b, r->len);
      memcpy (b->data, r->data, sizeof (double) * r->len);
    }
  lead = a->len > 0 ? g_array_index (a, double, a->len - 1) : 0;
  if (fabs (lead) > 1e-14)
    for (guint i = 0; i < a->len; i++)
      {
        double c = g_array_index (a, double, i) / lead;

        g_array_index (a, double, i) = fabs (c - round (c)) < 1e-9 ? round (c) : c;
      }
  g_array_set_size (out, a->len);
  memcpy (out->data, a->data, sizeof (double) * a->len);
}

/* The first name in an expression that is not one of the constants,
 * which is the variable Cancel and Simplify work in when they are not
 * told one. */
static const char *
first_symbol (const M42Node *n)
{
  static const char *const KNOWN[] = { "Pi", "E", "I", "Infinity", "Degree",
                                       "EulerGamma", "GoldenRatio", "True", "False" };

  if (n == NULL)
    return NULL;
  if (n->kind == M42_NODE_IDENT)
    {
      for (guint i = 0; i < G_N_ELEMENTS (KNOWN); i++)
        if (strcmp (n->name, KNOWN[i]) == 0)
          return NULL;
      return n->name;
    }
  for (guint i = 0; i < n->children->len; i++)
    {
      const char *found = first_symbol (m42_node_child (n, i));

      if (found != NULL)
        return found;
    }
  return NULL;
}

/* A fraction of two polynomials with what divides both taken off:
 * (x^2 - 1)/(x - 1) is x + 1.  NULL when there is nothing to take. */
static M42Node *
cancel_common_factor (const M42Node *tree)
{
  const char *var;
  g_autoptr (GArray) top = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) bottom = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) common = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) q = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) r = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (M42Node) num = NULL;
  g_autoptr (M42Node) den = NULL;

  if (tree->kind != M42_NODE_BINARY || tree->op != M42_TOK_SLASH)
    return NULL;
  var = first_symbol (tree);
  if (var == NULL)
    return NULL;
  if (!m42_node_polynomial (m42_node_child (tree, 0), var, top) ||
      !m42_node_polynomial (m42_node_child (tree, 1), var, bottom))
    return NULL;

  polynomial_gcd (top, bottom, common);
  if (common->len < 2)
    return NULL;                 /* nothing but a number divides both */

  m42_polynomial_divide (top, common, q, r);
  num = m42_node_from_polynomial (q, var);
  m42_polynomial_divide (bottom, common, q, r);
  den = m42_node_from_polynomial (q, var);
  if (num == NULL || den == NULL)
    return NULL;
  {
    g_autoptr (M42Node) whole =
      m42_node_binary (M42_TOK_SLASH, g_steal_pointer (&num), g_steal_pointer (&den));

    return m42_node_simplify (whole);
  }
}

/* --- polynomial algebra --------------------------------------------------
 *
 * Coefficient and its relatives, the two ways of writing a rational
 * function, and division with a remainder.  All of them work on the
 * expression as it was written, so the variable stays a name.
 */

/* The expression a polynomial function was given, with the variable
 * kept as a symbol. */
static M42Node *
polynomial_argument (M42Session *s, const M42Node *raw, const char *var)
{
  return symbolic_argument (s, raw, var);
}

static M42Value *
polynomial_algebra (M42Session *s, const M42Node *call, const char *name)
{
  const M42Node *var_node;
  const char *var;
  g_autoptr (M42Node) f = NULL;
  g_autoptr (GArray) coefficients = g_array_new (FALSE, TRUE, sizeof (double));

  /* Together takes no variable; the rest take one at the end. */
  if (name_is (name, "Together", "simplifyfraction") || name_is (name, "Cancel", NULL))
    {
      g_autoptr (M42Value) v = NULL;
      g_autoptr (M42Node) combined = NULL;
      M42Node *tree;

      if (call->children->len != 1)
        return m42_value_error ("%s expects one expression", name);
      v = eval (s, m42_node_child (call, 0));
      if (is_error (v))
        return m42_value_ref (v);
      tree = value_to_node (v);
      if (tree == NULL)
        return m42_value_ref (v);
      combined = m42_node_together (tree);
      m42_node_free (tree);

      /* Together puts it over one denominator; Cancel goes on to take
       * off what divides both. */
      if (name_is (name, "Cancel", NULL))
        {
          M42Node *shorter = cancel_common_factor (combined);

          if (shorter != NULL)
            return expr_result (shorter);
        }
      return expr_result (m42_node_copy (combined));
    }

  if (call->children->len < 2)
    return m42_value_error ("%s expects an expression and a variable", name);
  var_node = m42_node_child (call, 1);
  if (name_is (name, "PolynomialQuotient", "deconv") ||
      name_is (name, "PolynomialRemainder", NULL) ||
      name_is (name, "PolynomialGCD", NULL))
    var_node = call->children->len >= 3 ? m42_node_child (call, 2) : NULL;
  if (var_node == NULL || var_node->kind != M42_NODE_IDENT)
    return m42_value_error ("%s expects a name for the variable", name);
  var = var_node->name;

  f = polynomial_argument (s, m42_node_child (call, 0), var);

  /* Two polynomials, divided. */
  if (name_is (name, "PolynomialQuotient", "deconv") ||
      name_is (name, "PolynomialRemainder", NULL) ||
      name_is (name, "PolynomialGCD", NULL))
    {
      g_autoptr (M42Node) g = polynomial_argument (s, m42_node_child (call, 1), var);
      g_autoptr (GArray) a = g_array_new (FALSE, TRUE, sizeof (double));
      g_autoptr (GArray) b = g_array_new (FALSE, TRUE, sizeof (double));
      g_autoptr (GArray) q = g_array_new (FALSE, TRUE, sizeof (double));
      g_autoptr (GArray) r = g_array_new (FALSE, TRUE, sizeof (double));

      if (!m42_node_polynomial (f, var, a) || !m42_node_polynomial (g, var, b))
        return m42_value_error ("%s expects two polynomials", name);

      if (name_is (name, "PolynomialGCD", NULL))
        {
          /* Euclid, with polynomials in place of numbers. */
          for (int guard = 0; guard < 64 && b->len > 1; guard++)
            {
              m42_polynomial_divide (a, b, q, r);
              g_array_set_size (a, b->len);
              memcpy (a->data, b->data, sizeof (double) * b->len);
              g_array_set_size (b, r->len);
              memcpy (b->data, r->data, sizeof (double) * r->len);
            }
          /* Made monic, as a greatest common divisor is given. */
          {
            double lead = g_array_index (a, double, a->len - 1);

            if (fabs (lead) > 1e-14)
              for (guint i = 0; i < a->len; i++)
                {
                  double c = g_array_index (a, double, i) / lead;

                  g_array_index (a, double, i) = fabs (c - round (c)) < 1e-9 ? round (c) : c;
                }
          }
          return expr_result (m42_node_from_polynomial (a, var));
        }

      m42_polynomial_divide (a, b, q, r);
      return expr_result (m42_node_from_polynomial (
        name_is (name, "PolynomialQuotient", "deconv") ? q : r, var));
    }

  if (name_is (name, "Apart", "partfrac"))
    {
      /* Apart works on the fraction itself, so it comes before the
       * check that what we have is a polynomial. */
      M42Node *split = m42_node_apart (f, var);

      if (split == NULL)
        return m42_value_error ("Apart: that is not a fraction math42 can split");
      return expr_result (split);
    }

  if (!m42_node_polynomial (f, var, coefficients))
    return m42_value_error ("%s: that is not a polynomial in %s", name, var);

  if (name_is (name, "CoefficientList", NULL))
    {
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < coefficients->len; i++)
        m42_value_list_append (out, m42_value_number (g_array_index (coefficients, double, i)));
      return out;
    }

  if (name_is (name, "Exponent", NULL))
    return m42_value_number (coefficients->len - 1);

  if (name_is (name, "Coefficient", NULL))
    {
      double which = 1;
      M42Value *err;

      if (call->children->len >= 3)
        {
          g_autoptr (M42Value) v = eval (s, m42_node_child (call, 2));

          if (!need_number (v, name, &which, &err))
            return err;
        }
      if (which < 0 || which != floor (which))
        return m42_value_error ("Coefficient wants a whole power");
      if (which >= coefficients->len)
        return m42_value_number (0);
      return m42_value_number (g_array_index (coefficients, double, (guint) which));
    }

  if (name_is (name, "Collect", "collect"))
    return expr_result (m42_node_from_polynomial (coefficients, var));

  return m42_value_error ("%s is not a polynomial function math42 knows", name);
}

/* --- systems of equations ------------------------------------------------
 *
 * Solve[{x + y == 3, x - y == 1}, {x, y}] when every equation is
 * linear in the unknowns: the coefficients are read off with
 * derivatives and the matrix does the rest.
 */
static M42Value *
solve_system (M42Session *s, const M42Node *equations, const M42Node *unknowns)
{
  guint n = unknowns->children->len;
  guint rows = equations->children->len;
  g_autoptr (M42Matrix) m = NULL;
  g_autoptr (M42Matrix) rhs = NULL;
  g_autoptr (M42Matrix) answer = NULL;
  M42Value *out, *pair;

  if (rows != n)
    return m42_value_error ("Solve: %u equations for %u unknowns", rows, n);
  for (guint i = 0; i < n; i++)
    if (m42_node_child (unknowns, i)->kind != M42_NODE_IDENT)
      return m42_value_error ("Solve expects names for the unknowns");

  m = m42_matrix_new (n, n);
  rhs = m42_matrix_new (n, 1);

  for (guint i = 0; i < rows; i++)
    {
      const M42Node *eq = m42_node_child (equations, i);
      g_autoptr (M42Node) left = NULL;
      g_autoptr (M42Node) marked = NULL;
      double constant;

      if (eq->kind == M42_NODE_BINARY && eq->op == M42_TOK_EQ)
        left = m42_node_binary (M42_TOK_MINUS, m42_node_copy (m42_node_child (eq, 0)),
                                m42_node_copy (m42_node_child (eq, 1)));
      else
        left = m42_node_copy (eq);

      /* The unknowns kept as names while everything else is worked out. */
      push_scope (s);
      for (guint k = 0; k < n; k++)
        {
          const char *name = m42_node_child (unknowns, k)->name;

          bind (s, name, m42_value_expr (m42_node_ident (name)));
        }
      {
        g_autoptr (M42Value) v = eval (s, left);
        M42Node *tree = value_to_node (v);

        marked = tree != NULL ? tree : m42_node_copy (left);
      }
      pop_scope (s);

      for (guint k = 0; k < n; k++)
        {
          const char *name = m42_node_child (unknowns, k)->name;
          double coefficient;

          if (!coefficient_of (marked, name, &coefficient))
            return m42_value_error ("Solve: the equations must be linear in the unknowns");
          *m42_matrix_at (m, i, k) = coefficient;
        }

      {
        g_autoptr (M42Node) zeroed = m42_node_copy (marked);
        g_autoptr (M42Node) zero = m42_node_number (0);

        for (guint k = 0; k < n; k++)
          {
            M42Node *next = m42_node_substitute (zeroed, m42_node_child (unknowns, k)->name, zero);

            m42_node_free (zeroed);
            zeroed = next;
          }
        {
          g_autoptr (M42Node) simple = m42_node_simplify (zeroed);

          if (!constant_fold (simple, &constant))
            return m42_value_error ("Solve: the equations must be linear in the unknowns");
        }
      }
      *m42_matrix_at (rhs, i, 0) = -constant;
    }

  answer = m42_matrix_solve (m, rhs);
  if (answer == NULL)
    return m42_value_error ("Solve: those equations do not settle the unknowns");

  /* {{x -> 1, y -> 2}}, as Mathematica gives it. */
  pair = m42_value_list_new ();
  for (guint k = 0; k < n; k++)
    {
      double v = *m42_matrix_at (answer, k, 0);
      M42Node *rule = m42_node_new (M42_NODE_RULE);

      if (fabs (v - round (v)) < 1e-9)
        v = round (v);
      g_ptr_array_add (rule->children, m42_node_ident (m42_node_child (unknowns, k)->name));
      g_ptr_array_add (rule->children, coefficient_node (v));
      m42_value_list_append (pair, m42_value_expr (m42_node_simplify (rule)));
      m42_node_free (rule);
    }
  out = m42_value_list_new ();
  m42_value_list_append (out, pair);
  return out;
}

/* --- recurrences ---------------------------------------------------------
 *
 * RSolve[{a[n] == a[n-1] + a[n-2], a[0] == 0, a[1] == 1}, a, n]: the
 * closed form, found the way a discrete course finds it -- write down
 * the characteristic equation, take its roots, and fit the constants
 * to the first terms.
 */

/* a[n - k], as k; -1 when this is not the sequence being solved for. */
static int
step_back (const M42Node *n, const char *name, const char *index)
{
  const M42Node *argument;

  if (n->kind != M42_NODE_CALL || strcmp (n->name, name) != 0 || n->children->len != 1)
    return -1;
  argument = m42_node_child (n, 0);

  /* a[n] itself. */
  if (argument->kind == M42_NODE_IDENT && strcmp (argument->name, index) == 0)
    return 0;

  /* a[n - k], with k a whole number. */
  if (argument->kind == M42_NODE_BINARY &&
      (argument->op == M42_TOK_MINUS || argument->op == M42_TOK_PLUS) &&
      m42_node_child (argument, 0)->kind == M42_NODE_IDENT &&
      strcmp (m42_node_child (argument, 0)->name, index) == 0 &&
      m42_node_child (argument, 1)->kind == M42_NODE_NUMBER)
    {
      double k = m42_node_child (argument, 1)->number;

      if (argument->op == M42_TOK_PLUS)
        k = -k;
      if (k < 0 || k != floor (k))
        return -1;
      return (int) k;
    }
  return -1;
}

static M42Node *
mark_steps (const M42Node *n, const char *name, const char *index)
{
  int k = step_back (n, name, index);
  M42Node *out;

  if (k >= 0 && k <= 2)
    {
      g_autofree char *marker = g_strdup_printf ("$a%d", k);
      return m42_node_ident (marker);
    }

  out = m42_node_new (n->kind);
  out->op = n->op;
  out->number = n->number;
  out->name = g_strdup (n->name);
  for (guint i = 0; i < n->children->len; i++)
    g_ptr_array_add (out->children, mark_steps (m42_node_child (n, i), name, index));
  return out;
}

static M42Value *
rsolve (M42Session *s, const M42Node *call)
{
  const M42Node *eqns, *name_node, *index_node;
  const char *name, *index;
  g_autoptr (M42Node) left = NULL;
  g_autoptr (M42Node) marked = NULL;
  g_autoptr (GPtrArray) conditions = g_ptr_array_new ();
  double c0, c1, c2, forcing;
  double p, q, disc;
  M42Node *basis[2] = { NULL, NULL };
  M42Node *particular = NULL;    /* the answer to a constant on the right */
  guint order;
  M42Node *out;

  if (call->children->len != 3)
    return m42_value_error ("RSolve expects a recurrence, the sequence and the index");
  eqns = m42_node_child (call, 0);
  name_node = m42_node_child (call, 1);
  index_node = m42_node_child (call, 2);
  if (name_node->kind != M42_NODE_IDENT || index_node->kind != M42_NODE_IDENT)
    return m42_value_error ("RSolve expects names for the sequence and the index");
  name = name_node->name;
  index = index_node->name;

  if (eqns->kind == M42_NODE_LIST)
    {
      if (eqns->children->len == 0)
        return m42_value_error ("RSolve: there is no recurrence there");
      for (guint i = 1; i < eqns->children->len; i++)
        g_ptr_array_add (conditions, (gpointer) m42_node_child (eqns, i));
      eqns = m42_node_child (eqns, 0);
    }

  if (eqns->kind == M42_NODE_BINARY && eqns->op == M42_TOK_EQ)
    left = m42_node_binary (M42_TOK_MINUS, m42_node_copy (m42_node_child (eqns, 0)),
                            m42_node_copy (m42_node_child (eqns, 1)));
  else
    left = m42_node_copy (eqns);

  marked = mark_steps (left, name, index);
  if (!coefficient_of (marked, "$a0", &c0) || !coefficient_of (marked, "$a1", &c1) ||
      !coefficient_of (marked, "$a2", &c2))
    return m42_value_error ("RSolve: the recurrence must be linear with constant coefficients");
  {
    g_autoptr (M42Node) zero = m42_node_number (0);
    g_autoptr (M42Node) a2 = m42_node_substitute (marked, "$a2", zero);
    g_autoptr (M42Node) a1 = m42_node_substitute (a2, "$a1", zero);
    g_autoptr (M42Node) a0 = m42_node_substitute (a1, "$a0", zero);
    g_autoptr (M42Node) simple = m42_node_simplify (a0);

    if (!constant_fold (simple, &forcing))
      return m42_value_error ("RSolve: only a constant is allowed beside the sequence");
  }
  if (fabs (c0) < 1e-14)
    return m42_value_error ("RSolve: the recurrence must say what a[%s] is", index);

  /* a[n] = p a[n-1] + q a[n-2], so r^2 - p r - q = 0. */
  p = -c1 / c0;
  q = -c2 / c0;
  order = fabs (q) > 1e-14 ? 2 : (fabs (p) > 1e-14 ? 1 : 0);

  /* a[n] = p a[n-1] + q a[n-2] + c.  A constant of its own on the
   * right is met by a constant answer, A = c/(1 - p - q); and when
   * that would divide by nothing -- which is to say when 1 is already
   * a root -- by one that climbs, A n with A = c/(p + 2 q). */
  if (fabs (forcing) > 1e-14 && order > 0)
    {
      double c = -forcing / c0;
      double down = 1 - p - q;

      if (fabs (down) > 1e-12)
        particular = coefficient_node (c / down);
      else if (fabs (p + 2 * q) > 1e-12)
        particular = m42_node_binary (M42_TOK_STAR, coefficient_node (c / (p + 2 * q)),
                                      m42_node_ident (index));
      else
        return m42_value_error ("RSolve: that constant on the right is beyond math42");
    }
  else if (fabs (forcing) > 1e-14)
    return m42_value_error ("RSolve: the recurrence must say what %s[%s] is", name, index);

  if (order == 0)
    return m42_value_expr (m42_node_number (0));
  if (order == 1)
    {
      basis[0] = m42_node_binary (M42_TOK_CARET, m42_node_number (p), m42_node_ident (index));
    }
  else
    {
      disc = p * p + 4 * q;

      if (disc > 1e-12)
        {
          double r1 = (p + sqrt (disc)) / 2, r2 = (p - sqrt (disc)) / 2;

          basis[0] = m42_node_binary (M42_TOK_CARET, m42_node_number (r1), m42_node_ident (index));
          basis[1] = m42_node_binary (M42_TOK_CARET, m42_node_number (r2), m42_node_ident (index));
        }
      else if (fabs (disc) <= 1e-12)
        {
          double r = p / 2;

          basis[0] = m42_node_binary (M42_TOK_CARET, m42_node_number (r), m42_node_ident (index));
          basis[1] = m42_node_binary (M42_TOK_STAR, m42_node_ident (index),
                                      m42_node_binary (M42_TOK_CARET, m42_node_number (r),
                                                       m42_node_ident (index)));
        }
      else
        {
          /* A complex pair: a wave with a growing or shrinking size. */
          double modulus = sqrt (-q), angle = atan2 (sqrt (-disc) / 2, p / 2);
          M42Node *size = m42_node_binary (M42_TOK_CARET, m42_node_number (modulus),
                                           m42_node_ident (index));
          M42Node *inside = m42_node_binary (M42_TOK_STAR, m42_node_number (angle),
                                             m42_node_ident (index));

          basis[0] = m42_node_binary (M42_TOK_STAR, size,
                                      m42_node_call1 ("Cos", inside));
          basis[1] = m42_node_binary (M42_TOK_STAR, m42_node_copy (size),
                                      m42_node_call1 ("Sin", m42_node_copy (inside)));
        }
    }

  /* The constants, from the first terms if they were given. */
  if (conditions->len >= order)
    {
      g_autoptr (M42Matrix) m = m42_matrix_new (order, order);
      g_autoptr (M42Matrix) rhs = m42_matrix_new (order, 1);
      g_autoptr (M42Matrix) found = NULL;
      guint row = 0;

      for (guint i = 0; i < conditions->len && row < order; i++)
        {
          const M42Node *cond = g_ptr_array_index (conditions, i);
          double at, want;

          if (cond->kind != M42_NODE_BINARY || cond->op != M42_TOK_EQ ||
              m42_node_child (cond, 0)->kind != M42_NODE_CALL ||
              strcmp (m42_node_child (cond, 0)->name, name) != 0 ||
              !constant_fold (m42_node_child (m42_node_child (cond, 0), 0), &at) ||
              !constant_fold (m42_node_child (cond, 1), &want))
            return m42_value_error ("RSolve: a condition looks like %s[0] == 1", name);

          for (guint k = 0; k < order; k++)
            {
              double v;

              if (!value_at (basis[k], index, at, &v))
                return m42_value_error ("RSolve: cannot work the answer out at %g", at);
              *m42_matrix_at (m, row, k) = v;
            }
          /* What the particular answer already accounts for is not
           * for the constants to explain. */
          if (particular != NULL)
            {
              double already;

              if (!value_at (particular, index, at, &already))
                return m42_value_error ("RSolve: cannot work the answer out at %g", at);
              want -= already;
            }
          *m42_matrix_at (rhs, row, 0) = want;
          row++;
        }

      found = m42_matrix_solve (m, rhs);
      if (found == NULL)
        {
          for (guint k = 0; k < order; k++)
            m42_node_free (basis[k]);
          return m42_value_error ("RSolve: those first terms do not settle the constants");
        }

      out = NULL;
      for (guint k = 0; k < order; k++)
        {
          double v = *m42_matrix_at (found, k, 0);
          M42Node *term;

          if (fabs (v - round (v)) < 1e-9)
            v = round (v);
          if (fabs (v) < 1e-12)
            {
              m42_node_free (basis[k]);
              continue;
            }
          term = fabs (v - 1) < 1e-12 ? basis[k]
            : m42_node_binary (M42_TOK_STAR, coefficient_node (v), basis[k]);
          out = out == NULL ? term : m42_node_binary (M42_TOK_PLUS, out, term);
        }
      if (particular != NULL)
        out = out == NULL ? particular
                          : m42_node_binary (M42_TOK_PLUS, out, particular);
      return expr_result (out != NULL ? out : m42_node_number (0));
    }

  out = NULL;
  for (guint k = 0; k < order; k++)
    {
      g_autofree char *constant = g_strdup_printf ("C%u", k + 1);
      M42Node *term = m42_node_binary (M42_TOK_STAR, m42_node_ident (constant), basis[k]);

      out = out == NULL ? term : m42_node_binary (M42_TOK_PLUS, out, term);
    }
  if (particular != NULL)
    out = out == NULL ? particular : m42_node_binary (M42_TOK_PLUS, out, particular);
  return expr_result (out);
}

/* --- the shape of a program ---------------------------------------------
 *
 * Mathematica writes its loops as functions -- For, While, Do -- and so
 * does math42; each takes its arguments unevaluated and returns Null,
 * as the language it copies does.
 */

/* What a loop should do about an error its body handed back.  BREAK
 * and CONTINUE are caught here and the flag cleared; anything else --
 * a Return[] on its way out of a function, or a real mistake -- is
 * left to walk further. */
typedef enum { LOOP_GO_ON, LOOP_STOP, LOOP_PASS_IT_ON } LoopAnswer;

static LoopAnswer
loop_caught (M42Session *s, const M42Value *body)
{
  if (!is_error (body))
    return LOOP_GO_ON;
  if (s->unwind == M42_UNWIND_BREAK)
    {
      s->unwind = M42_UNWIND_NONE;
      return LOOP_STOP;
    }
  if (s->unwind == M42_UNWIND_CONTINUE)
    {
      s->unwind = M42_UNWIND_NONE;
      return LOOP_GO_ON;
    }
  return LOOP_PASS_IT_ON;
}

static M42Value *
control_flow (M42Session *s, const M42Node *n, gboolean *handled)
{
  const char *name = n->name;
  guint argc = n->children->len;

  *handled = TRUE;

  if (name_is (name, "Break", NULL) && argc == 0)
    {
      s->unwind = M42_UNWIND_BREAK;
      return m42_value_error ("Break[] with no loop around it");
    }

  if (name_is (name, "Continue", NULL) && argc == 0)
    {
      s->unwind = M42_UNWIND_CONTINUE;
      return m42_value_error ("Continue[] with no loop around it");
    }

  if (name_is (name, "Return", NULL) && argc <= 1)
    {
      g_autoptr (M42Value) what = argc == 1 ? eval (s, m42_node_child (n, 0))
                                            : m42_value_null ();

      if (is_error (what))
        return g_steal_pointer (&what);
      g_clear_pointer (&s->returned, m42_value_unref);
      s->returned = g_steal_pointer (&what);
      s->unwind = M42_UNWIND_RETURN;
      return m42_value_error ("Return[] with no function around it");
    }

  /* Switch[x, a, this, b, that, _, otherwise] -- MATLAB writes the same
   * thing with case and otherwise, and both come here. */
  if (name_is (name, "Switch", "switch") && argc >= 3)
    {
      g_autoptr (M42Value) subject = eval (s, m42_node_child (n, 0));

      if (is_error (subject))
        return g_steal_pointer (&subject);
      for (guint i = 1; i + 1 < argc; i += 2)
        {
          g_autoptr (M42Value) against = eval (s, m42_node_child (n, i));

          if (is_error (against))
            return g_steal_pointer (&against);
          if (value_matches (s, subject, against))
            return eval (s, m42_node_child (n, i + 1));
        }
      /* An odd one at the end is what to do when nothing matched. */
      if (argc % 2 == 0)
        return eval (s, m42_node_child (n, argc - 1));
      return m42_value_null ();
    }

  if (name_is (name, "If", NULL) && (argc == 2 || argc == 3))
    {
      g_autoptr (M42Value) c = eval (s, m42_node_child (n, 0));
      if (is_error (c))
        return g_steal_pointer (&c);
      if (!is_num (c))
        return m42_value_error ("If expects a condition");
      if (c->u.number != 0)
        return eval (s, m42_node_child (n, 1));
      return argc == 3 ? eval (s, m42_node_child (n, 2)) : m42_value_null ();
    }

  if (name_is (name, "Which", NULL) && argc >= 2)
    {
      for (guint i = 0; i + 1 < argc; i += 2)
        {
          g_autoptr (M42Value) c = eval (s, m42_node_child (n, i));
          if (is_error (c))
            return g_steal_pointer (&c);
          if (is_num (c) && c->u.number != 0)
            return eval (s, m42_node_child (n, i + 1));
        }
      return m42_value_null ();
    }

  /* For[start, test, step, body] */
  if (name_is (name, "For", NULL) && argc == 4)
    {
      g_autoptr (M42Value) init = eval (s, m42_node_child (n, 0));
      if (is_error (init))
        return g_steal_pointer (&init);
      for (int guard = 0; guard < 1000000; guard++)
        {
          g_autoptr (M42Value) c = eval (s, m42_node_child (n, 1));
          g_autoptr (M42Value) body = NULL;
          g_autoptr (M42Value) step = NULL;

          if (is_error (c))
            return g_steal_pointer (&c);
          if (!is_num (c))
            return m42_value_error ("For expects a condition");
          if (c->u.number == 0)
            return m42_value_null ();
          body = eval (s, m42_node_child (n, 3));
          switch (loop_caught (s, body))
            {
            case LOOP_STOP:        return m42_value_null ();
            case LOOP_PASS_IT_ON:  return g_steal_pointer (&body);
            case LOOP_GO_ON:       break;
            }
          step = eval (s, m42_node_child (n, 2));
          if (is_error (step))
            return g_steal_pointer (&step);
        }
      return m42_value_error ("For: too many turns");
    }

  if (name_is (name, "While", NULL) && argc == 2)
    {
      for (int guard = 0; guard < 1000000; guard++)
        {
          g_autoptr (M42Value) c = eval (s, m42_node_child (n, 0));
          g_autoptr (M42Value) body = NULL;

          if (is_error (c))
            return g_steal_pointer (&c);
          if (!is_num (c))
            return m42_value_error ("While expects a condition");
          if (c->u.number == 0)
            return m42_value_null ();
          body = eval (s, m42_node_child (n, 1));
          switch (loop_caught (s, body))
            {
            case LOOP_STOP:        return m42_value_null ();
            case LOOP_PASS_IT_ON:  return g_steal_pointer (&body);
            case LOOP_GO_ON:       break;
            }
        }
      return m42_value_error ("While: too many turns");
    }

  /* Do[body, {i, 1, 10}] -- Table without the answers. */
  if (name_is (name, "Do", NULL) && argc == 2)
    {
      Iterator it = { 0 };
      M42Value *err = iterator_parse (s, m42_node_child (n, 1), &it, "Do");

      if (err != NULL)
        return err;
      push_scope (s);
      for (guint i = 0; i < m42_value_list_length (it.values); i++)
        {
          g_autoptr (M42Value) r = NULL;
          bind (s, it.var, m42_value_ref (m42_value_list_nth (it.values, i)));
          r = eval (s, m42_node_child (n, 0));
          {
            LoopAnswer what = loop_caught (s, r);

            if (what != LOOP_GO_ON)
              {
                pop_scope (s);
                m42_value_unref (it.values);
                return what == LOOP_STOP ? m42_value_null () : g_steal_pointer (&r);
              }
          }
        }
      pop_scope (s);
      m42_value_unref (it.values);
      return m42_value_null ();
    }

  /* Module[{x, y = 1}, body] and Block, which math42 does not tell
   * apart: names in the list are local to the body. */
  if ((name_is (name, "Module", "Block") || name_is (name, "With", NULL)) && argc == 2 &&
      m42_node_child (n, 0)->kind == M42_NODE_LIST)
    {
      const M42Node *locals = m42_node_child (n, 0);
      M42Value *r;

      push_scope (s);
      for (guint i = 0; i < locals->children->len; i++)
        {
          const M42Node *l = m42_node_child (locals, i);
          if (l->kind == M42_NODE_IDENT)
            bind (s, l->name, m42_value_expr (m42_node_ident (l->name)));
          else if (l->kind == M42_NODE_ASSIGN)
            {
              M42Value *v = eval (s, m42_node_child (l, 0));
              if (is_error (v))
                {
                  pop_scope (s);
                  return v;
                }
              bind (s, l->name, v);
            }
          else
            {
              pop_scope (s);
              return m42_value_error ("%s expects a list of local names", name);
            }
        }
      r = eval (s, m42_node_child (n, 1));
      pop_scope (s);
      return r;
    }

  if (name_is (name, "Print", "disp") && argc >= 1)
    {
      for (guint i = 0; i < argc; i++)
        {
          g_autoptr (M42Value) v = eval (s, m42_node_child (n, i));
          g_autofree char *text = NULL;

          if (is_error (v))
            return g_steal_pointer (&v);
          /* A string is written as it reads, without its quotes. */
          if (v->kind == M42_VALUE_STRING)
            g_string_append (s->printed, v->u.string);
          else
            {
              text = m42_value_to_string (v);
              g_string_append (s->printed, text);
            }
        }
      g_string_append_c (s->printed, '\n');
      return m42_value_null ();
    }

  *handled = FALSE;
  return NULL;
}

/* f(x) for root finding: the two sides of an == subtracted, else f itself. */
static double
residual (M42Session *s, const M42Node *f, const char *var, double x)
{
  if (f->kind == M42_NODE_BINARY && f->op == M42_TOK_EQ)
    return number_at (s, m42_node_child (f, 0), var, x) -
           number_at (s, m42_node_child (f, 1), var, x);
  return number_at (s, f, var, x);
}

static gboolean
newton (M42Session *s, const M42Node *f, const char *var, double *x)
{
  for (int i = 0; i < 100; i++)
    {
      double fx = residual (s, f, var, *x);
      double h = 1e-6 * MAX (fabs (*x), 1.0);
      double dfx = (residual (s, f, var, *x + h) - residual (s, f, var, *x - h)) / (2 * h);
      double step;

      if (!isfinite (fx) || !isfinite (dfx))
        return FALSE;
      if (fabs (fx) < 1e-12)
        return TRUE;
      if (dfx == 0)
        return FALSE;
      step = fx / dfx;
      *x -= step;
      if (fabs (step) < 1e-14 * MAX (fabs (*x), 1.0))
        return fabs (residual (s, f, var, *x)) < 1e-6;
    }
  return fabs (residual (s, f, var, *x)) < 1e-6;
}

static M42Value *
rule_value (const char *var, double x)
{
  M42Node *r = m42_node_new (M42_NODE_RULE);
  g_ptr_array_add (r->children, m42_node_ident (var));
  g_ptr_array_add (r->children, m42_node_number (x));
  return m42_value_expr (r);
}

/* FindRoot[f, {x, x0}] -> {x -> root}; fzero(f, x0) -> root. */
static M42Value *
find_root (M42Session *s, const M42Node *call, gboolean matlab)
{
  const M42Node *spec;
  const char *var = "x";
  double x0;
  M42Value *err;

  if (call->children->len != 2)
    return m42_value_error ("FindRoot expects an equation and {x, x0}");
  spec = m42_node_child (call, 1);
  if (!matlab)
    {
      if (spec->kind != M42_NODE_LIST || spec->children->len != 2 ||
          m42_node_child (spec, 0)->kind != M42_NODE_IDENT)
        return m42_value_error ("FindRoot expects {x, x0}");
      var = m42_node_child (spec, 0)->name;
      spec = m42_node_child (spec, 1);
    }
  {
    g_autoptr (M42Value) v = eval (s, spec);
    if (!need_number (v, "FindRoot", &x0, &err))
      return err;
  }
  if (!newton (s, m42_node_child (call, 0), var, &x0))
    return m42_value_error ("FindRoot: no root found from that starting point");
  if (matlab)
    return m42_value_number (x0);
  {
    M42Value *out = m42_value_list_new ();
    m42_value_list_append (out, rule_value (var, x0));
    return out;
  }
}

/* The coefficients of a polynomial in var, lowest power first, or
 * FALSE if the expression is not one.  Only numbers are accepted as
 * coefficients: a polynomial in two symbols is not one here. */
static gboolean
poly_coeffs (const M42Node *n, const char *var, GArray *out, int depth)
{
  if (depth > 20)
    return FALSE;

  switch (n->kind)
    {
    case M42_NODE_NUMBER:
      {
        double c = n->number;
        g_array_set_size (out, 1);
        g_array_index (out, double, 0) = c;
        return TRUE;
      }

    case M42_NODE_IDENT:
      if (strcmp (n->name, var) != 0)
        return FALSE;
      g_array_set_size (out, 2);
      g_array_index (out, double, 0) = 0;
      g_array_index (out, double, 1) = 1;
      return TRUE;

    case M42_NODE_UNARY:
      if (n->op != M42_TOK_MINUS || !poly_coeffs (m42_node_child (n, 0), var, out, depth + 1))
        return FALSE;
      for (guint i = 0; i < out->len; i++)
        g_array_index (out, double, i) = -g_array_index (out, double, i);
      return TRUE;

    case M42_NODE_BINARY:
      {
        g_autoptr (GArray) a = g_array_new (FALSE, TRUE, sizeof (double));
        g_autoptr (GArray) b = g_array_new (FALSE, TRUE, sizeof (double));

        if (n->op == M42_TOK_CARET)
          {
            const M42Node *e = m42_node_child (n, 1);
            int k;

            if (e->kind != M42_NODE_NUMBER || e->number < 0 || e->number > 40 ||
                e->number != floor (e->number) ||
                !poly_coeffs (m42_node_child (n, 0), var, a, depth + 1))
              return FALSE;
            k = (int) e->number;
            g_array_set_size (out, 1);
            g_array_index (out, double, 0) = 1;
            for (int t = 0; t < k; t++)
              {
                g_autoptr (GArray) acc = g_array_new (FALSE, TRUE, sizeof (double));
                g_array_set_size (acc, out->len + a->len - 1);
                for (guint i = 0; i < out->len; i++)
                  for (guint j = 0; j < a->len; j++)
                    g_array_index (acc, double, i + j) +=
                      g_array_index (out, double, i) * g_array_index (a, double, j);
                g_array_set_size (out, acc->len);
                memcpy (out->data, acc->data, sizeof (double) * acc->len);
              }
            return TRUE;
          }

        if (!poly_coeffs (m42_node_child (n, 0), var, a, depth + 1) ||
            !poly_coeffs (m42_node_child (n, 1), var, b, depth + 1))
          return FALSE;

        switch (n->op)
          {
          case M42_TOK_PLUS: case M42_TOK_MINUS:
            {
              guint len = MAX (a->len, b->len);
              g_array_set_size (out, len);
              for (guint i = 0; i < len; i++)
                {
                  double x = i < a->len ? g_array_index (a, double, i) : 0;
                  double y = i < b->len ? g_array_index (b, double, i) : 0;
                  g_array_index (out, double, i) = n->op == M42_TOK_PLUS ? x + y : x - y;
                }
              return TRUE;
            }
          case M42_TOK_STAR:
            g_array_set_size (out, a->len + b->len - 1);
            memset (out->data, 0, sizeof (double) * out->len);
            for (guint i = 0; i < a->len; i++)
              for (guint j = 0; j < b->len; j++)
                g_array_index (out, double, i + j) +=
                  g_array_index (a, double, i) * g_array_index (b, double, j);
            return TRUE;
          case M42_TOK_SLASH:
            if (b->len != 1 || g_array_index (b, double, 0) == 0)
              return FALSE;
            g_array_set_size (out, a->len);
            for (guint i = 0; i < a->len; i++)
              g_array_index (out, double, i) = g_array_index (a, double, i) /
                                               g_array_index (b, double, 0);
            return TRUE;
          default:
            return FALSE;
          }
      }

    default:
      return FALSE;
    }
}

/* Every root of a polynomial, real and complex, by the Durand-Kerner
 * iteration: start the n roots spread around a circle and let each one
 * walk away from the others until they settle. */
static gboolean
polynomial_roots (GArray *coeffs, GArray *roots)
{
  guint n;
  double lead;
  g_autofree double _Complex *z = NULL;
  g_autofree double _Complex *c = NULL;

  while (coeffs->len > 1 && fabs (g_array_index (coeffs, double, coeffs->len - 1)) < 1e-12)
    g_array_set_size (coeffs, coeffs->len - 1);
  if (coeffs->len < 2)
    return FALSE;

  n = coeffs->len - 1;
  lead = g_array_index (coeffs, double, n);
  c = g_new (double _Complex, n + 1);
  for (guint i = 0; i <= n; i++)
    c[i] = g_array_index (coeffs, double, i) / lead;

  z = g_new (double _Complex, n);
  for (guint i = 0; i < n; i++)
    z[i] = cpow (0.4 + 0.9 * I, i);

  for (int iter = 0; iter < 2000; iter++)
    {
      double move = 0;

      for (guint i = 0; i < n; i++)
        {
          double _Complex p = c[n];
          double _Complex denom = 1;
          double _Complex step;

          for (guint k = n; k > 0; k--)
            p = p * z[i] + c[k - 1];
          for (guint j = 0; j < n; j++)
            if (j != i)
              denom *= z[i] - z[j];
          if (cabs (denom) < 1e-300)
            continue;
          step = p / denom;
          z[i] -= step;
          move = MAX (move, cabs (step));
        }
      if (move < 1e-14)
        break;
    }

  for (guint i = 0; i < n; i++)
    {
      double _Complex r = z[i];
      double re = creal (r), im = cimag (r);

      if (fabs (im) < 1e-8 * MAX (1.0, fabs (re)))
        im = 0;
      if (fabs (re - round (re)) < 1e-8 * MAX (1.0, fabs (re)))
        re = round (re);
      if (fabs (im - round (im)) < 1e-8 * MAX (1.0, fabs (im)))
        im = round (im);
      r = re + im * I;
      g_array_append_val (roots, r);
    }
  return TRUE;
}

static int
compare_roots (gconstpointer a, gconstpointer b)
{
  double _Complex x = *(const double _Complex *) a, y = *(const double _Complex *) b;

  if (creal (x) != creal (y))
    return creal (x) < creal (y) ? -1 : 1;
  if (cimag (x) != cimag (y))
    return cimag (x) < cimag (y) ? -1 : 1;
  return 0;
}

/* Solve[f == g, x]: exactly, root by root, when the equation is a
 * polynomial -- complex roots and all -- and by scanning for sign
 * changes when it is not. */
/* A root found by sweeping comes back as 1.99999999999989 where the
 * answer is 2.  If a simple number nearby answers the equation just as
 * well -- which is checked, not assumed -- that is the one to give. */
static double
polished_root (M42Session *s, const M42Node *f, const char *var, double root)
{
  double here = fabs (residual (s, f, var, root));

  for (int den = 1; den <= 12; den++)
    {
      double near = round (root * den) / den;

      if (fabs (near - root) > 1e-8 * MAX (1.0, fabs (root)))
        continue;
      if (fabs (residual (s, f, var, near)) <= MAX (here, 1e-9))
        return near;
    }
  return root;
}

static M42Value *
solve (M42Session *s, const M42Node *call)
{
  const M42Node *f, *spec;
  const char *var;
  M42Value *out = m42_value_list_new ();
  double prev_x = -100, prev_f;
  GArray *roots = g_array_new (FALSE, FALSE, sizeof (double));

  if (call->children->len != 2)
    return m42_value_error ("Solve expects an equation and a variable");
  f = m42_node_child (call, 0);
  spec = m42_node_child (call, 1);
  if (spec->kind != M42_NODE_IDENT)
    return m42_value_error ("Solve expects a variable as its second argument");
  var = spec->name;

  /* Everything on one side of the equation, which is where both ways
   * of solving it start. */
  g_autoptr (M42Node) lhs = NULL;

  if (f->kind == M42_NODE_BINARY && f->op == M42_TOK_EQ)
    lhs = m42_node_binary (M42_TOK_MINUS,
                           symbolic_argument (s, m42_node_child (f, 0), var),
                           symbolic_argument (s, m42_node_child (f, 1), var));
  else
    lhs = symbolic_argument (s, f, var);

  /* A polynomial with numbers for coefficients gives up all its roots
   * at once. */
  {
    g_autoptr (GArray) coeffs = g_array_new (FALSE, TRUE, sizeof (double));

    if (poly_coeffs (lhs, var, coeffs, 0) && coeffs->len >= 2)
      {
        g_autoptr (GArray) found = g_array_new (FALSE, FALSE, sizeof (double _Complex));

        if (polynomial_roots (coeffs, found))
          {
            g_array_sort (found, compare_roots);
            g_array_unref (roots);
            for (guint i = 0; i < found->len; i++)
              {
                double _Complex r = g_array_index (found, double _Complex, i);
                M42Value *pair;
                M42Node *rule;
                gboolean duplicate = FALSE;

                for (guint k = 0; k < i; k++)
                  if (cabs (g_array_index (found, double _Complex, k) - r) < 1e-7)
                    duplicate = TRUE;
                if (duplicate)
                  continue;

                rule = m42_node_new (M42_NODE_RULE);
                g_ptr_array_add (rule->children, m42_node_ident (var));
                if (cimag (r) == 0)
                  g_ptr_array_add (rule->children, m42_node_number (creal (r)));
                else
                  {
                    /* a + b I as a written expression. */
                    M42Node *im = m42_node_binary (M42_TOK_STAR,
                                                   m42_node_number (cimag (r)),
                                                   m42_node_ident ("I"));
                    g_ptr_array_add (rule->children,
                                     creal (r) == 0 ? im
                                     : m42_node_binary (M42_TOK_PLUS,
                                                        m42_node_number (creal (r)), im));
                  }
                pair = m42_value_list_new ();
                m42_value_list_append (pair, m42_value_expr (m42_node_simplify (rule)));
                m42_node_free (rule);
                m42_value_list_append (out, pair);
              }
            return out;
          }
      }
  }

  /* Numbers would not do: there are other letters in the equation.  A
   * line and a quadratic have a closed form, and that is the answer
   * Mathematica gives -- a x + b == 0 is x -> -b/a, whatever a and b
   * turn out to be. */
  {
    g_autoptr (GPtrArray) terms = m42_node_poly_terms (lhs, var);

    if (terms != NULL && (terms->len == 2 || terms->len == 3))
      {
        M42Node *answers[2] = { NULL, NULL };
        guint how_many = 1;

        if (terms->len == 2)
          {
            /* a x + b == 0 */
            const M42Node *b = g_ptr_array_index (terms, 0);
            const M42Node *a = g_ptr_array_index (terms, 1);

            answers[0] = m42_node_binary (M42_TOK_SLASH,
                                          m42_node_unary (M42_TOK_MINUS, m42_node_copy (b)),
                                          m42_node_copy (a));
          }
        else
          {
            /* a x^2 + b x + c == 0, both roots, the lesser one first
             * the way Mathematica writes them. */
            const M42Node *c = g_ptr_array_index (terms, 0);
            const M42Node *b = g_ptr_array_index (terms, 1);
            const M42Node *a = g_ptr_array_index (terms, 2);
            M42Node *under =
              m42_node_binary (M42_TOK_MINUS,
                               m42_node_binary (M42_TOK_CARET, m42_node_copy (b),
                                                m42_node_number (2)),
                               m42_node_binary (M42_TOK_STAR, m42_node_number (4),
                                                m42_node_binary (M42_TOK_STAR,
                                                                 m42_node_copy (a),
                                                                 m42_node_copy (c))));
            M42Node *root = m42_node_call1 ("Sqrt", under);
            M42Node *twice = m42_node_binary (M42_TOK_STAR, m42_node_number (2),
                                              m42_node_copy (a));

            for (int side = 0; side < 2; side++)
              {
                M42Node *top =
                  m42_node_binary (side == 0 ? M42_TOK_MINUS : M42_TOK_PLUS,
                                   m42_node_unary (M42_TOK_MINUS, m42_node_copy (b)),
                                   m42_node_copy (root));

                answers[side] = m42_node_binary (M42_TOK_SLASH, top,
                                                 m42_node_copy (twice));
              }
            m42_node_free (root);
            m42_node_free (twice);
            how_many = 2;
          }

        for (guint i = 0; i < how_many; i++)
          {
            M42Node *rule = m42_node_new (M42_NODE_RULE);
            M42Value *pair = m42_value_list_new ();

            g_ptr_array_add (rule->children, m42_node_ident (var));
            g_ptr_array_add (rule->children, m42_node_simplify (answers[i]));
            m42_node_free (answers[i]);
            m42_value_list_append (pair, m42_value_expr (m42_node_simplify (rule)));
            m42_node_free (rule);
            m42_value_list_append (out, pair);
          }
        g_array_unref (roots);
        return out;
      }
  }

  /* Not a polynomial: sweep a window for sign changes.  An equation
   * like Sin[x] == 1/2 has roots for ever, so the window is a modest
   * one and the answer is the roots inside it. */
  prev_x = -20;
  prev_f = residual (s, f, var, prev_x);
  for (int i = 1; i <= 1600 && out->u.list->len < 24; i++)
    {
      double x = -20 + i * 0.025;
      double fx = residual (s, f, var, x);
      double root = x;
      gboolean found = FALSE;

      if (isfinite (fx) && isfinite (prev_f) &&
          (fx == 0 || (fx < 0) != (prev_f < 0)))
        {
          root = (prev_x + x) / 2;
          found = newton (s, f, var, &root) && root >= prev_x - 0.1 && root <= x + 0.1;
          if (!found)
            {
              /* Newton wandered; bisect instead. */
              double lo = prev_x, hi = x, flo = prev_f;
              for (int k = 0; k < 60; k++)
                {
                  double mid = (lo + hi) / 2, fm = residual (s, f, var, mid);
                  if ((fm < 0) == (flo < 0)) { lo = mid; flo = fm; } else hi = mid;
                }
              root = (lo + hi) / 2;
              found = fabs (residual (s, f, var, root)) < 1e-6;
            }
        }
      if (found)
        {
          gboolean dup = FALSE;
          for (guint k = 0; k < roots->len; k++)
            if (fabs (g_array_index (roots, double, k) - root) < 1e-7)
              dup = TRUE;
          if (!dup)
            {
              if (fabs (root) < 1e-12)
                root = 0;
              root = polished_root (s, f, var, root);
              g_array_append_val (roots, root);
              {
                M42Value *pair = m42_value_list_new ();
                m42_value_list_append (pair, rule_value (var, root));
                m42_value_list_append (out, pair);
              }
            }
        }
      prev_x = x;
      prev_f = fx;
    }
  g_array_unref (roots);

  /* An empty list says there are no roots, which is only true when the
   * equation was all numbers to begin with.  With another letter in it
   * -- a cubic in x with an a in front, say -- nothing was found
   * because nothing could be looked for, and the honest answer is the
   * question back again. */
  if (m42_value_list_length (out) == 0)
    {
      g_autoptr (M42Node) bare = m42_node_substitute (lhs, var, m42_node_number (1));

      if (first_symbol (bare) != NULL)
        {
          M42Node *unevaluated = m42_node_new (M42_NODE_CALL);

          unevaluated->name = g_strdup ("Solve");
          g_ptr_array_add (unevaluated->children, m42_node_copy (m42_node_child (call, 0)));
          g_ptr_array_add (unevaluated->children, m42_node_ident (var));
          m42_value_unref (out);
          return m42_value_expr (unevaluated);
        }
    }
  return out;
}

/* Factor[p]: a polynomial written as a product of its factors, found
 * from its roots.  A pair of complex roots becomes the real quadratic
 * they belong to, so the answer stays in real arithmetic. */
static M42Value *
factor (M42Session *s, const M42Node *call)
{
  const M42Node *arg = m42_node_child (call, 0);
  g_autoptr (M42Node) expr = NULL;
  g_autoptr (GArray) coeffs = g_array_new (FALSE, TRUE, sizeof (double));
  g_autoptr (GArray) roots = g_array_new (FALSE, FALSE, sizeof (double _Complex));
  const char *var = NULL;
  M42Node *out = NULL;
  double lead;

  /* The one name in the expression is the variable. */
  {
    g_autoptr (M42Value) v = eval (s, arg);
    if (is_error (v))
      return g_steal_pointer (&v);
    if (v->kind != M42_VALUE_EXPR)
      return g_steal_pointer (&v);       /* a number factors into itself */
    expr = m42_node_copy (v->u.expr);
  }
  {
    static const char *CANDIDATES[] = { "x", "y", "z", "t", "n", "a", "b", "u", "v", "w" };
    for (guint i = 0; i < G_N_ELEMENTS (CANDIDATES) && var == NULL; i++)
      if (m42_node_depends_on (expr, CANDIDATES[i]))
        var = CANDIDATES[i];
  }
  if (var == NULL || !poly_coeffs (expr, var, coeffs, 0) || coeffs->len < 2)
    return m42_value_expr (g_steal_pointer (&expr));

  lead = g_array_index (coeffs, double, coeffs->len - 1);
  if (!polynomial_roots (coeffs, roots))
    return m42_value_expr (g_steal_pointer (&expr));

  g_array_sort (roots, compare_roots);
  for (guint i = 0; i < roots->len; i++)
    {
      double _Complex r = g_array_index (roots, double _Complex, i);
      M42Node *piece;

      if (cimag (r) == 0)
        {
          /* (x - r), or x when the root is at the origin. */
          double v = creal (r);
          piece = v == 0 ? m42_node_ident (var)
                         : m42_node_binary (v > 0 ? M42_TOK_MINUS : M42_TOK_PLUS,
                                            m42_node_ident (var), m42_node_number (fabs (v)));
        }
      else
        {
          /* The pair together: x^2 - 2 Re(r) x + |r|^2. */
          double re = creal (r), mag = creal (r) * creal (r) + cimag (r) * cimag (r);
          M42Node *quad = m42_node_binary (M42_TOK_CARET, m42_node_ident (var), m42_node_number (2));

          if (re != 0)
            quad = m42_node_binary (re > 0 ? M42_TOK_MINUS : M42_TOK_PLUS, quad,
                                    m42_node_binary (M42_TOK_STAR, m42_node_number (fabs (2 * re)),
                                                     m42_node_ident (var)));
          quad = m42_node_binary (M42_TOK_PLUS, quad, m42_node_number (mag));
          piece = quad;
          i++;   /* its conjugate is the next root, and is used up here */
        }
      out = out == NULL ? piece : m42_node_binary (M42_TOK_STAR, out, piece);
    }

  if (out == NULL)
    return m42_value_expr (g_steal_pointer (&expr));
  if (lead != 1)
    out = m42_node_binary (M42_TOK_STAR, m42_node_number (lead), out);
  return expr_result (out);
}

/* --- plotting ------------------------------------------------------------- */

/* The options a plot takes, written as rules after its arguments:
 * PlotLabel -> "title", AxesLabel -> {"x", "y"}, PlotRange -> {lo, hi}. */
static M42Value *
plot_options (M42Session *s, const M42Node *call, guint from, M42Plot *p)
{
  for (guint i = from; i < call->children->len; i++)
    {
      const M42Node *opt = m42_node_child (call, i);
      const char *name;
      g_autoptr (M42Value) v = NULL;

      if (opt->kind != M42_NODE_RULE || m42_node_child (opt, 0)->kind != M42_NODE_IDENT)
        return m42_value_error ("Plot: expected an option like PlotLabel -> \"title\"");
      name = m42_node_child (opt, 0)->name;
      v = eval (s, m42_node_child (opt, 1));
      if (is_error (v))
        return m42_value_ref (v);

      if (name_is (name, "PlotLabel", "title") && v->kind == M42_VALUE_STRING)
        {
          g_free (p->title);
          p->title = g_strdup (v->u.string);
        }
      else if (name_is (name, "AxesLabel", "xlabel") &&
               v->kind == M42_VALUE_LIST && m42_value_list_length (v) == 2)
        {
          M42Value *x = m42_value_list_nth (v, 0), *y = m42_value_list_nth (v, 1);
          if (x->kind == M42_VALUE_STRING)
            {
              g_free (p->xlabel);
              p->xlabel = g_strdup (x->u.string);
            }
          if (y->kind == M42_VALUE_STRING)
            {
              g_free (p->ylabel);
              p->ylabel = g_strdup (y->u.string);
            }
        }
      else if (name_is (name, "PlotRange", "ylim") && m42_value_is_vector (v) &&
               m42_value_list_length (v) == 2)
        {
          p->ymin = m42_value_list_nth (v, 0)->u.number;
          p->ymax = m42_value_list_nth (v, 1)->u.number;
        }
      else
        return m42_value_error ("Plot: %s is not an option math42 knows", name);
    }
  return NULL;
}

/* Plot[f, {x, a, b}] and Plot[{f, g}, {x, a, b}]: 300 samples each. */
static M42Value *
plot (M42Session *s, const M42Node *call, gboolean log_x, gboolean log_y)
{
  const M42Node *fs, *spec;
  const char *var;
  double a, b;
  M42Value *err, *out;
  M42Plot *p;

  if (call->children->len < 2)
    return m42_value_error ("Plot expects an expression and {x, a, b}");
  fs = m42_node_child (call, 0);
  spec = m42_node_child (call, 1);
  if (spec->kind != M42_NODE_LIST || spec->children->len != 3 ||
      m42_node_child (spec, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("Plot expects {x, a, b} as its second argument");
  var = m42_node_child (spec, 0)->name;
  {
    g_autoptr (M42Value) va = eval (s, m42_node_child (spec, 1));
    g_autoptr (M42Value) vb = eval (s, m42_node_child (spec, 2));
    if (!need_number (va, "Plot", &a, &err) || !need_number (vb, "Plot", &b, &err))
      return err;
  }

  out = m42_value_plot_new ();
  p = out->u.plot;
  p->xlabel = g_strdup (var);
  {
    guint count = fs->kind == M42_NODE_LIST ? fs->children->len : 1;
    for (guint k = 0; k < count; k++)
      {
        const M42Node *f = fs->kind == M42_NODE_LIST ? m42_node_child (fs, k) : fs;
        M42Series *series = m42_plot_add_series (p, M42_SERIES_LINE);
        for (int i = 0; i <= 300; i++)
          {
            double x = a + (b - a) * i / 300.0;
            m42_series_add_point (series, x, number_at (s, f, var, x));
          }
      }
  }
  /* A logarithmic axis holds the powers, and the renderer writes the
   * numbers they stand for. */
  if (log_y || log_x)
    for (guint i = 0; i < p->series->len; i++)
      {
        M42Series *series = g_ptr_array_index (p->series, i);

        for (guint k = 0; k + 1 < series->points->len; k += 2)
          {
            double *x = &g_array_index (series->points, double, k);
            double *y = &g_array_index (series->points, double, k + 1);

            if (log_x)
              *x = *x > 0 ? log10 (*x) : NAN;
            if (log_y)
              *y = *y > 0 ? log10 (*y) : NAN;
          }
      }
  p->log_x = log_x;
  p->log_y = log_y;

  m42_plot_autoscale (p);
  if (!log_x)
    {
      p->xmin = a;
      p->xmax = b;
    }
  {
    M42Value *bad = plot_options (s, call, 2, p);
    if (bad != NULL)
      {
        m42_value_unref (out);
        return bad;
      }
  }
  return out;
}

/* ParametricPlot[{fx, fy}, {t, a, b}] and PolarPlot[r, {t, a, b}]:
 * the curve a moving point traces. */
static M42Value *
parametric_plot (M42Session *s, const M42Node *call, gboolean polar)
{
  const char *who = polar ? "PolarPlot" : "ParametricPlot";
  const M42Node *fs, *spec, *fx = NULL, *fy = NULL;
  const char *var;
  double a, b;
  M42Value *err, *out;
  M42Plot *p;
  M42Series *series;

  if (call->children->len < 2)
    return m42_value_error ("%s expects a curve and {t, a, b}", who);
  fs = m42_node_child (call, 0);
  spec = m42_node_child (call, 1);
  if (!polar)
    {
      if (fs->kind != M42_NODE_LIST || fs->children->len != 2)
        return m42_value_error ("ParametricPlot expects {x(t), y(t)}");
      fx = m42_node_child (fs, 0);
      fy = m42_node_child (fs, 1);
    }
  if (spec->kind != M42_NODE_LIST || spec->children->len != 3 ||
      m42_node_child (spec, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("%s expects {t, a, b} as its second argument", who);
  var = m42_node_child (spec, 0)->name;
  {
    g_autoptr (M42Value) va = eval (s, m42_node_child (spec, 1));
    g_autoptr (M42Value) vb = eval (s, m42_node_child (spec, 2));
    if (!need_number (va, who, &a, &err) || !need_number (vb, who, &b, &err))
      return err;
  }

  out = m42_value_plot_new ();
  p = out->u.plot;
  series = m42_plot_add_series (p, M42_SERIES_LINE);
  for (int i = 0; i <= 600; i++)
    {
      double t = a + (b - a) * i / 600.0;

      if (polar)
        {
          double r = number_at (s, fs, var, t);
          m42_series_add_point (series, r * cos (t), r * sin (t));
        }
      else
        m42_series_add_point (series, number_at (s, fx, var, t), number_at (s, fy, var, t));
    }
  m42_plot_autoscale (p);
  {
    M42Value *bad = plot_options (s, call, 2, p);
    if (bad != NULL)
      {
        m42_value_unref (out);
        return bad;
      }
  }
  return out;
}

/* f(x, y) at a point, for a surface. */
static double
surface_at (M42Session *s, const M42Node *f, const char *xvar, double x,
            const char *yvar, double y)
{
  return number_at2 (s, f, xvar, x, yvar, y);
}

/* Plot3D[f, {x, a, b}, {y, c, d}]: the surface over a grid, which the
 * notebook draws in projection.  MATLAB's surf and mesh land here too. */
static M42Value *
plot3d (M42Session *s, const M42Node *call, gboolean flat)
{
  const M42Node *xs, *ys;
  const char *xvar, *yvar;
  double x0, x1, y0, y1;
  M42Value *err, *out;
  M42Plot *p;
  M42Surface *surface;
  const guint n = 44;

  if (call->children->len < 3)
    return m42_value_error ("Plot3D expects an expression, {x, a, b} and {y, c, d}");
  xs = m42_node_child (call, 1);
  ys = m42_node_child (call, 2);
  if (xs->kind != M42_NODE_LIST || xs->children->len != 3 ||
      ys->kind != M42_NODE_LIST || ys->children->len != 3 ||
      m42_node_child (xs, 0)->kind != M42_NODE_IDENT ||
      m42_node_child (ys, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("Plot3D expects {x, a, b} and {y, c, d}");
  xvar = m42_node_child (xs, 0)->name;
  yvar = m42_node_child (ys, 0)->name;
  {
    g_autoptr (M42Value) a = eval (s, m42_node_child (xs, 1));
    g_autoptr (M42Value) b = eval (s, m42_node_child (xs, 2));
    g_autoptr (M42Value) c = eval (s, m42_node_child (ys, 1));
    g_autoptr (M42Value) d = eval (s, m42_node_child (ys, 2));

    if (!need_number (a, "Plot3D", &x0, &err) || !need_number (b, "Plot3D", &x1, &err) ||
        !need_number (c, "Plot3D", &y0, &err) || !need_number (d, "Plot3D", &y1, &err))
      return err;
  }

  out = m42_value_plot_new ();
  p = out->u.plot;
  p->xlabel = g_strdup (xvar);
  p->ylabel = g_strdup (yvar);
  surface = m42_plot_add_surface (p, n, n);
  surface->xmin = x0;
  surface->xmax = x1;
  surface->ymin = y0;
  surface->ymax = y1;

  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      {
        double x = x0 + (x1 - x0) * i / (double) (n - 1);
        double y = y0 + (y1 - y0) * j / (double) (n - 1);
        surface->z[i * n + j] = surface_at (s, m42_node_child (call, 0), xvar, x, yvar, y);
      }
  surface->flat = flat;
  m42_surface_autoscale (surface);
  p->xmin = x0;
  p->xmax = x1;
  p->ymin = y0;
  p->ymax = y1;
  {
    M42Value *bad = plot_options (s, call, 3, p);
    if (bad != NULL)
      {
        m42_value_unref (out);
        return bad;
      }
  }
  return out;
}

/* --- the Fourier transform -----------------------------------------------
 *
 * The table a course hands out, written as the rules it is:
 *
 *   Exp[-a |t|]    Sqrt[2/Pi] a/(a^2 + w^2)
 *   Exp[-a t^2]    Exp[-w^2/(4 a)]/Sqrt[2 a]
 *   1/(a^2 + t^2)  Sqrt[Pi/2] Exp[-a |w|]/a
 *   1              Sqrt[2 Pi] DiracDelta[w]
 *   Cos[a t]       Sqrt[Pi/2] (DiracDelta[w - a] + DiracDelta[w + a])
 *   Sin[a t]       I Sqrt[Pi/2] (DiracDelta[w + a] - DiracDelta[w - a])
 *
 * with the convention Mathematica uses, F(w) = 1/Sqrt[2 Pi] INT f(t)
 * e^(-i w t) dt, which is where every Sqrt[2 Pi] below comes from.
 *
 * The rules are matched with the same matcher /. uses, so the table
 * reads as the mathematics it is rather than as a tree walk.  The
 * letter a in a rule must not itself hold the variable, which the
 * matcher cannot say and is checked after it.
 */
/* One word swapped for another, everywhere it appears. */
static char *
with_name (const char *text, const char *mark, const char *name)
{
  g_auto (GStrv) pieces = g_strsplit (text, mark, -1);

  return g_strjoinv (name, pieces);
}

static M42Value *
fourier_transform (M42Session *s, const M42Node *call, gboolean inverse)
{
  static const char *const TABLE[] = {
    /* A number in front of the variable is folded into the number by
     * the time the rule sees it, so these are written the way they
     * arrive -- with A negative, which the condition says. */
    "Exp[A_ Abs[%t]] /; A < 0 -> Sqrt[2/Pi] (-A)/(A^2 + %w^2)",
    "Exp[-Abs[%t]] -> Sqrt[2/Pi]/(1 + %w^2)",
    "Exp[A_ %t^2] /; A < 0 -> Exp[%w^2/(4 A)]/Sqrt[-2 A]",
    "Exp[-%t^2] -> Exp[-%w^2/4]/Sqrt[2]",
    "1/(A_ + %t^2) -> Sqrt[Pi/2] Exp[-Sqrt[A] Abs[%w]]/Sqrt[A]",
    "Cos[A_ %t] -> Sqrt[Pi/2] (DiracDelta[%w - A] + DiracDelta[%w + A])",
    "Sin[A_ %t] -> I Sqrt[Pi/2] (DiracDelta[%w + A] - DiracDelta[%w - A])",
    "Cos[%t] -> Sqrt[Pi/2] (DiracDelta[%w - 1] + DiracDelta[%w + 1])",
    "Sin[%t] -> I Sqrt[Pi/2] (DiracDelta[%w + 1] - DiracDelta[%w - 1])",
    "DiracDelta[%t] -> 1/Sqrt[2 Pi]",
  };
  const char *who = inverse ? "InverseFourierTransform" : "FourierTransform";
  const M42Node *from, *to;
  g_autoptr (M42Node) f = NULL;

  if (call->children->len != 3)
    return m42_value_error ("%s expects f, the name it is in, and the name to give", who);
  from = m42_node_child (call, 1);
  to = m42_node_child (call, 2);
  if (from->kind != M42_NODE_IDENT || to->kind != M42_NODE_IDENT)
    return m42_value_error ("%s expects two names", who);
  f = symbolic_argument (s, m42_node_child (call, 0), from->name);

  for (guint i = 0; i < G_N_ELEMENTS (TABLE); i++)
    {
      g_autofree char *with_t = NULL;
      g_autofree char *text = NULL;
      g_autofree char *complaint = NULL;
      g_autoptr (M42Node) rule = NULL;
      g_autoptr (GHashTable) names = m42_pattern_names_new ();
      const M42Node *use;

      /* The inverse reads the same table the other way round, which is
       * what makes it an inverse. */
      with_t = with_name (TABLE[i], "%t", inverse ? to->name : from->name);
      text = with_name (with_t, "%w", inverse ? from->name : to->name);
      rule = m42_parse (text, &complaint);
      use = rule;
      while (use != NULL && use->kind == M42_NODE_SEQ && use->children->len == 1)
        use = m42_node_child (use, 0);
      if (use == NULL || use->kind != M42_NODE_RULE)
        continue;

      {
        const M42Node *shape = m42_node_child (use, inverse ? 1 : 0);
        const M42Node *gives = m42_node_child (use, inverse ? 0 : 1);

        /* Read backwards, the side with the condition on it is the
         * answer, and a condition is not part of an answer. */
        if (gives->kind == M42_NODE_CONDITION)
          gives = m42_node_child (gives, 0);
        if (shape->kind == M42_NODE_CONDITION && inverse)
          shape = m42_node_child (shape, 0);

        if (!m42_node_match (shape, f, names, pattern_test, s))
          continue;
        /* A letter in the rule that swallowed the variable itself has
         * matched something the rule did not mean. */
        {
          GHashTableIter iter;
          gpointer key, value;
          gboolean honest = TRUE;

          g_hash_table_iter_init (&iter, names);
          while (g_hash_table_iter_next (&iter, &key, &value))
            if (m42_node_depends_on (value, inverse ? to->name : from->name))
              honest = FALSE;
          if (!honest)
            continue;
        }
        {
          g_autoptr (M42Node) filled = m42_node_bind (gives, names);

          return expr_result (m42_node_simplify (filled));
        }
      }
    }
  return m42_value_error ("%s: that one is not in the table", who);
}

/* VectorPlot[{fx, fy}, {x, a, b}, {y, c, d}]: which way a field points
 * at each place on a grid, which is what a direction field is.  Every
 * arrow is drawn the same length but for a little, so that the picture
 * says which way rather than how far -- how far is said by the colour
 * and by a shorter arrow where the field is weak.
 */
static M42Value *
vector_plot (M42Session *s, const M42Node *call)
{
  const M42Node *parts, *xs, *ys;
  const char *xvar, *yvar;
  double x0, x1, y0, y1;
  M42Value *err, *out;
  M42Plot *p;
  const guint n = 17;
  g_autofree double *dx = NULL;
  g_autofree double *dy = NULL;
  double longest = 0;

  if (call->children->len < 3)
    return m42_value_error ("VectorPlot expects {p, q}, {x, a, b} and {y, c, d}");
  parts = m42_node_child (call, 0);
  xs = m42_node_child (call, 1);
  ys = m42_node_child (call, 2);
  if (parts->kind != M42_NODE_LIST || parts->children->len != 2)
    return m42_value_error ("VectorPlot expects two expressions in a list");
  if (xs->kind != M42_NODE_LIST || xs->children->len != 3 ||
      ys->kind != M42_NODE_LIST || ys->children->len != 3 ||
      m42_node_child (xs, 0)->kind != M42_NODE_IDENT ||
      m42_node_child (ys, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("VectorPlot expects {x, a, b} and {y, c, d}");
  xvar = m42_node_child (xs, 0)->name;
  yvar = m42_node_child (ys, 0)->name;
  {
    g_autoptr (M42Value) a = eval (s, m42_node_child (xs, 1));
    g_autoptr (M42Value) b = eval (s, m42_node_child (xs, 2));
    g_autoptr (M42Value) c = eval (s, m42_node_child (ys, 1));
    g_autoptr (M42Value) d = eval (s, m42_node_child (ys, 2));

    if (!need_number (a, "VectorPlot", &x0, &err) || !need_number (b, "VectorPlot", &x1, &err) ||
        !need_number (c, "VectorPlot", &y0, &err) || !need_number (d, "VectorPlot", &y1, &err))
      return err;
  }

  dx = g_new (double, n * n);
  dy = g_new (double, n * n);
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      {
        double x = x0 + (x1 - x0) * i / (double) (n - 1);
        double y = y0 + (y1 - y0) * j / (double) (n - 1);
        double u = number_at2 (s, m42_node_child (parts, 0), xvar, x, yvar, y);
        double v = number_at2 (s, m42_node_child (parts, 1), xvar, x, yvar, y);

        dx[i * n + j] = u;
        dy[i * n + j] = v;
        if (isfinite (u) && isfinite (v))
          longest = MAX (longest, hypot (u, v));
      }
  if (longest <= 0)
    return m42_value_error ("VectorPlot: the field is nothing everywhere");

  out = m42_value_plot_new ();
  p = out->u.plot;
  p->xmin = x0;
  p->xmax = x1;
  p->ymin = y0;
  p->ymax = y1;
  p->xlabel = g_strdup (xvar);
  p->ylabel = g_strdup (yvar);

  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      {
        double u = dx[i * n + j], v = dy[i * n + j];
        double x = x0 + (x1 - x0) * i / (double) (n - 1);
        double y = y0 + (y1 - y0) * j / (double) (n - 1);
        double len = hypot (u, v);
        double strength = len / longest;
        double step = MIN ((x1 - x0), (y1 - y0)) / (n - 1);
        double want;

        if (!isfinite (u) || !isfinite (v) || len == 0)
          continue;
        /* Long enough to see which way it points, and a little longer
         * where the field is stronger -- but never long enough to run
         * into the next arrow. */
        want = step * 0.85 * (0.45 + 0.55 * sqrt (strength));
        m42_plot_add_arrow (p, x, y, u / len * want, v / len * want, strength);
      }
  {
    M42Value *bad = plot_options (s, call, 3, p);

    if (bad != NULL)
      {
        m42_value_unref (out);
        return bad;
      }
  }
  return out;
}

/* ParametricPlot3D[{fx, fy, fz}, {t, a, b}]: where a point goes as t
 * runs, drawn in the same projection the surfaces use. */
static M42Value *
parametric_plot3d (M42Session *s, const M42Node *call)
{
  const M42Node *parts, *spec;
  const char *var;
  double t0, t1;
  M42Value *err, *out;
  M42Curve3D *curve;
  const guint n = 600;

  if (call->children->len < 2)
    return m42_value_error ("ParametricPlot3D expects {x, y, z} and {t, a, b}");
  parts = m42_node_child (call, 0);
  spec = m42_node_child (call, 1);
  if (parts->kind != M42_NODE_LIST || parts->children->len != 3)
    return m42_value_error ("ParametricPlot3D expects three expressions in a list");
  if (spec->kind != M42_NODE_LIST || spec->children->len != 3 ||
      m42_node_child (spec, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("ParametricPlot3D expects {t, a, b}");
  var = m42_node_child (spec, 0)->name;
  {
    g_autoptr (M42Value) a = eval (s, m42_node_child (spec, 1));
    g_autoptr (M42Value) b = eval (s, m42_node_child (spec, 2));

    if (!need_number (a, "ParametricPlot3D", &t0, &err) ||
        !need_number (b, "ParametricPlot3D", &t1, &err))
      return err;
  }
  if (!(t1 > t0))
    return m42_value_error ("ParametricPlot3D: the second end must be past the first");

  out = m42_value_plot_new ();
  curve = m42_plot_add_curve3d (out->u.plot);
  for (guint i = 0; i < n; i++)
    {
      double t = t0 + (t1 - t0) * i / (double) (n - 1);
      double x = number_at (s, m42_node_child (parts, 0), var, t);
      double y = number_at (s, m42_node_child (parts, 1), var, t);
      double z = number_at (s, m42_node_child (parts, 2), var, t);

      m42_curve3d_add_point (curve, x, y, z);
    }
  if (curve->points->len < 6)
    {
      m42_value_unref (out);
      return m42_value_error ("ParametricPlot3D: nothing came of that");
    }
  {
    M42Value *bad = plot_options (s, call, 2, out->u.plot);

    if (bad != NULL)
      {
        m42_value_unref (out);
        return bad;
      }
  }
  return out;
}

/* ListPlot3D, ListContourPlot and ListDensityPlot: the same three
 * pictures, but handed the grid of heights instead of a function to
 * work one out from.  The places run 1, 2, 3 across and down, as they
 * do in both languages. */
static M42Value *
list_grid_plot (const M42Value *data, const char *name, gboolean as_contours,
                gboolean flat)
{
  guint rows, cols;
  g_autofree double *z = NULL;
  double lowest = INFINITY, highest = -INFINITY;
  M42Value *out;
  M42Plot *p;

  if (!m42_value_is_matrix (data, &rows, &cols) || rows < 2 || cols < 2)
    return m42_value_error ("%s wants a grid of heights, two or more each way", name);

  /* z is held the way a surface holds it: the first index across. */
  z = g_new (double, rows * cols);
  for (guint i = 0; i < cols; i++)
    for (guint j = 0; j < rows; j++)
      {
        double v;

        if (!value_number (m42_value_list_nth (m42_value_list_nth (data, j), i), &v))
          return m42_value_error ("%s wants numbers", name);
        z[i * rows + j] = v;
        lowest = MIN (lowest, v);
        highest = MAX (highest, v);
      }
  if (!(highest > lowest))
    return m42_value_error ("%s: every height is the same", name);

  out = m42_value_plot_new ();
  p = out->u.plot;
  p->xmin = 1;
  p->xmax = cols;
  p->ymin = 1;
  p->ymax = rows;

  if (as_contours)
    {
      /* The marching squares want a square grid; a rectangle is walked
       * the same way with the two counts kept apart, so the grid is
       * squared off by using the smaller count. */
      guint n = MIN (rows, cols);
      g_autofree double *square = g_new (double, n * n);

      for (guint i = 0; i < n; i++)
        for (guint j = 0; j < n; j++)
          square[i * n + j] = z[i * rows + j];
      add_contours (p, square, n, 1, n, 1, n, lowest, highest, 9);
      p->xmax = n;
      p->ymax = n;
      return out;
    }

  {
    M42Surface *surface = m42_plot_add_surface (p, cols, rows);

    memcpy (surface->z, z, sizeof (double) * rows * cols);
    surface->xmin = 1;
    surface->xmax = cols;
    surface->ymin = 1;
    surface->ymax = rows;
    surface->flat = flat;
    m42_surface_autoscale (surface);
  }
  return out;
}

/* The curves along which a grid of values keeps the same height, found
 * by walking it square by square and joining where the level crosses
 * each edge -- the method that goes by the name of marching squares.
 * ContourPlot works a function out over the grid first; ListContourPlot
 * is handed the grid. */
static void
add_contours (M42Plot *p, const double *z, guint n, double x0, double x1,
              double y0, double y1, double lowest, double highest, guint levels)
{
  for (guint k = 0; k < levels; k++)
    {
      double level = lowest + (highest - lowest) * (k + 1) / (levels + 1);
      M42Contour *contour = m42_plot_add_contour (p, level, k, levels);

      for (guint i = 0; i + 1 < n; i++)
        for (guint j = 0; j + 1 < n; j++)
          {
            /* The four corners of one square, and where the level
             * crosses its edges. */
            double corner[4] = { z[i * n + j], z[(i + 1) * n + j],
                                 z[(i + 1) * n + j + 1], z[i * n + j + 1] };
            double cx[4], cy[4];
            double left = x0 + (x1 - x0) * i / (double) (n - 1);
            double right = x0 + (x1 - x0) * (i + 1) / (double) (n - 1);
            double bottom = y0 + (y1 - y0) * j / (double) (n - 1);
            double top = y0 + (y1 - y0) * (j + 1) / (double) (n - 1);
            double px[2], py[2];
            guint found = 0;
            gboolean whole = TRUE;

            for (int c = 0; c < 4; c++)
              if (!isfinite (corner[c]))
                whole = FALSE;
            if (!whole)
              continue;

            cx[0] = left;   cy[0] = bottom;
            cx[1] = right;  cy[1] = bottom;
            cx[2] = right;  cy[2] = top;
            cx[3] = left;   cy[3] = top;

            for (int e = 0; e < 4 && found < 2; e++)
              {
                int a = e, b = (e + 1) % 4;
                double va = corner[a], vb = corner[b];

                if ((va < level) == (vb < level))
                  continue;
                {
                  double t = (level - va) / (vb - va);

                  px[found] = cx[a] + (cx[b] - cx[a]) * t;
                  py[found] = cy[a] + (cy[b] - cy[a]) * t;
                  found++;
                }
              }
            if (found == 2)
              m42_contour_add_segment (contour, px[0], py[0], px[1], py[1]);
          }
    }
}

/* ContourPlot[f, {x, a, b}, {y, c, d}]: the curves along which f keeps
 * the same value, found by walking the grid square by square and
 * joining where the level crosses each edge -- the method that goes by
 * the name of marching squares. */
static M42Value *
contour_plot (M42Session *s, const M42Node *call)
{
  const M42Node *xs, *ys;
  const char *xvar, *yvar;
  double x0, x1, y0, y1;
  M42Value *err, *out;
  M42Plot *p;
  const guint n = 60;
  const guint levels = 9;
  g_autofree double *z = NULL;
  double lowest = INFINITY, highest = -INFINITY;

  if (call->children->len < 3)
    return m42_value_error ("ContourPlot expects an expression, {x, a, b} and {y, c, d}");
  xs = m42_node_child (call, 1);
  ys = m42_node_child (call, 2);
  if (xs->kind != M42_NODE_LIST || xs->children->len != 3 ||
      ys->kind != M42_NODE_LIST || ys->children->len != 3 ||
      m42_node_child (xs, 0)->kind != M42_NODE_IDENT ||
      m42_node_child (ys, 0)->kind != M42_NODE_IDENT)
    return m42_value_error ("ContourPlot expects {x, a, b} and {y, c, d}");
  xvar = m42_node_child (xs, 0)->name;
  yvar = m42_node_child (ys, 0)->name;
  {
    g_autoptr (M42Value) a = eval (s, m42_node_child (xs, 1));
    g_autoptr (M42Value) b = eval (s, m42_node_child (xs, 2));
    g_autoptr (M42Value) c = eval (s, m42_node_child (ys, 1));
    g_autoptr (M42Value) d = eval (s, m42_node_child (ys, 2));

    if (!need_number (a, "ContourPlot", &x0, &err) || !need_number (b, "ContourPlot", &x1, &err) ||
        !need_number (c, "ContourPlot", &y0, &err) || !need_number (d, "ContourPlot", &y1, &err))
      return err;
  }

  z = g_new (double, n * n);
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      {
        double x = x0 + (x1 - x0) * i / (double) (n - 1);
        double y = y0 + (y1 - y0) * j / (double) (n - 1);
        double v = number_at2 (s, m42_node_child (call, 0), xvar, x, yvar, y);

        z[i * n + j] = v;
        if (isfinite (v))
          {
            lowest = MIN (lowest, v);
            highest = MAX (highest, v);
          }
      }
  if (!isfinite (lowest) || highest <= lowest)
    return m42_value_error ("ContourPlot: the function does not change over that square");

  out = m42_value_plot_new ();
  p = out->u.plot;
  p->xmin = x0;
  p->xmax = x1;
  p->ymin = y0;
  p->ymax = y1;
  p->xlabel = g_strdup (xvar);
  p->ylabel = g_strdup (yvar);

  add_contours (p, z, n, x0, x1, y0, y1, lowest, highest, levels);

  {
    M42Value *bad = plot_options (s, call, 3, p);

    if (bad != NULL)
      {
        m42_value_unref (out);
        return bad;
      }
  }
  return out;
}

/* ListPlot[{y1, y2, ...}] or ListPlot[{{x, y}, ...}], and one series
 * per list when given a list of such lists. */
static M42Value *
list_plot (GPtrArray *args, M42SeriesKind kind)
{
  M42Value *out = m42_value_plot_new ();
  M42Plot *p = out->u.plot;

  for (guint k = 0; k < args->len; k++)
    {
      M42Value *data = g_ptr_array_index (args, k);
      guint n;

      if (data->kind != M42_VALUE_LIST || m42_value_list_length (data) == 0)
        {
          m42_value_unref (out);
          return m42_value_error ("ListPlot expects a list of numbers or of {x, y} pairs");
        }
      n = m42_value_list_length (data);

      if (m42_value_is_vector (data))
        {
          M42Series *series = m42_plot_add_series (p, kind);
          for (guint i = 0; i < n; i++)
            m42_series_add_point (series, i + 1, m42_value_list_nth (data, i)->u.number);
        }
      else
        {
          M42Value *first = m42_value_list_nth (data, 0);
          gboolean pairs = m42_value_is_vector (first) && m42_value_list_length (first) == 2;

          if (pairs)
            {
              M42Series *series = m42_plot_add_series (p, kind);
              for (guint i = 0; i < n; i++)
                {
                  M42Value *pt = m42_value_list_nth (data, i);
                  if (!m42_value_is_vector (pt) || m42_value_list_length (pt) != 2)
                    {
                      m42_value_unref (out);
                      return m42_value_error ("ListPlot expects {x, y} pairs");
                    }
                  m42_series_add_point (series, m42_value_list_nth (pt, 0)->u.number,
                                        m42_value_list_nth (pt, 1)->u.number);
                }
            }
          else
            {
              /* A list of datasets. */
              GPtrArray *inner = g_ptr_array_new ();
              M42Value *r;
              for (guint i = 0; i < n; i++)
                g_ptr_array_add (inner, m42_value_list_nth (data, i));
              r = list_plot (inner, kind);
              g_ptr_array_unref (inner);
              m42_value_unref (out);
              if (is_error (r))
                return r;
              out = r;
              p = out->u.plot;
            }
        }
    }
  m42_plot_autoscale (p);
  return out;
}

/* Histogram[data]: the range split into bins by Sturges's rule, each
 * bin a bar as wide as it is. */
static M42Value *
histogram (GPtrArray *args)
{
  g_autoptr (GArray) xs = g_array_new (FALSE, FALSE, sizeof (double));
  M42Value *out;
  M42Plot *p;
  M42Series *series;
  double lo = INFINITY, hi = -INFINITY, width;
  int bins;
  g_autofree int *count = NULL;

  if (!collect_numbers (args, xs) || xs->len == 0)
    return m42_value_error ("Histogram expects a list of numbers");

  for (guint i = 0; i < xs->len; i++)
    {
      double x = g_array_index (xs, double, i);
      lo = MIN (lo, x);
      hi = MAX (hi, x);
    }
  if (hi == lo)
    {
      lo -= 0.5;
      hi += 0.5;
    }
  bins = CLAMP ((int) ceil (log2 (xs->len) + 1), 1, 60);
  width = (hi - lo) / bins;
  count = g_new0 (int, bins);
  for (guint i = 0; i < xs->len; i++)
    {
      int b = (int) ((g_array_index (xs, double, i) - lo) / width);
      count[CLAMP (b, 0, bins - 1)]++;
    }

  out = m42_value_plot_new ();
  p = out->u.plot;
  series = m42_plot_add_series (p, M42_SERIES_BARS);
  series->width = width;
  for (int i = 0; i < bins; i++)
    m42_series_add_point (series, lo + width * (i + 0.5), count[i]);
  m42_plot_autoscale (p);
  p->xmin = lo - width * 0.1;
  p->xmax = hi + width * 0.1;
  p->ymin = 0;
  return out;
}

/* The same options as Plot takes, but read from values rather than
 * from the tree: ListPlot and its family have their arguments in hand
 * by the time they are called. */
static gboolean
is_rule_value (const M42Value *v, const char **name, const M42Node **rhs)
{
  if (v->kind != M42_VALUE_EXPR || v->u.expr->kind != M42_NODE_RULE ||
      m42_node_child (v->u.expr, 0)->kind != M42_NODE_IDENT)
    return FALSE;
  if (name != NULL)
    *name = m42_node_child (v->u.expr, 0)->name;
  if (rhs != NULL)
    *rhs = m42_node_child (v->u.expr, 1);
  return TRUE;
}

static M42Value *
apply_value_options (GPtrArray *args, guint from, M42Plot *p)
{
  for (guint i = from; i < args->len; i++)
    {
      const char *name;
      const M42Node *rhs;

      if (!is_rule_value (ARG (i), &name, &rhs))
        return m42_value_error ("A plot takes options like PlotLabel -> \"title\" after its data");

      if (name_is (name, "PlotLabel", "title") && rhs->kind == M42_NODE_STRING)
        {
          g_free (p->title);
          p->title = g_strdup (rhs->name);
        }
      else if (name_is (name, "AxesLabel", "xlabel") && rhs->kind == M42_NODE_LIST &&
               rhs->children->len == 2)
        {
          if (m42_node_child (rhs, 0)->kind == M42_NODE_STRING)
            {
              g_free (p->xlabel);
              p->xlabel = g_strdup (m42_node_child (rhs, 0)->name);
            }
          if (m42_node_child (rhs, 1)->kind == M42_NODE_STRING)
            {
              g_free (p->ylabel);
              p->ylabel = g_strdup (m42_node_child (rhs, 1)->name);
            }
        }
      else if (name_is (name, "PlotRange", "ylim") && rhs->kind == M42_NODE_LIST &&
               rhs->children->len == 2 &&
               m42_node_child (rhs, 0)->kind == M42_NODE_NUMBER &&
               m42_node_child (rhs, 1)->kind == M42_NODE_NUMBER)
        {
          p->ymin = m42_node_child (rhs, 0)->number;
          p->ymax = m42_node_child (rhs, 1)->number;
        }
      else
        return m42_value_error ("A plot does not know the option %s", name);
    }
  return NULL;
}

/* The data arguments, which stop where the options begin. */
static GPtrArray *
data_arguments (GPtrArray *args, guint *options_from)
{
  GPtrArray *data = g_ptr_array_new ();

  *options_from = args->len;
  for (guint i = 0; i < args->len; i++)
    {
      if (is_rule_value (ARG (i), NULL, NULL))
        {
          *options_from = i;
          break;
        }
      g_ptr_array_add (data, ARG (i));
    }
  return data;
}

/* Runs one of the list plots over its data, then its options. */
static M42Value *
list_plot_with_options (GPtrArray *args, M42SeriesKind kind, gboolean matlab)
{
  guint from;
  GPtrArray *data = data_arguments (args, &from);
  M42Value *out;

  if (data->len == 0)
    {
      g_ptr_array_unref (data);
      return m42_value_error ("A plot needs something to draw");
    }
  out = kind == M42_SERIES_BARS && matlab ? histogram (data)
        : matlab ? plot_matlab (data)
                 : list_plot (data, kind);
  g_ptr_array_unref (data);

  if (out->kind == M42_VALUE_PLOT)
    {
      M42Value *bad = apply_value_options (args, from, out->u.plot);
      if (bad != NULL)
        {
          m42_value_unref (out);
          return bad;
        }
    }
  return out;
}

/* Show[p, q]: the graphs laid over one another, which is how two
 * curves from different calls end up on one picture. */
static M42Value *
show_plots (GPtrArray *args)
{
  M42Value *out = m42_value_plot_new ();
  M42Plot *p = out->u.plot;
  gboolean first = TRUE;

  for (guint i = 0; i < args->len; i++)
    {
      M42Plot *from;

      if (ARG (i)->kind != M42_VALUE_PLOT)
        {
          m42_value_unref (out);
          return m42_value_error ("Show wants graphs to lay over one another");
        }
      from = ARG (i)->u.plot;

      for (guint k = 0; k < from->series->len; k++)
        {
          M42Series *source = g_ptr_array_index (from->series, k);
          M42Series *copy = m42_plot_add_series (p, source->kind);

          /* The colour is left as the palette gave it here, so that
           * curves from two calls are told apart rather than both
           * arriving in the first colour. */
          copy->width = source->width;
          for (guint j = 0; j + 1 < source->points->len; j += 2)
            m42_series_add_point (copy, g_array_index (source->points, double, j),
                                  g_array_index (source->points, double, j + 1));
        }

      if (first || from->xmin < p->xmin)
        p->xmin = from->xmin;
      if (first || from->xmax > p->xmax)
        p->xmax = from->xmax;
      if (first || from->ymin < p->ymin)
        p->ymin = from->ymin;
      if (first || from->ymax > p->ymax)
        p->ymax = from->ymax;
      if (p->title == NULL && from->title != NULL)
        p->title = g_strdup (from->title);
      if (p->xlabel == NULL && from->xlabel != NULL)
        p->xlabel = g_strdup (from->xlabel);
      if (p->ylabel == NULL && from->ylabel != NULL)
        p->ylabel = g_strdup (from->ylabel);
      p->log_x = p->log_x || from->log_x;
      p->log_y = p->log_y || from->log_y;
      first = FALSE;
    }
  return out;
}

/* MATLAB: plot(y), plot(x, y), plot(x1, y1, x2, y2, ...). */
static M42Value *
plot_matlab (GPtrArray *args)
{
  M42Value *out = m42_value_plot_new ();
  M42Plot *p = out->u.plot;

  if (args->len == 1)
    {
      m42_value_unref (out);
      return list_plot (args, M42_SERIES_LINE);
    }
  for (guint k = 0; k + 1 < args->len; k += 2)
    {
      M42Value *xs = g_ptr_array_index (args, k), *ys = g_ptr_array_index (args, k + 1);
      M42Series *series;

      if (!m42_value_is_vector (xs) || !m42_value_is_vector (ys) ||
          m42_value_list_length (xs) != m42_value_list_length (ys))
        {
          m42_value_unref (out);
          return m42_value_error ("plot expects x and y vectors of the same length");
        }
      series = m42_plot_add_series (p, M42_SERIES_LINE);
      for (guint i = 0; i < m42_value_list_length (xs); i++)
        m42_series_add_point (series, m42_value_list_nth (xs, i)->u.number,
                              m42_value_list_nth (ys, i)->u.number);
    }
  m42_plot_autoscale (p);
  return out;
}

/* --- builtins over evaluated arguments -------------------------------------- */

static double d_add (double a, double b) { return a + b; }
static double d_mul (double a, double b) { return a * b; }
static double d_max (double a, double b) { return a > b ? a : b; }
static double d_min (double a, double b) { return a < b ? a : b; }

/* Every number in the arguments, lists flattened, in order. */
static gboolean
collect_numbers (GPtrArray *args, GArray *out)
{
  for (guint i = 0; i < args->len; i++)
    {
      M42Value *v = ARG (i);
      if (is_num (v))
        g_array_append_val (out, v->u.number);
      else if (v->kind == M42_VALUE_LIST)
        {
          GPtrArray *inner = g_ptr_array_new ();
          gboolean ok;
          for (guint j = 0; j < m42_value_list_length (v); j++)
            g_ptr_array_add (inner, m42_value_list_nth (v, j));
          ok = collect_numbers (inner, out);
          g_ptr_array_unref (inner);
          if (!ok)
            return FALSE;
        }
      else
        return FALSE;
    }
  return TRUE;
}

/* Everything the arguments hold, as values rather than doubles, so
 * that a fold can keep an exact number exact.  A list is opened out,
 * as collect_numbers opens it. */
static gboolean
collect_values (GPtrArray *args, GPtrArray *out)
{
  for (guint i = 0; i < args->len; i++)
    {
      M42Value *v = g_ptr_array_index (args, i);

      if (v->kind == M42_VALUE_LIST)
        {
          GPtrArray *inner = g_ptr_array_new ();
          gboolean ok;

          for (guint k = 0; k < m42_value_list_length (v); k++)
            g_ptr_array_add (inner, m42_value_list_nth (v, k));
          ok = collect_values (inner, out);
          g_ptr_array_unref (inner);
          if (!ok)
            return FALSE;
        }
      else if (is_num (v) || v->kind == M42_VALUE_BIGINT)
        g_ptr_array_add (out, v);
      else
        return FALSE;
    }
  return TRUE;
}

/* Adding up or multiplying together, through the arithmetic that keeps
 * an exact number exact: Total[{1/2, 1/3}] is 5/6 and not 0.8333.
 * Max and Min hand back the value that won rather than its double, for
 * the same reason.  NULL when the arguments are not all numbers, and
 * then the doubles do it as before. */
static M42Value *
exact_fold (int op, GPtrArray *args, gboolean smallest, gboolean is_extreme)
{
  g_autoptr (GPtrArray) values = g_ptr_array_new ();
  M42Value *acc;

  if (!collect_values (args, values) || values->len == 0)
    return NULL;

  if (is_extreme)
    {
      M42Value *best = g_ptr_array_index (values, 0);
      double best_x;

      if (!value_number (best, &best_x))
        return NULL;
      for (guint i = 1; i < values->len; i++)
        {
          double x;

          if (!value_number (g_ptr_array_index (values, i), &x))
            return NULL;
          if (smallest ? x < best_x : x > best_x)
            {
              best = g_ptr_array_index (values, i);
              best_x = x;
            }
        }
      return m42_value_ref (best);
    }

  acc = m42_value_ref (g_ptr_array_index (values, 0));
  for (guint i = 1; i < values->len; i++)
    {
      M42Value *next = map2 (op, acc, g_ptr_array_index (values, i));

      m42_value_unref (acc);
      acc = next;
      if (is_error (acc))
        {
          m42_value_unref (acc);
          return NULL;
        }
    }
  return acc;
}

static M42Value *
fold_args (const char *name, GPtrArray *args, double init, double (*combine) (double, double))
{
  g_autoptr (GArray) xs = g_array_new (FALSE, FALSE, sizeof (double));
  double acc = init;

  if (!collect_numbers (args, xs))
    return m42_value_error ("%s expects numbers", name);
  for (guint i = 0; i < xs->len; i++)
    acc = i == 0 ? g_array_index (xs, double, 0) : combine (acc, g_array_index (xs, double, i));
  return m42_value_number (xs->len == 0 ? init : acc);
}

static int
compare_doubles (gconstpointer a, gconstpointer b)
{
  double x = *(const double *) a, y = *(const double *) b;
  return x < y ? -1 : x > y ? 1 : 0;
}

/* Mean, Median and Variance of exact numbers, worked out exactly.
 * Mathematica's spellings promise it -- Mean[{1, 2, 3, 4}] is 5/2, not
 * 2.5 -- while mean() and its lower-case kin are numeric like the rest
 * of MATLAB.  NULL when anything in the list is not exact, and then
 * the doubles do it as before. */
static M42Value *
exact_statistic (const char *name, const M42Value *list)
{
  guint n;
  g_autoptr (GPtrArray) order = NULL;
  g_autoptr (M42Value) total = NULL;
  g_autoptr (M42Value) mean = NULL;

  /* These three and no others: anything else -- a standard deviation,
   * or either of them in MATLAB's spelling -- belongs to the doubles,
   * and must not fall through to the last answer written here. */
  if (strcmp (name, "Mean") != 0 && strcmp (name, "Median") != 0 &&
      strcmp (name, "Variance") != 0)
    return NULL;
  if (list == NULL || list->kind != M42_VALUE_LIST)
    return NULL;
  n = m42_value_list_length (list);
  if (n == 0)
    return NULL;
  for (guint i = 0; i < n; i++)
    {
      const M42Value *e = m42_value_list_nth (list, i);

      if (!(e->kind == M42_VALUE_BIGINT ||
            (e->kind == M42_VALUE_NUMBER && e->exact)))
        return NULL;
    }

  if (strcmp (name, "Median") == 0)
    {
      double middle;

      order = g_ptr_array_new ();
      for (guint i = 0; i < n; i++)
        g_ptr_array_add (order, m42_value_list_nth (list, i));
      for (guint i = 0; i + 1 < order->len; i++)
        for (guint k = 0; k + 1 < order->len - i; k++)
          {
            double x, y;

            if (value_number (g_ptr_array_index (order, k), &x) &&
                value_number (g_ptr_array_index (order, k + 1), &y) && x > y)
              {
                gpointer swap = g_ptr_array_index (order, k);

                g_ptr_array_index (order, k) = g_ptr_array_index (order, k + 1);
                g_ptr_array_index (order, k + 1) = swap;
              }
          }
      if (n % 2 == 1)
        return m42_value_ref (g_ptr_array_index (order, n / 2));
      {
        g_autoptr (M42Value) both = map2 (M42_TOK_PLUS,
                                          g_ptr_array_index (order, n / 2 - 1),
                                          g_ptr_array_index (order, n / 2));
        g_autoptr (M42Value) two = m42_value_number (2);

        (void) middle;
        return map2 (M42_TOK_SLASH, both, two);
      }
    }

  total = m42_value_number (0);
  for (guint i = 0; i < n; i++)
    {
      M42Value *next = map2 (M42_TOK_PLUS, total, m42_value_list_nth (list, i));

      m42_value_unref (total);
      total = next;
      if (is_error (total))
        return NULL;
    }
  {
    g_autoptr (M42Value) count = m42_value_number (n);

    mean = map2 (M42_TOK_SLASH, total, count);
  }
  if (is_error (mean))
    return NULL;
  if (strcmp (name, "Mean") == 0)
    return g_steal_pointer (&mean);

  /* Variance, the sample one: the squares of the distances from the
   * mean, over one less than the count. */
  {
    g_autoptr (M42Value) sum = m42_value_number (0);
    g_autoptr (M42Value) less_one = m42_value_number (n > 1 ? n - 1 : 1);

    if (n < 2)
      return m42_value_number (0);
    for (guint i = 0; i < n; i++)
      {
        g_autoptr (M42Value) away = map2 (M42_TOK_MINUS, m42_value_list_nth (list, i), mean);
        g_autoptr (M42Value) two = m42_value_number (2);
        g_autoptr (M42Value) squared = map2 (M42_TOK_CARET, away, two);
        M42Value *next = map2 (M42_TOK_PLUS, sum, squared);

        m42_value_unref (sum);
        sum = next;
        if (is_error (sum))
          return NULL;
      }
    return map2 (M42_TOK_SLASH, sum, less_one);
  }
}

static M42Value *
statistic (const char *name, GPtrArray *args)
{
  g_autoptr (GArray) xs = g_array_new (FALSE, FALSE, sizeof (double));
  double mean = 0, var = 0;
  guint n;

  if (!collect_numbers (args, xs) || xs->len == 0)
    return m42_value_error ("%s expects numbers", name);
  n = xs->len;
  for (guint i = 0; i < n; i++)
    mean += g_array_index (xs, double, i);
  mean /= n;
  for (guint i = 0; i < n; i++)
    var += pow (g_array_index (xs, double, i) - mean, 2);
  var = n > 1 ? var / (n - 1) : 0;

  if (!strcmp (name, "Mean") || !strcmp (name, "mean"))
    return m42_value_number (mean);
  if (!strcmp (name, "Variance") || !strcmp (name, "var"))
    return m42_value_number (var);
  if (!strcmp (name, "StandardDeviation") || !strcmp (name, "std"))
    return m42_value_number (sqrt (var));
  if (!strcmp (name, "Median") || !strcmp (name, "median"))
    {
      g_array_sort (xs, compare_doubles);
      return m42_value_number (n % 2 ? g_array_index (xs, double, n / 2)
                               : (g_array_index (xs, double, n / 2 - 1) + g_array_index (xs, double, n / 2)) / 2);
    }
  return m42_value_error ("Unknown statistic %s", name);
}

static M42Value *
make_filled (const char *name, GPtrArray *args, double fill, gboolean identity)
{
  double r, c;
  M42Value *err, *out;

  if (args->len < 1 || args->len > 2)
    return m42_value_error ("%s expects a size", name);
  if (!need_number (ARG (0), name, &r, &err))
    return err;
  c = r;
  if (args->len == 2 && !need_number (ARG (1), name, &c, &err))
    return err;
  if (r < 0 || c < 0 || r * c > 1e6)
    return m42_value_error ("%s: size out of range", name);

  out = m42_value_list_new ();
  for (guint i = 0; i < (guint) r; i++)
    {
      M42Value *row = m42_value_list_new ();
      for (guint j = 0; j < (guint) c; j++)
        m42_value_list_append (row, m42_value_number (identity ? (i == j) : fill));
      m42_value_list_append (out, row);
    }
  return out;
}

static M42Value *
range_builtin (const char *name, GPtrArray *args)
{
  double lo = 1, hi, step = 1;
  M42Value *out;

  if (args->len < 1 || args->len > 3)
    return m42_value_error ("%s takes one to three arguments", name);
  for (guint i = 0; i < args->len; i++)
    if (!is_num (ARG (i)))
      return m42_value_error ("%s expects numbers", name);

  if (args->len == 1)
    hi = ARG (0)->u.number;
  else
    {
      lo = ARG (0)->u.number;
      hi = ARG (1)->u.number;
      if (args->len == 3)
        {
          step = ARG (2)->u.number;
          if (strcmp (name, "linspace") == 0)
            step = step > 1 ? (hi - lo) / (step - 1) : hi - lo;
        }
    }
  if (step == 0 || (hi - lo) / step > 1e6)
    return m42_value_error ("%s: range too long", name);

  out = m42_value_list_new ();
  for (double x = lo; step > 0 ? x <= hi + 1e-9 * step : x >= hi - 1e-9 * step; x += step)
    m42_value_list_append (out, m42_value_number (x));
  return out;
}

static void
flatten_into (M42Value *v, M42Value *out)
{
  if (v->kind == M42_VALUE_LIST)
    for (guint i = 0; i < m42_value_list_length (v); i++)
      flatten_into (m42_value_list_nth (v, i), out);
  else
    m42_value_list_append (out, m42_value_ref (v));
}

static gboolean
name_is (const char *name, const char *a, const char *b)
{
  return strcmp (name, a) == 0 || (b != NULL && strcmp (name, b) == 0);
}

static M42Value *
call_builtin (M42Session *s, const char *name, GPtrArray *args)
{
  /* The complex parts, which mean something for a real number too. */
  if (args->len == 1 && is_numeric (ARG (0)))
    {
      static const struct { const char *name, *alias, *canon; } PARTS[] = {
        { "Re", "real", "Re" }, { "Im", "imag", "Im" },
        { "Conjugate", "conj", "Conjugate" }, { "Arg", "angle", "Arg" },
      };

      for (guint i = 0; i < G_N_ELEMENTS (PARTS); i++)
        if (name_is (name, PARTS[i].name, PARTS[i].alias))
          return complex_function (PARTS[i].canon, as_complex (ARG (0)));
    }

  if (args->len == 1)
    for (guint i = 0; i < G_N_ELEMENTS (UNARY_FUNCS); i++)
      if (strcmp (UNARY_FUNCS[i].name, name) == 0)
        return map1_full (ARG (0), UNARY_FUNCS[i].canon, UNARY_FUNCS[i].fn,
                          g_ascii_isupper (name[0]));

  /* sum(A, 2), mean(A, 1) and the rest: along the rows or the columns.
   * Mathematica's Total[m, 2] adds everything, which is what the
   * flattening fold below already does. */
  if (args->len == 2 && is_num (ARG (1)) && m42_value_is_matrix (ARG (0), NULL, NULL) &&
      (name_is (name, "sum", NULL) || name_is (name, "mean", NULL) ||
       name_is (name, "max", NULL) || name_is (name, "min", NULL) ||
       name_is (name, "prod", NULL)))
    {
      guint rows, cols;
      int dim = (int) ARG (1)->u.number;
      M42Value *out = m42_value_list_new ();

      m42_value_is_matrix (ARG (0), &rows, &cols);
      if (dim != 1 && dim != 2)
        return m42_value_error ("%s: the dimension must be 1 or 2", name);

      for (guint a = 0; a < (dim == 1 ? cols : rows); a++)
        {
          double acc = name[0] == 'p' ? 1 : name[0] == 'm' && name[1] == 'a' ? -INFINITY
                        : name[0] == 'm' && name[1] == 'i' ? INFINITY : 0;
          guint count = dim == 1 ? rows : cols;

          for (guint b = 0; b < count; b++)
            {
              guint i = dim == 1 ? b : a, j = dim == 1 ? a : b;
              double x = m42_value_list_nth (m42_value_list_nth (ARG (0), i), j)->u.number;

              if (name[0] == 'p')
                acc *= x;
              else if (name[1] == 'a')
                acc = MAX (acc, x);
              else if (name[1] == 'i')
                acc = MIN (acc, x);
              else
                acc += x;
            }
          if (name[0] == 'm' && name[1] == 'e')
            acc /= count;
          m42_value_list_append (out, m42_value_number (acc));
        }
      return out;
    }

  if (name_is (name, "Total", "sum") || name_is (name, "Plus", NULL))
    {
      /* The total of a matrix is the total of its rows -- a vector of
       * column sums -- which is what both languages mean by it. */
      guint rows, cols;
      g_autoptr (GPtrArray) alone = NULL;
      GPtrArray *adding = args;
      guint levels = 1;

      /* Total[list, n] adds over n levels: the second argument says
       * how deep to go and is not another number to add.  It used to
       * be added, so Total[{1, 2, 3}, 1] came back 7.  sum(A, 2) is
       * MATLAB's dimension and is dealt with further up. */
      if (args->len == 2 && strcmp (name, "Total") == 0 && is_num (ARG (1)))
        {
          double deep = ARG (1)->u.number;

          if (deep < 1 || deep != floor (deep))
            return m42_value_error ("Total: the level is a whole number from one");
          levels = (guint) deep;
          alone = g_ptr_array_new ();
          g_ptr_array_add (alone, ARG (0));
          adding = alone;
        }

      if (levels == 1 && adding->len == 1 &&
          m42_value_is_matrix (g_ptr_array_index (adding, 0), &rows, &cols))
        {
          const M42Value *m = g_ptr_array_index (adding, 0);
          M42Value *out = m42_value_list_new ();

          for (guint j = 0; j < cols; j++)
            {
              double acc = 0;

              for (guint i = 0; i < rows; i++)
                acc += m42_value_list_nth (m42_value_list_nth (m, i), j)->u.number;
              m42_value_list_append (out, m42_value_number (acc));
            }
          return out;
        }
      {
        M42Value *exact = exact_fold (M42_TOK_PLUS, adding, FALSE, FALSE);

        if (exact != NULL)
          return exact;
      }
      return fold_args (name, adding, 0.0, d_add);
    }
  if (name_is (name, "Times", "prod"))
    {
      M42Value *exact = exact_fold (M42_TOK_STAR, args, FALSE, FALSE);

      return exact != NULL ? exact : fold_args (name, args, 1.0, d_mul);
    }
  if (name_is (name, "Max", "max") || name_is (name, "Min", "min"))
    {
      gboolean smallest = name_is (name, "Min", "min");
      M42Value *exact = exact_fold (0, args, smallest, TRUE);

      if (exact != NULL)
        return exact;
      /* Max[x, 0] with a symbol in it is left as it was written, the
       * way every other function of a symbol is, rather than being
       * told it expects numbers. */
      for (guint i = 0; i < args->len; i++)
        if (ARG (i)->kind == M42_VALUE_EXPR)
          {
            M42Node *held = m42_node_new (M42_NODE_CALL);

            held->name = g_strdup (smallest ? "Min" : "Max");
            for (guint k = 0; k < args->len; k++)
              {
                M42Node *one = value_to_node (ARG (k));

                if (one == NULL)
                  {
                    m42_node_free (held);
                    return m42_value_error ("%s expects numbers", name);
                  }
                g_ptr_array_add (held->children, one);
              }
            return m42_value_expr (held);
          }
      return fold_args (name, args, smallest ? INFINITY : -INFINITY,
                        smallest ? d_min : d_max);
    }
  if ((name_is (name, "Mean", "mean") || name_is (name, "Variance", "var") ||
       name_is (name, "StandardDeviation", "std") || name_is (name, "Median", "median")) &&
      !(args->len >= 1 && ARG (0)->kind == M42_VALUE_EXPR))
    {
      /* Of a matrix, each of these is taken down the columns, which is
       * what both languages mean by it -- mean([1 2; 3 4]) is [2 3] --
       * and is how Total already behaves. */
      {
        guint rows, cols;

        if (args->len == 1 && m42_value_is_matrix (ARG (0), &rows, &cols))
          {
            M42Value *out = m42_value_list_new ();

            for (guint j = 0; j < cols; j++)
              {
                GPtrArray *column = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
                M42Value *down = m42_value_list_new ();
                M42Value *answer;

                for (guint i = 0; i < rows; i++)
                  m42_value_list_append (down,
                    m42_value_ref (m42_value_list_nth (m42_value_list_nth (ARG (0), i), j)));
                g_ptr_array_add (column, down);
                answer = exact_statistic (name, down);
                if (answer == NULL)
                  answer = statistic (name, column);
                g_ptr_array_unref (column);
                m42_value_list_append (out, answer);
              }
            return out;
          }
      }

      /* Written the Mathematica way, of exact numbers, the answer is
       * exact too: Mean[{1, 2, 3, 4}] is 5/2. */
      if (args->len == 1 && (strcmp (name, "Mean") == 0 || strcmp (name, "Median") == 0 ||
                             strcmp (name, "Variance") == 0))
        {
          M42Value *exact = exact_statistic (name, ARG (0));

          if (exact != NULL)
            return exact;
        }
      return statistic (name, args);
    }

  if (name_is (name, "Length", "length") || name_is (name, "numel", NULL))
    {
      if (args->len != 1)
        return m42_value_error ("%s takes one argument", name);
      return m42_value_number (ARG (0)->kind == M42_VALUE_LIST ? m42_value_list_length (ARG (0)) : 0);
    }
  if (name_is (name, "Dimensions", "size"))
    {
      M42Value *out = m42_value_list_new ();
      M42Value *v = args->len >= 1 ? ARG (0) : NULL;
      while (v != NULL && v->kind == M42_VALUE_LIST)
        {
          m42_value_list_append (out, m42_value_number (m42_value_list_length (v)));
          v = m42_value_list_length (v) > 0 ? m42_value_list_nth (v, 0) : NULL;
        }
      /* size(A, 1) asks for one of them. */
      if (args->len == 2 && is_num (ARG (1)))
        {
          guint k = (guint) ARG (1)->u.number;
          M42Value *one = k >= 1 && k <= m42_value_list_length (out)
            ? m42_value_ref (m42_value_list_nth (out, k - 1))
            : m42_value_number (1);
          m42_value_unref (out);
          return one;
        }
      return out;
    }
  if (name_is (name, "Range", "linspace") || name_is (name, "colon", NULL))
    return range_builtin (name, args);
  if (name_is (name, "IdentityMatrix", "eye"))
    return make_filled (name, args, 0, TRUE);
  if (name_is (name, "zeros", NULL))
    return make_filled (name, args, 0, FALSE);
  if (name_is (name, "ones", NULL))
    return make_filled (name, args, 1, FALSE);
  if (name_is (name, "ConstantArray", NULL) && args->len == 2)
    {
      double fill;
      M42Value *err;
      GPtrArray *sz = g_ptr_array_new ();
      M42Value *r;
      if (!need_number (ARG (0), name, &fill, &err))
        return err;
      if (ARG (1)->kind == M42_VALUE_LIST)
        for (guint i = 0; i < m42_value_list_length (ARG (1)); i++)
          g_ptr_array_add (sz, m42_value_list_nth (ARG (1), i));
      else
        g_ptr_array_add (sz, ARG (1));
      r = make_filled (name, sz, fill, FALSE);
      g_ptr_array_unref (sz);
      if (sz->len == 1 && !is_error (r))
        {
          M42Value *flat = m42_value_list_new ();
          flatten_into (r, flat);
          m42_value_unref (r);
          return flat;
        }
      return r;
    }

  if (name_is (name, "Transpose", "transpose") && args->len == 1)
    return m42_value_transpose (ARG (0));
  if (name_is (name, "Det", "det") && args->len == 1)
    return m42_value_det (ARG (0));
  if (name_is (name, "Inverse", "inv") && args->len == 1)
    return m42_value_inverse (ARG (0));
  if (name_is (name, "Dot", "mtimes") && args->len == 2)
    return m42_value_dot (ARG (0), ARG (1));
  if (name_is (name, "LinearSolve", "mldivide") && args->len == 2)
    return m42_value_linear_solve (ARG (0), ARG (1));
  if (name_is (name, "RowReduce", "rref") && args->len == 1)
    return m42_value_row_reduce (ARG (0));
  if (name_is (name, "NullSpace", "null") && args->len == 1)
    return m42_value_null_space (ARG (0));
  if (name_is (name, "Orthogonalize", "orth") && args->len == 1)
    return m42_value_orthogonalize (ARG (0));
  if (name_is (name, "LeastSquares", "lscov") && args->len == 2)
    return m42_value_least_squares (ARG (0), ARG (1));
  if (name_is (name, "CharacteristicPolynomial", "poly") && args->len >= 1)
    {
      M42Value *coefficients = m42_value_characteristic (ARG (0));

      /* With a name given, the polynomial itself rather than its
       * coefficients. */
      if (args->len == 2 && ARG (1)->kind == M42_VALUE_EXPR &&
          ARG (1)->u.expr->kind == M42_NODE_IDENT && !is_error (coefficients))
        {
          const char *var = ARG (1)->u.expr->name;
          M42Node *out = NULL;

          for (guint i = 0; i < m42_value_list_length (coefficients); i++)
            {
              double c = m42_value_list_nth (coefficients, i)->u.number;
              M42Node *term;

              if (c == 0)
                continue;
              if (i == 0)
                term = m42_node_number (c);
              else
                {
                  M42Node *power = i == 1 ? m42_node_ident (var)
                    : m42_node_binary (M42_TOK_CARET, m42_node_ident (var),
                                       m42_node_number (i));
                  term = c == 1 ? power
                    : m42_node_binary (M42_TOK_STAR, m42_node_number (c), power);
                }
              out = out == NULL ? term : m42_node_binary (M42_TOK_PLUS, out, term);
            }
          m42_value_unref (coefficients);
          return expr_result (out != NULL ? out : m42_node_number (0));
        }
      return coefficients;
    }
  if (name_is (name, "LUDecomposition", "lu") && args->len == 1)
    return m42_value_lu (ARG (0));
  if (name_is (name, "QRDecomposition", "qr") && args->len == 1)
    return m42_value_qr (ARG (0));
  if (name_is (name, "SingularValueDecomposition", NULL) && args->len == 1)
    return m42_value_svd (ARG (0));
  if (name_is (name, "SingularValueList", "svd") && args->len == 1)
    return m42_value_singular_values (ARG (0));
  if (name_is (name, "PseudoInverse", "pinv") && args->len == 1)
    return m42_value_pseudo_inverse (ARG (0));
  if (name_is (name, "Cond", "cond") && args->len == 1)
    return m42_value_condition (ARG (0));
  if (name_is (name, "Eigensystem", NULL) && args->len == 1)
    return m42_value_eigensystem (ARG (0));
  if (name_is (name, "MatrixExp", "expm") && args->len == 1)
    return m42_value_matrix_exp (ARG (0));
  if (name_is (name, "Projection", NULL) && args->len == 2 &&
      m42_value_is_vector (ARG (0)) && m42_value_is_vector (ARG (1)))
    {
      /* The part of one vector that lies along another. */
      guint n = m42_value_list_length (ARG (0));
      double dot = 0, len = 0;
      M42Value *out;

      if (n != m42_value_list_length (ARG (1)))
        return m42_value_error ("Projection wants two vectors of the same length");
      for (guint i = 0; i < n; i++)
        {
          double x = m42_value_list_nth (ARG (0), i)->u.number;
          double y = m42_value_list_nth (ARG (1), i)->u.number;

          dot += x * y;
          len += y * y;
        }
      if (len < 1e-14)
        return m42_value_error ("Projection: cannot project onto nothing");
      out = m42_value_list_new ();
      for (guint i = 0; i < n; i++)
        m42_value_list_append (out, m42_value_number (dot / len *
                                                      m42_value_list_nth (ARG (1), i)->u.number));
      return out;
    }

  if (name_is (name, "Tr", "trace") && args->len == 1)
    {
      guint r, c;
      double t = 0;
      if (!m42_value_is_matrix (ARG (0), &r, &c) || r != c)
        return m42_value_error ("%s expects a square matrix", name);
      for (guint i = 0; i < r; i++)
        t += m42_value_list_nth (m42_value_list_nth (ARG (0), i), i)->u.number;
      return m42_value_number (t);
    }
  /* --- a list of rules, read as a table ----------------------------------
   *
   * Solve, GroupBy and the options of a plot all hand back rules, and
   * these are how you take them apart.  math42 has no association, so
   * a list of rules is what stands in for one.
   */

  if ((name_is (name, "Keys", NULL) || name_is (name, "Values", NULL)) && args->len == 1)
    {
      gboolean wants_keys = name_is (name, "Keys", NULL);
      M42Value *out;

      /* One rule on its own is answered with one thing, as a list of
       * them is answered with a list. */
      if (ARG (0)->kind == M42_VALUE_EXPR && ARG (0)->u.expr->kind == M42_NODE_RULE)
        return expr_result (m42_node_copy (m42_node_child (ARG (0)->u.expr,
                                                           wants_keys ? 0 : 1)));
      if (ARG (0)->kind != M42_VALUE_LIST)
        return m42_value_error ("%s wants a rule, or a list of them", name);

      out = m42_value_list_new ();
      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          M42Value *e = m42_value_list_nth (ARG (0), i);

          if (e->kind != M42_VALUE_EXPR || e->u.expr->kind != M42_NODE_RULE)
            {
              m42_value_unref (out);
              return m42_value_error ("%s: that is not a rule", name);
            }
          m42_value_list_append (out,
            expr_result (m42_node_copy (m42_node_child (e->u.expr, wants_keys ? 0 : 1))));
        }
      return out;
    }

  if (name_is (name, "Lookup", NULL) && args->len >= 2 && ARG (0)->kind == M42_VALUE_LIST)
    {
      g_autofree char *wanted = m42_value_to_string (ARG (1));

      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          M42Value *e = m42_value_list_nth (ARG (0), i);
          g_autoptr (GString) key = g_string_new (NULL);

          if (e->kind != M42_VALUE_EXPR || e->u.expr->kind != M42_NODE_RULE)
            continue;
          m42_node_to_string (key, m42_node_child (e->u.expr, 0));
          if (strcmp (key->str, wanted) == 0)
            return expr_result (m42_node_copy (m42_node_child (e->u.expr, 1)));
        }
      /* Not there: what the caller said to use instead, or nothing. */
      return args->len > 2 ? m42_value_ref (ARG (2)) : m42_value_null ();
    }

  /* --- a few more of the everyday ones ------------------------------------ */

  /* Subdivide[a, b, n] cuts the way from a to b into n pieces, which
   * is n + 1 points -- one more than linspace gives, and its own
   * function for that reason. */
  if (name_is (name, "Subdivide", NULL) && args->len >= 1)
    {
      double lo = 0, hi = 1;
      double pieces;
      M42Value *out;

      if (args->len == 1)
        {
          if (!value_number (ARG (0), &pieces))
            return m42_value_error ("Subdivide wants a whole number of pieces");
        }
      else if (args->len == 2)
        {
          if (!value_number (ARG (0), &hi) || !value_number (ARG (1), &pieces))
            return m42_value_error ("Subdivide wants numbers");
        }
      else if (!value_number (ARG (0), &lo) || !value_number (ARG (1), &hi) ||
               !value_number (ARG (2), &pieces))
        return m42_value_error ("Subdivide wants numbers");

      if (pieces < 1 || pieces > 1e6 || pieces != floor (pieces))
        return m42_value_error ("Subdivide: that many pieces is out of range");
      out = m42_value_list_new ();
      for (int i = 0; i <= (int) pieces; i++)
        {
          g_autoptr (M42Value) step = m42_value_number (i);
          g_autoptr (M42Value) many = m42_value_number (pieces);
          g_autoptr (M42Value) part = map2 (M42_TOK_SLASH, step, many);
          g_autoptr (M42Value) width = m42_value_number (hi - lo);
          g_autoptr (M42Value) along = map2 (M42_TOK_STAR, part, width);
          g_autoptr (M42Value) from = m42_value_number (lo);

          m42_value_list_append (out, map2 (M42_TOK_PLUS, from, along));
        }
      return out;
    }

  if ((name_is (name, "Rescale", NULL) || name_is (name, "Standardize", NULL)) &&
      args->len == 1 && m42_value_is_vector (ARG (0)))
    {
      guint n = m42_value_list_length (ARG (0));
      gboolean to_range = name_is (name, "Rescale", NULL);
      double lo = INFINITY, hi = -INFINITY, mean = 0, spread = 0;
      M42Value *out;

      for (guint i = 0; i < n; i++)
        {
          double x;

          if (!value_number (m42_value_list_nth (ARG (0), i), &x))
            return m42_value_error ("%s expects numbers", name);
          lo = MIN (lo, x);
          hi = MAX (hi, x);
          mean += x;
        }
      mean /= n;
      for (guint i = 0; i < n; i++)
        {
          double x;

          value_number (m42_value_list_nth (ARG (0), i), &x);
          spread += (x - mean) * (x - mean);
        }
      spread = n > 1 ? sqrt (spread / (n - 1)) : 0;
      if ((to_range && hi == lo) || (!to_range && spread == 0))
        return m42_value_error ("%s: they are all the same", name);

      out = m42_value_list_new ();
      for (guint i = 0; i < n; i++)
        {
          double x;

          value_number (m42_value_list_nth (ARG (0), i), &x);
          m42_value_list_append (out, m42_value_real (to_range ? (x - lo) / (hi - lo)
                                                               : (x - mean) / spread));
        }
      return out;
    }

  if (name_is (name, "ArrayDepth", "ndims") && args->len == 1)
    {
      const M42Value *v = ARG (0);
      guint deep = 0;

      while (v != NULL && v->kind == M42_VALUE_LIST && m42_value_list_length (v) > 0)
        {
          deep++;
          v = m42_value_list_nth (v, 0);
        }
      return m42_value_number (deep);
    }

  /* A number written to so many figures, as a string. */
  if (name_is (name, "NumberForm", NULL) && args->len == 2)
    {
      double x, figures;

      if (!value_number (ARG (0), &x) || !value_number (ARG (1), &figures))
        return m42_value_error ("NumberForm wants a number and how many figures");
      if (figures < 1 || figures > 17)
        return m42_value_error ("NumberForm: between one and seventeen figures");
      {
        g_autofree char *text = g_strdup_printf ("%.*g", (int) figures, x);

        return m42_value_string (text);
      }
    }

  if (name_is (name, "Norm", "norm") && args->len >= 1)
    {
      g_autoptr (GArray) xs = g_array_new (FALSE, FALSE, sizeof (double));
      GPtrArray *first = g_ptr_array_new ();
      double p = 2, t = 0;

      /* Norm[v, p] is the pth root of the sum of pth powers, and
       * Norm[v, Infinity] the largest of them. */
      if (args->len > 1 && !value_number (ARG (1), &p))
        {
          g_ptr_array_unref (first);
          return m42_value_error ("%s: the second argument is which norm", name);
        }
      g_ptr_array_add (first, ARG (0));
      if (!collect_numbers (first, xs))
        {
          g_ptr_array_unref (first);
          return m42_value_error ("%s expects numbers", name);
        }
      g_ptr_array_unref (first);
      if (p < 1)
        return m42_value_error ("%s: the norm is one or more", name);
      if (isinf (p))
        {
          for (guint i = 0; i < xs->len; i++)
            t = MAX (t, fabs (g_array_index (xs, double, i)));
          return m42_value_number (t);
        }
      for (guint i = 0; i < xs->len; i++)
        t += pow (fabs (g_array_index (xs, double, i)), p);
      return m42_value_number (p == 2 ? sqrt (t) : pow (t, 1 / p));
    }
  if (name_is (name, "Cross", "cross") && args->len == 2)
    {
      M42Value *a = ARG (0), *b = ARG (1), *out;
      double x[3], y[3];
      if (!m42_value_is_vector (a) || !m42_value_is_vector (b) ||
          m42_value_list_length (a) != 3 || m42_value_list_length (b) != 3)
        return m42_value_error ("%s expects two vectors of length 3", name);
      for (int i = 0; i < 3; i++)
        {
          x[i] = m42_value_list_nth (a, i)->u.number;
          y[i] = m42_value_list_nth (b, i)->u.number;
        }
      out = m42_value_list_new ();
      m42_value_list_append (out, m42_value_number (x[1] * y[2] - x[2] * y[1]));
      m42_value_list_append (out, m42_value_number (x[2] * y[0] - x[0] * y[2]));
      m42_value_list_append (out, m42_value_number (x[0] * y[1] - x[1] * y[0]));
      return out;
    }
  if (name_is (name, "MatrixForm", NULL) && args->len >= 1)
    return m42_value_ref (ARG (0));
  /* N asks for the decimal: 1/3 becomes 0.333333333333333. */
  if (name_is (name, "N", "double") && args->len >= 1)
    {
      double x;

      if (value_number (ARG (0), &x))
        return m42_value_real (x);
      if (ARG (0)->kind == M42_VALUE_LIST)
        {
          M42Value *out = m42_value_list_new ();
          for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
            {
              M42Value *e = m42_value_list_nth (ARG (0), i);
              m42_value_list_append (out, is_num (e) ? m42_value_real (e->u.number)
                                                     : m42_value_ref (e));
            }
          return out;
        }
      return m42_value_ref (ARG (0));
    }
  /* The two halves of a fraction, as Mathematica hands them over. */
  if (name_is (name, "Numerator", NULL) && args->len == 1 && is_num (ARG (0)))
    return m42_value_number (ARG (0)->exact ? (double) ARG (0)->num : ARG (0)->u.number);
  if (name_is (name, "Denominator", NULL) && args->len == 1 && is_num (ARG (0)))
    return m42_value_number (ARG (0)->exact ? (double) ARG (0)->den : 1);
  if (name_is (name, "Rationalize", NULL) && args->len == 1 && is_num (ARG (0)))
    {
      /* The best fraction with a small denominator, by continued
       * fractions: 0.333333 becomes 1/3. */
      double x = ARG (0)->u.number, frac = x;
      gint64 p0 = 0, p1 = 1, q0 = 1, q1 = 0;

      if (!isfinite (x))
        return m42_value_ref (ARG (0));
      for (int i = 0; i < 30; i++)
        {
          double whole = floor (frac);
          gint64 p2 = (gint64) whole * p1 + p0, q2 = (gint64) whole * q1 + q0;

          p0 = p1; p1 = p2;
          q0 = q1; q1 = q2;
          if (q1 != 0 && fabs ((double) p1 / (double) q1 - x) < 1e-10)
            return m42_value_rational (p1, q1);
          frac -= whole;
          if (fabs (frac) < 1e-12)
            break;
          frac = 1 / frac;
        }
      return m42_value_ref (ARG (0));
    }
  if (name_is (name, "Eigenvalues", "eig") && args->len == 1)
    return m42_value_eigenvalues (ARG (0));
  if (name_is (name, "Eigenvectors", NULL) && args->len == 1)
    return m42_value_eigenvectors (ARG (0));
  if (name_is (name, "MatrixPower", "mpower") && args->len == 2)
    {
      double k;
      M42Value *err;
      if (!need_number (ARG (1), name, &k, &err))
        return err;
      if (k != floor (k) || fabs (k) > 1000)
        return m42_value_error ("%s expects a whole exponent", name);
      return m42_value_matrix_power (ARG (0), (int) k);
    }
  if (name_is (name, "Rank", "rank") && args->len == 1)
    {
      /* The rank as row reduction finds it. */
      g_autoptr (M42Matrix) m = m42_matrix_from_value (ARG (0), FALSE);
      guint rank = 0;

      if (m == NULL)
        return m42_value_error ("%s expects a matrix", name);
      for (guint col = 0, row = 0; col < m->cols && row < m->rows; col++)
        {
          guint pivot = row;
          for (guint i = row + 1; i < m->rows; i++)
            if (fabs (*m42_matrix_at (m, i, col)) > fabs (*m42_matrix_at (m, pivot, col)))
              pivot = i;
          if (fabs (*m42_matrix_at (m, pivot, col)) < 1e-10)
            continue;
          for (guint j = 0; j < m->cols; j++)
            {
              double t = *m42_matrix_at (m, row, j);
              *m42_matrix_at (m, row, j) = *m42_matrix_at (m, pivot, j);
              *m42_matrix_at (m, pivot, j) = t;
            }
          for (guint i = row + 1; i < m->rows; i++)
            {
              double f = *m42_matrix_at (m, i, col) / *m42_matrix_at (m, row, col);
              for (guint j = 0; j < m->cols; j++)
                *m42_matrix_at (m, i, j) -= f * *m42_matrix_at (m, row, j);
            }
          rank++;
          row++;
        }
      return m42_value_number (rank);
    }

  /* The functional shapes: Apply, Select, Fold, Nest. */
  if (name_is (name, "Apply", NULL) && args->len == 2 &&
      ARG (1)->kind == M42_VALUE_LIST)
    {
      GPtrArray *inner = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
      M42Value *r;
      for (guint i = 0; i < m42_value_list_length (ARG (1)); i++)
        g_ptr_array_add (inner, m42_value_ref (m42_value_list_nth (ARG (1), i)));
      r = apply_callable (s, ARG (0), inner);
      g_ptr_array_unref (inner);
      return r;
    }
  if (name_is (name, "Select", NULL) && args->len == 2 &&
      ARG (0)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();
      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          GPtrArray *one = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
          g_autoptr (M42Value) keep = NULL;
          g_ptr_array_add (one, m42_value_ref (m42_value_list_nth (ARG (0), i)));
          keep = apply_callable (s, ARG (1), one);
          g_ptr_array_unref (one);
          if (is_error (keep))
            {
              m42_value_unref (out);
              return g_steal_pointer (&keep);
            }
          if (is_num (keep) && keep->u.number != 0)
            m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i)));
        }
      return out;
    }
  /* --- folding, nesting and gathering ------------------------------------
   *
   * The rest of the shelf Mathematica keeps next to Map and Fold.
   */

  if (name_is (name, "FoldList", NULL) && args->len == 3 &&
      ARG (2)->kind == M42_VALUE_LIST)
    {
      M42Value *acc = m42_value_ref (ARG (1));
      M42Value *out = m42_value_list_new ();

      m42_value_list_append (out, m42_value_ref (acc));
      for (guint i = 0; i < m42_value_list_length (ARG (2)); i++)
        {
          GPtrArray *two = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
          M42Value *next;

          g_ptr_array_add (two, acc);
          g_ptr_array_add (two, m42_value_ref (m42_value_list_nth (ARG (2), i)));
          next = apply_callable (s, ARG (0), two);
          g_ptr_array_unref (two);
          acc = next;
          if (is_error (acc))
            {
              m42_value_unref (out);
              return acc;
            }
          m42_value_list_append (out, m42_value_ref (acc));
        }
      m42_value_unref (acc);
      return out;
    }

  /* Applied over and over until it stops making a difference, or while
   * a test still holds. */
  if ((name_is (name, "FixedPoint", NULL) || name_is (name, "FixedPointList", NULL) ||
       name_is (name, "NestWhile", NULL) || name_is (name, "NestWhileList", NULL)) &&
      args->len >= 2)
    {
      gboolean keep_all = strstr (name, "List") != NULL;
      gboolean while_test = strncmp (name, "NestWhile", 9) == 0;
      M42Value *acc = m42_value_ref (ARG (1));
      M42Value *out = keep_all ? m42_value_list_new () : NULL;

      if (while_test && args->len < 3)
        {
          m42_value_unref (acc);
          m42_value_unref (out);
          return m42_value_error ("%s wants something to keep going by", name);
        }
      if (keep_all)
        m42_value_list_append (out, m42_value_ref (acc));

      for (int step = 0; step < 10000; step++)
        {
          GPtrArray *one = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
          M42Value *next;

          if (while_test)
            {
              GPtrArray *ask = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
              g_autoptr (M42Value) holds = NULL;

              g_ptr_array_add (ask, m42_value_ref (acc));
              holds = apply_callable (s, ARG (2), ask);
              g_ptr_array_unref (ask);
              if (is_error (holds))
                {
                  g_ptr_array_unref (one);
                  m42_value_unref (acc);
                  m42_value_unref (out);
                  return m42_value_ref (holds);
                }
              if (!is_num (holds) || holds->u.number == 0)
                {
                  g_ptr_array_unref (one);
                  break;
                }
            }

          g_ptr_array_add (one, m42_value_ref (acc));
          next = apply_callable (s, ARG (0), one);
          g_ptr_array_unref (one);
          if (is_error (next))
            {
              m42_value_unref (acc);
              m42_value_unref (out);
              return next;
            }

          /* Stopped moving: the point it was looking for. */
          if (!while_test)
            {
              g_autofree char *was = m42_value_to_string (acc);
              g_autofree char *now = m42_value_to_string (next);
              gboolean settled = strcmp (was, now) == 0;

              if (settled)
                {
                  m42_value_unref (next);
                  break;
                }
              /* Growing rather than settling: FixedPoint[x, x] builds
               * x[x[x[...]]] for ever, and ten thousand of those is
               * deep enough to take the stack down when it is printed.
               * Something this large is not on its way anywhere. */
              if (strlen (now) > 20000)
                {
                  m42_value_unref (next);
                  m42_value_unref (acc);
                  m42_value_unref (out);
                  return m42_value_error ("%s: that is growing, not settling", name);
                }
            }
          m42_value_unref (acc);
          acc = next;
          if (keep_all)
            m42_value_list_append (out, m42_value_ref (acc));
        }
      if (keep_all)
        {
          m42_value_unref (acc);
          return out;
        }
      return acc;
    }

  /* The ones that are the same, put together. */
  if ((name_is (name, "Gather", NULL) || name_is (name, "GatherBy", NULL) ||
       name_is (name, "GroupBy", NULL)) && args->len >= 1 &&
      ARG (0)->kind == M42_VALUE_LIST)
    {
      gboolean by_key = name_is (name, "GroupBy", NULL);
      guint n = m42_value_list_length (ARG (0));
      g_autoptr (GPtrArray) keys = g_ptr_array_new_with_free_func (g_free);
      g_autoptr (GPtrArray) groups = g_ptr_array_new ();
      M42Value *out;

      for (guint i = 0; i < n; i++)
        {
          M42Value *e = m42_value_list_nth (ARG (0), i);
          g_autoptr (M42Value) key = NULL;
          g_autofree char *written = NULL;
          guint found = keys->len;

          if (args->len > 1)
            {
              GPtrArray *one = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);

              g_ptr_array_add (one, m42_value_ref (e));
              key = apply_callable (s, ARG (1), one);
              g_ptr_array_unref (one);
              if (is_error (key))
                return m42_value_ref (key);
            }
          else
            key = m42_value_ref (e);

          written = m42_value_to_string (key);
          for (guint k = 0; k < keys->len; k++)
            if (strcmp (g_ptr_array_index (keys, k), written) == 0)
              found = k;
          if (found == keys->len)
            {
              M42Value *group = m42_value_list_new ();

              /* GroupBy keeps what it grouped by, at the front. */
              if (by_key)
                m42_value_list_append (group, m42_value_ref (key));
              g_ptr_array_add (keys, g_strdup (written));
              g_ptr_array_add (groups, group);
            }
          m42_value_list_append (g_ptr_array_index (groups, found), m42_value_ref (e));
        }

      out = m42_value_list_new ();
      for (guint k = 0; k < groups->len; k++)
        {
          M42Value *group = g_ptr_array_index (groups, k);

          if (by_key)
            {
              /* key -> {the ones with it} */
              M42Value *rest = m42_value_list_new ();
              M42Node *rule = m42_node_new (M42_NODE_RULE);
              M42Value *key_value = m42_value_list_nth (group, 0);
              M42Node *key_node = value_to_node (key_value);

              for (guint i = 1; i < m42_value_list_length (group); i++)
                m42_value_list_append (rest, m42_value_ref (m42_value_list_nth (group, i)));
              g_ptr_array_add (rule->children,
                               key_node != NULL ? key_node : m42_node_number (0));
              {
                M42Node *rest_node = value_to_node (rest);

                g_ptr_array_add (rule->children,
                                 rest_node != NULL ? rest_node : m42_node_number (0));
              }
              m42_value_unref (rest);
              m42_value_list_append (out, m42_value_expr (rule));
            }
          else
            m42_value_list_append (out, m42_value_ref (group));
          m42_value_unref (group);
        }
      return out;
    }

  /* {Sin, Cos} all given the same argument. */
  if (name_is (name, "Through", NULL) && args->len == 2 &&
      ARG (0)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          GPtrArray *one = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
          M42Value *answer;

          g_ptr_array_add (one, m42_value_ref (ARG (1)));
          answer = apply_callable (s, m42_value_list_nth (ARG (0), i), one);
          g_ptr_array_unref (one);
          if (is_error (answer))
            {
              m42_value_unref (out);
              return answer;
            }
          m42_value_list_append (out, answer);
        }
      return out;
    }

  if (name_is (name, "Fold", NULL) && args->len == 3 &&
      ARG (2)->kind == M42_VALUE_LIST)
    {
      M42Value *acc = m42_value_ref (ARG (1));
      for (guint i = 0; i < m42_value_list_length (ARG (2)); i++)
        {
          GPtrArray *two = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
          M42Value *next;
          g_ptr_array_add (two, acc);
          g_ptr_array_add (two, m42_value_ref (m42_value_list_nth (ARG (2), i)));
          next = apply_callable (s, ARG (0), two);
          g_ptr_array_unref (two);
          acc = next;
          if (is_error (acc))
            return acc;
        }
      return acc;
    }
  if ((name_is (name, "Nest", NULL) || name_is (name, "NestList", NULL)) && args->len == 3)
    {
      gboolean keep_all = name[4] == 'L';
      double k;
      M42Value *err, *acc, *out = keep_all ? m42_value_list_new () : NULL;

      if (!need_number (ARG (2), name, &k, &err))
        return err;
      if (k < 0 || k > 100000)
        return m42_value_error ("%s: bad count", name);
      acc = m42_value_ref (ARG (1));
      if (keep_all)
        m42_value_list_append (out, m42_value_ref (acc));
      for (int i = 0; i < (int) k; i++)
        {
          GPtrArray *one = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
          M42Value *next;
          g_ptr_array_add (one, acc);
          next = apply_callable (s, ARG (0), one);
          g_ptr_array_unref (one);
          acc = next;
          if (is_error (acc))
            {
              g_clear_pointer (&out, m42_value_unref);
              return acc;
            }
          if (keep_all)
            m42_value_list_append (out, m42_value_ref (acc));
        }
      if (keep_all)
        {
          m42_value_unref (acc);
          return out;
        }
      return acc;
    }

  if (name_is (name, "Flatten", NULL) && args->len == 1)
    {
      M42Value *out = m42_value_list_new ();
      flatten_into (ARG (0), out);
      return out;
    }
  if (name_is (name, "Sort", "sort") && args->len == 1 &&
      ARG (0)->kind == M42_VALUE_LIST)
    {
      /* The values themselves are put in order, not their doubles, so
       * that a third stays a third. */
      guint n = m42_value_list_length (ARG (0));
      g_autoptr (GPtrArray) order = g_ptr_array_new ();
      M42Value *out;

      for (guint i = 0; i < n; i++)
        g_ptr_array_add (order, m42_value_list_nth (ARG (0), i));
      for (guint i = 0; i + 1 < n; i++)
        for (guint k = 0; k + 1 < n - i; k++)
          {
            double x, y;

            if (!value_number (g_ptr_array_index (order, k), &x) ||
                !value_number (g_ptr_array_index (order, k + 1), &y))
              return m42_value_error ("%s expects numbers", name);
            if (x > y)
              {
                gpointer swap = g_ptr_array_index (order, k);

                g_ptr_array_index (order, k) = g_ptr_array_index (order, k + 1);
                g_ptr_array_index (order, k + 1) = swap;
              }
          }
      out = m42_value_list_new ();
      for (guint i = 0; i < n; i++)
        m42_value_list_append (out, m42_value_ref (g_ptr_array_index (order, i)));
      return out;
    }
  if (name_is (name, "Reverse", "flip") && args->len == 1 && ARG (0)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();
      for (guint i = m42_value_list_length (ARG (0)); i > 0; i--)
        m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i - 1)));
      return out;
    }
  if ((name_is (name, "First", NULL) || name_is (name, "Last", NULL)) &&
      args->len == 1 && ARG (0)->kind == M42_VALUE_LIST && m42_value_list_length (ARG (0)) > 0)
    return m42_value_ref (m42_value_list_nth (ARG (0), name[0] == 'F' ? 0 : m42_value_list_length (ARG (0)) - 1));
  if (name_is (name, "Rest", NULL) && args->len == 1 && ARG (0)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();
      for (guint i = 1; i < m42_value_list_length (ARG (0)); i++)
        m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i)));
      return out;
    }
  if (name_is (name, "Append", NULL) && args->len == 2 && ARG (0)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();
      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i)));
      m42_value_list_append (out, m42_value_ref (ARG (1)));
      return out;
    }
  if (name_is (name, "Join", "horzcat"))
    {
      M42Value *out = m42_value_list_new ();
      for (guint k = 0; k < args->len; k++)
        {
          if (ARG (k)->kind != M42_VALUE_LIST)
            {
              m42_value_unref (out);
              return m42_value_error ("%s expects lists", name);
            }
          for (guint i = 0; i < m42_value_list_length (ARG (k)); i++)
            m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (k), i)));
        }
      return out;
    }
  if (name_is (name, "Map", "arrayfun") && args->len == 2)
    {
      M42Value *f = ARG (0), *list = ARG (1), *out;
      if (list->kind != M42_VALUE_LIST)
        return m42_value_error ("%s expects a function and a list", name);
      out = m42_value_list_new ();
      for (guint i = 0; i < m42_value_list_length (list); i++)
        {
          GPtrArray *one = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
          M42Value *r;
          g_ptr_array_add (one, m42_value_ref (m42_value_list_nth (list, i)));
          r = apply_callable (s, f, one);
          g_ptr_array_unref (one);
          if (is_error (r))
            {
              m42_value_unref (out);
              return r;
            }
          m42_value_list_append (out, r);
        }
      return out;
    }
  if (name_is (name, "Power", "power") && args->len == 2)
    return map2 (M42_TOK_CARET, ARG (0), ARG (1));
  if (name_is (name, "Mod", "mod") && args->len == 2)
    return map2 (M42_TOK_PERCENT, ARG (0), ARG (1));
  /* Whole numbers, as a first course in number theory meets them. */
  if ((name_is (name, "GCD", "gcd") || name_is (name, "LCM", "lcm")) && args->len >= 2)
    {
      g_autoptr (GArray) xs = g_array_new (FALSE, FALSE, sizeof (double));
      gint64 acc;
      gboolean want_lcm = name[0] == 'L' || name[0] == 'l';

      if (!collect_numbers (args, xs) || xs->len == 0)
        return m42_value_error ("%s expects whole numbers", name);
      acc = (gint64) g_array_index (xs, double, 0);
      for (guint i = 1; i < xs->len; i++)
        {
          gint64 b = (gint64) g_array_index (xs, double, i), a = acc, t;
          gint64 g;

          while (b != 0)
            {
              t = a % b;
              a = b;
              b = t;
            }
          g = a < 0 ? -a : a;
          if (g == 0)
            g = 1;
          acc = want_lcm ? acc / g * (gint64) g_array_index (xs, double, i) : g;
          if (acc < 0)
            acc = -acc;
        }
      return m42_value_number ((double) acc);
    }
  /* The Bessel functions and the orthogonal polynomials: an order (or a
   * degree) and an argument. */
  {
    static const struct { const char *name, *matlab; char which; } TWO[] = {
      { "BesselJ", "besselj", 'J' }, { "BesselY", "bessely", 'Y' },
      { "BesselI", "besseli", 'I' }, { "BesselK", "besselk", 'K' },
      { "LegendreP", NULL, 'P' },    { "ChebyshevT", NULL, 'T' },
      { "ChebyshevU", NULL, 'U' },   { "HermiteH", NULL, 'H' },
      { "LaguerreL", NULL, 'L' }
    };

    for (guint i = 0; i < G_N_ELEMENTS (TWO); i++)
      if (name_is (name, TWO[i].name, TWO[i].matlab) && args->len == 2)
        {
          double order, x, answer;
          M42Value *err;

          if (!need_number (ARG (0), name, &order, &err) ||
              !need_number (ARG (1), name, &x, &err))
            return err;
          switch (TWO[i].which)
            {
            case 'J': answer = bessel_j (order, x); break;
            case 'Y': answer = bessel_y (order, x); break;
            case 'I': answer = bessel_i (order, x); break;
            case 'K': answer = bessel_k (order, x); break;
            default:
              if (order != floor (order) || order < 0)
                return m42_value_error ("%s expects a whole degree that is not negative", name);
              answer = orthogonal (&TWO[i].which, (int) order, x);
              break;
            }
          if (isnan (answer))
            return m42_value_error ("%s: no value there", name);
          return m42_value_number (answer);
        }
  }
  if (name_is (name, "Beta", "beta") && args->len == 2)
    {
      double a, b;
      M42Value *err;

      if (!need_number (ARG (0), name, &a, &err) || !need_number (ARG (1), name, &b, &err))
        return err;
      if (a <= 0 || b <= 0)
        return m42_value_error ("Beta expects two numbers above zero");
      return m42_value_number (exp (lgamma (a) + lgamma (b) - lgamma (a + b)));
    }
  if (name_is (name, "Factorial", "factorial") && args->len == 1 && is_whole (ARG (0)) &&
      ARG (0)->kind == M42_VALUE_NUMBER && ARG (0)->num >= 0 && ARG (0)->num <= 20000)
    return m42_value_bigint (m42_big_factorial ((guint) ARG (0)->num));

  if (name_is (name, "Binomial", "nchoosek") && args->len == 2 &&
      is_whole (ARG (0)) && is_whole (ARG (1)) &&
      ARG (0)->kind == M42_VALUE_NUMBER && ARG (1)->kind == M42_VALUE_NUMBER &&
      ARG (0)->num >= 0 && ARG (1)->num >= 0 && ARG (1)->num <= ARG (0)->num &&
      ARG (0)->num <= 100000)
    {
      /* Multiplied up and divided down as it goes, so that it stays
       * exact however large it grows. */
      g_autoptr (M42Big) acc = m42_big_from_int64 (1);
      gint64 n = ARG (0)->num, k = MIN (ARG (1)->num, ARG (0)->num - ARG (1)->num);

      for (gint64 i = 0; i < k; i++)
        {
          g_autoptr (M42Big) step = m42_big_from_int64 (n - i);
          M42Big *up = m42_big_multiply (acc, step);
          M42Big *down = m42_big_divide_small (up, i + 1, NULL);

          m42_big_free (up);
          m42_big_free (acc);
          acc = down;
        }
      return m42_value_bigint (m42_big_copy (acc));
    }

  if (name_is (name, "Binomial", "nchoosek") && args->len == 2)
    {
      double n_, k_;
      M42Value *err;
      double acc = 1;

      if (!need_number (ARG (0), name, &n_, &err) || !need_number (ARG (1), name, &k_, &err))
        return err;
      if (k_ < 0 || k_ != floor (k_) || k_ > 1000)
        return m42_value_error ("%s expects a small whole second argument", name);
      for (int i = 0; i < (int) k_; i++)
        acc = acc * (n_ - i) / (i + 1);
      return m42_value_number (round (acc * 1e9) / 1e9);
    }
  if ((name_is (name, "PrimeQ", "isprime") || name_is (name, "Prime", NULL) ||
       name_is (name, "Fibonacci", NULL)) && args->len == 1)
    {
      double x;
      M42Value *err;

      if (!need_number (ARG (0), name, &x, &err))
        return err;
      if (x != floor (x) || fabs (x) > 1e9)
        return m42_value_error ("%s expects a whole number", name);

      if (name[0] == 'F')
        {
          /* Exactly, however far along: the numbers outgrow a gint64
           * at the ninety-third and keep going. */
          g_autoptr (M42Big) previous = m42_big_from_int64 (0);
          g_autoptr (M42Big) current = m42_big_from_int64 (1);

          if (x < 0 || x > 20000)
            return m42_value_error ("Fibonacci: between nothing and twenty thousand");
          for (int i = 0; i < (int) x; i++)
            {
              M42Big *next = m42_big_add (previous, current);

              m42_big_free (previous);
              previous = current;
              current = next;
            }
          return m42_value_bigint (m42_big_copy (previous));
        }
      {
        /* Trial division: honest, and fast enough at this size. */
        gint64 target = (gint64) x, found = 0, candidate = 1;

        for (;;)
          {
            gboolean prime;
            gint64 test = name_is (name, "PrimeQ", "isprime") ? target : ++candidate;

            prime = test >= 2;
            for (gint64 d = 2; prime && d * d <= test; d++)
              if (test % d == 0)
                prime = FALSE;
            if (name_is (name, "PrimeQ", "isprime"))
              return m42_value_number (prime);
            if (prime && ++found == target)
              return m42_value_number ((double) candidate);
            if (candidate > 1000000)
              return m42_value_error ("Prime: that one is too far along");
          }
      }
    }
  if (name_is (name, "RandomReal", "rand") && args->len <= 1)
    {
      if (args->len == 0)
        return m42_value_real (g_random_double ());
      if (ARG (0)->kind == M42_VALUE_LIST && m42_value_list_length (ARG (0)) == 2 &&
          m42_value_is_vector (ARG (0)))
        return m42_value_real (g_random_double_range (m42_value_list_nth (ARG (0), 0)->u.number,
                                                      m42_value_list_nth (ARG (0), 1)->u.number));
      if (is_num (ARG (0)))
        {
          M42Value *out = m42_value_list_new ();
          for (int i = 0; i < (int) ARG (0)->u.number && i < 100000; i++)
            m42_value_list_append (out, m42_value_real (g_random_double ()));
          return out;
        }
    }
  if (name_is (name, "RandomInteger", "randi") && args->len == 1 &&
      ARG (0)->kind == M42_VALUE_LIST && m42_value_list_length (ARG (0)) == 2)
    return m42_value_number (g_random_int_range ((gint32) m42_value_list_nth (ARG (0), 0)->u.number,
                                                 (gint32) m42_value_list_nth (ARG (0), 1)->u.number + 1));




  /* --- patterns ---------------------------------------------------------
   *
   * Cases and MatchQ read the shape; DeleteCases throws away what
   * answers to it, FreeQ asks whether it is anywhere at all, and
   * ReplaceAll is the /. operator under its own name.
   */

  if ((name_is (name, "Cases", NULL) || name_is (name, "DeleteCases", NULL) ||
       name_is (name, "MatchQ", NULL)) && args->len >= 2)
    {
      gboolean keeping = !name_is (name, "DeleteCases", NULL);

      if (name_is (name, "MatchQ", NULL))
        return m42_value_number (value_matches (s, ARG (0), ARG (1)));

      /* Cases[expr, shape, All] looks the whole way down a tree rather
       * than along a list. */
      if (args->len > 2 && ARG (0)->kind != M42_VALUE_LIST && keeping)
        {
          g_autoptr (M42Node) tree = value_to_node (ARG (0));
          const M42Node *shape = ARG (1)->kind == M42_VALUE_EXPR ? ARG (1)->u.expr : NULL;
          g_autoptr (GPtrArray) found = NULL;
          M42Value *out;

          if (tree == NULL || shape == NULL)
            return m42_value_error ("Cases wants an expression and a shape");
          found = m42_node_collect (tree, shape, pattern_test, s);
          out = m42_value_list_new ();
          for (guint i = 0; i < found->len; i++)
            m42_value_list_append (out, expr_result (m42_node_copy (g_ptr_array_index (found, i))));
          return out;
        }

      if (ARG (0)->kind != M42_VALUE_LIST)
        return m42_value_error ("%s wants a list to look through", name);
      {
        M42Value *out = m42_value_list_new ();

        for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
          {
            M42Value *e = m42_value_list_nth (ARG (0), i);

            if (value_matches (s, e, ARG (1)) == keeping)
              m42_value_list_append (out, m42_value_ref (e));
          }
        return out;
      }
    }

  if (name_is (name, "FreeQ", NULL) && args->len == 2)
    {
      g_autoptr (M42Node) tree = value_to_node (ARG (0));
      const M42Node *shape = ARG (1)->kind == M42_VALUE_EXPR ? ARG (1)->u.expr : NULL;
      g_autoptr (M42Node) literal = shape == NULL ? value_to_node (ARG (1)) : NULL;

      if (tree == NULL)
        return m42_value_error ("FreeQ wants an expression to look through");
      if (shape == NULL)
        shape = literal;
      if (shape == NULL)
        return m42_value_error ("FreeQ wants something to look for");
      return m42_value_number (!m42_node_contains (tree, shape, pattern_test, s));
    }

  if ((name_is (name, "ReplaceAll", NULL) || name_is (name, "ReplaceRepeated", NULL)) &&
      args->len == 2)
    return replace_by_rules (s, ARG (0), ARG (1),
                             name_is (name, "ReplaceRepeated", NULL) ? 128 : 1);

  /* --- more ways to take a list apart ---------------------------------- */

  if (name_is (name, "SortBy", "sortby") && args->len == 2 &&
      ARG (0)->kind == M42_VALUE_LIST)
    {
      guint n = m42_value_list_length (ARG (0));
      g_autofree double *keys = g_new (double, n);
      g_autofree guint *order = g_new (guint, n);
      M42Value *out;

      for (guint i = 0; i < n; i++)
        {
          GPtrArray *one = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
          g_autoptr (M42Value) key = NULL;

          g_ptr_array_add (one, m42_value_ref (m42_value_list_nth (ARG (0), i)));
          key = apply_callable (s, ARG (1), one);
          g_ptr_array_unref (one);
          if (!value_number (key, &keys[i]))
            return m42_value_error ("SortBy: the function must give a number");
          order[i] = i;
        }
      /* A steady sort, so that ties keep the order they came in. */
      for (guint i = 1; i < n; i++)
        for (guint j = i; j > 0 && keys[order[j - 1]] > keys[order[j]]; j--)
          {
            guint t = order[j];

            order[j] = order[j - 1];
            order[j - 1] = t;
          }
      out = m42_value_list_new ();
      for (guint i = 0; i < n; i++)
        m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), order[i])));
      return out;
    }

  if (name_is (name, "Ordering", "sortindex") && args->len == 1 &&
      m42_value_is_vector (ARG (0)))
    {
      guint n = m42_value_list_length (ARG (0));
      g_autofree guint *order = g_new (guint, n);
      M42Value *out;

      for (guint i = 0; i < n; i++)
        order[i] = i;
      for (guint i = 1; i < n; i++)
        for (guint j = i;
             j > 0 && m42_value_list_nth (ARG (0), order[j - 1])->u.number >
                      m42_value_list_nth (ARG (0), order[j])->u.number;
             j--)
          {
            guint t = order[j];

            order[j] = order[j - 1];
            order[j - 1] = t;
          }
      out = m42_value_list_new ();
      for (guint i = 0; i < n; i++)
        m42_value_list_append (out, m42_value_number (order[i] + 1));
      return out;
    }

  if (name_is (name, "Split", NULL) && args->len == 1 && ARG (0)->kind == M42_VALUE_LIST)
    {
      /* Runs of equal neighbours, each run its own list. */
      guint n = m42_value_list_length (ARG (0));
      M42Value *out = m42_value_list_new ();
      M42Value *run = NULL;

      for (guint i = 0; i < n; i++)
        {
          g_autofree char *here = m42_value_to_string (m42_value_list_nth (ARG (0), i));
          gboolean same = FALSE;

          if (run != NULL)
            {
              g_autofree char *before = m42_value_to_string (
                m42_value_list_nth (ARG (0), i - 1));

              same = strcmp (here, before) == 0;
            }
          if (!same)
            {
              run = m42_value_list_new ();
              m42_value_list_append (out, run);
            }
          m42_value_list_append (run, m42_value_ref (m42_value_list_nth (ARG (0), i)));
        }
      return out;
    }

  if (name_is (name, "DeleteDuplicates", "unique") && args->len == 1 &&
      ARG (0)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();
      g_autoptr (GPtrArray) seen = g_ptr_array_new_with_free_func (g_free);

      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          M42Value *e = m42_value_list_nth (ARG (0), i);
          g_autofree char *key = m42_value_to_string (e);
          gboolean already = FALSE;

          for (guint k = 0; k < seen->len; k++)
            if (strcmp (g_ptr_array_index (seen, k), key) == 0)
              already = TRUE;
          if (already)
            continue;
          g_ptr_array_add (seen, g_strdup (key));
          m42_value_list_append (out, m42_value_ref (e));
        }
      return out;
    }

  if (name_is (name, "MemberQ", "ismember") && args->len == 2 &&
      ARG (0)->kind == M42_VALUE_LIST)
    {
      g_autofree char *wanted = m42_value_to_string (ARG (1));

      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          g_autofree char *here = m42_value_to_string (m42_value_list_nth (ARG (0), i));

          if (strcmp (here, wanted) == 0)
            return m42_value_number (1);
        }
      return m42_value_number (0);
    }

  if (name_is (name, "MapThread", NULL) && args->len == 2 &&
      ARG (1)->kind == M42_VALUE_LIST && m42_value_list_length (ARG (1)) > 0)
    {
      /* f applied across the lists, one item from each at a time. */
      guint lists = m42_value_list_length (ARG (1));
      guint n;
      M42Value *out;

      /* Every one of them has to be a list before its length can be
       * asked for.  This used to ask the first one for its length
       * before looking, so MapThread[f, {3, 4}] walked into the
       * length of the number 3. */
      for (guint k = 0; k < lists; k++)
        if (m42_value_list_nth (ARG (1), k)->kind != M42_VALUE_LIST)
          return m42_value_error ("MapThread wants a list of lists");
      n = m42_value_list_length (m42_value_list_nth (ARG (1), 0));
      for (guint k = 1; k < lists; k++)
        if (m42_value_list_length (m42_value_list_nth (ARG (1), k)) != n)
          return m42_value_error ("MapThread wants lists of the same length");
      out = m42_value_list_new ();

      for (guint i = 0; i < n; i++)
        {
          GPtrArray *one = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
          M42Value *r;

          for (guint k = 0; k < lists; k++)
            g_ptr_array_add (one, m42_value_ref (
              m42_value_list_nth (m42_value_list_nth (ARG (1), k), i)));
          r = apply_callable (s, ARG (0), one);
          g_ptr_array_unref (one);
          if (is_error (r))
            {
              m42_value_unref (out);
              return r;
            }
          m42_value_list_append (out, r);
        }
      return out;
    }

  if (name_is (name, "Outer", NULL) && args->len == 3 &&
      ARG (1)->kind == M42_VALUE_LIST && ARG (2)->kind == M42_VALUE_LIST)
    {
      /* Every pairing of the two lists, one row for each item of the
       * first. */
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < m42_value_list_length (ARG (1)); i++)
        {
          M42Value *row = m42_value_list_new ();

          for (guint j = 0; j < m42_value_list_length (ARG (2)); j++)
            {
              GPtrArray *two = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
              M42Value *r;

              g_ptr_array_add (two, m42_value_ref (m42_value_list_nth (ARG (1), i)));
              g_ptr_array_add (two, m42_value_ref (m42_value_list_nth (ARG (2), j)));
              r = apply_callable (s, ARG (0), two);
              g_ptr_array_unref (two);
              if (is_error (r))
                {
                  m42_value_unref (row);
                  m42_value_unref (out);
                  return r;
                }
              m42_value_list_append (row, r);
            }
          m42_value_list_append (out, row);
        }
      return out;
    }

  if (name_is (name, "Array", NULL) && args->len == 2 && is_num (ARG (1)))
    {
      int n = (int) ARG (1)->u.number;
      M42Value *out;

      if (n < 0 || n > 100000)
        return m42_value_error ("Array: that many is out of range");
      out = m42_value_list_new ();
      for (int i = 1; i <= n; i++)
        {
          GPtrArray *one = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
          M42Value *r;

          g_ptr_array_add (one, m42_value_number (i));
          r = apply_callable (s, ARG (0), one);
          g_ptr_array_unref (one);
          if (is_error (r))
            {
              m42_value_unref (out);
              return r;
            }
          m42_value_list_append (out, r);
        }
      return out;
    }

  if (name_is (name, "Tuples", NULL) && args->len == 2 &&
      ARG (0)->kind == M42_VALUE_LIST && is_num (ARG (1)))
    {
      guint n = m42_value_list_length (ARG (0));
      int k = (int) ARG (1)->u.number;
      M42Value *out;
      double total = pow (n, k);

      if (k < 0 || total > 100000)
        return m42_value_error ("Tuples: that many would be too many");
      out = m42_value_list_new ();
      for (guint index = 0; index < (guint) total; index++)
        {
          M42Value *one = m42_value_list_new ();
          guint left = index;

          for (int place = 0; place < k; place++)
            {
              m42_value_list_append (one, m42_value_ref (m42_value_list_nth (ARG (0), left % n)));
              left /= n;
            }
          m42_value_list_append (out, one);
        }
      return out;
    }

  if (name_is (name, "Riffle", NULL) && args->len == 2 && ARG (0)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          if (i > 0)
            m42_value_list_append (out, m42_value_ref (ARG (1)));
          m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i)));
        }
      return out;
    }

  if ((name_is (name, "RotateLeft", NULL) || name_is (name, "RotateRight", "circshift")) &&
      args->len >= 1 && ARG (0)->kind == M42_VALUE_LIST)
    {
      guint n = m42_value_list_length (ARG (0));
      int by = args->len == 2 && is_num (ARG (1)) ? (int) ARG (1)->u.number : 1;
      M42Value *out = m42_value_list_new ();

      if (n == 0)
        return out;
      if (name[6] == 'R' || name[0] == 'c')
        by = -by;
      for (guint i = 0; i < n; i++)
        {
          int from = ((int) i + by) % (int) n;

          if (from < 0)
            from += n;
          m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), from)));
        }
      return out;
    }

  if (name_is (name, "Most", NULL) && args->len == 1 && ARG (0)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();
      guint n = m42_value_list_length (ARG (0));

      for (guint i = 0; i + 1 < n; i++)
        m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i)));
      return out;
    }

  if (name_is (name, "Prepend", NULL) && args->len == 2 && ARG (0)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();

      m42_value_list_append (out, m42_value_ref (ARG (1)));
      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i)));
      return out;
    }

  if (name_is (name, "Delete", NULL) && args->len == 2 && ARG (0)->kind == M42_VALUE_LIST &&
      is_num (ARG (1)))
    {
      guint n = m42_value_list_length (ARG (0));
      int which = (int) ARG (1)->u.number;
      M42Value *out = m42_value_list_new ();

      if (which < 0)
        which = (int) n + which + 1;
      for (guint i = 0; i < n; i++)
        if ((int) i + 1 != which)
          m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i)));
      return out;
    }

  if (name_is (name, "Insert", NULL) && args->len == 3 && ARG (0)->kind == M42_VALUE_LIST &&
      is_num (ARG (2)))
    {
      guint n = m42_value_list_length (ARG (0));
      int where = (int) ARG (2)->u.number;
      M42Value *out = m42_value_list_new ();

      if (where < 0)
        where = (int) n + where + 2;
      for (guint i = 0; i <= n; i++)
        {
          if ((int) i + 1 == where)
            m42_value_list_append (out, m42_value_ref (ARG (1)));
          if (i < n)
            m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i)));
        }
      return out;
    }

  if (name_is (name, "meshgrid", NULL) && args->len >= 1 && m42_value_is_vector (ARG (0)))
    {
      /* {X, Y}: the two grids MATLAB makes out of two ranges. */
      M42Value *xs = ARG (0);
      M42Value *ys = args->len == 2 && m42_value_is_vector (ARG (1)) ? ARG (1) : ARG (0);
      guint nx = m42_value_list_length (xs), ny = m42_value_list_length (ys);
      M42Value *grid_x = m42_value_list_new ();
      M42Value *grid_y = m42_value_list_new ();
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < ny; i++)
        {
          M42Value *row_x = m42_value_list_new ();
          M42Value *row_y = m42_value_list_new ();

          for (guint j = 0; j < nx; j++)
            {
              m42_value_list_append (row_x, m42_value_ref (m42_value_list_nth (xs, j)));
              m42_value_list_append (row_y, m42_value_ref (m42_value_list_nth (ys, i)));
            }
          m42_value_list_append (grid_x, row_x);
          m42_value_list_append (grid_y, row_y);
        }
      m42_value_list_append (out, grid_x);
      m42_value_list_append (out, grid_y);
      return out;
    }

  /* --- statistics ------------------------------------------------------ */

  if ((name_is (name, "Correlation", "corrcoef") || name_is (name, "Covariance", "cov")) &&
      args->len == 2 && m42_value_is_vector (ARG (0)) && m42_value_is_vector (ARG (1)))
    {
      guint n = m42_value_list_length (ARG (0));
      double mean_x = 0, mean_y = 0, sxy = 0, sxx = 0, syy = 0;

      if (n != m42_value_list_length (ARG (1)) || n < 2)
        return m42_value_error ("%s wants two lists of the same length", name);
      for (guint i = 0; i < n; i++)
        {
          mean_x += m42_value_list_nth (ARG (0), i)->u.number;
          mean_y += m42_value_list_nth (ARG (1), i)->u.number;
        }
      mean_x /= n;
      mean_y /= n;
      for (guint i = 0; i < n; i++)
        {
          double dx = m42_value_list_nth (ARG (0), i)->u.number - mean_x;
          double dy = m42_value_list_nth (ARG (1), i)->u.number - mean_y;

          sxy += dx * dy;
          sxx += dx * dx;
          syy += dy * dy;
        }
      if (name[0] == 'C' && name[1] == 'o' && name[2] == 'v')
        return m42_value_real (sxy / (n - 1));
      if (name[0] == 'c' && name[1] == 'o' && name[2] == 'v')
        return m42_value_real (sxy / (n - 1));
      if (sxx < 1e-300 || syy < 1e-300)
        return m42_value_error ("%s: a list with no spread has no correlation", name);
      return m42_value_real (sxy / sqrt (sxx * syy));
    }

  if (name_is (name, "Quantile", "quantile") && args->len == 2 && is_num (ARG (1)))
    {
      g_autoptr (GArray) xs = g_array_new (FALSE, FALSE, sizeof (double));
      double p = ARG (1)->u.number;
      guint place;

      if (!collect_numbers (args, xs) || xs->len < 2)
        return m42_value_error ("%s wants a list of numbers and a fraction", name);
      g_array_set_size (xs, xs->len - 1);          /* the fraction is not data */
      if (p < 0 || p > 1)
        return m42_value_error ("%s: the fraction must be between nothing and one", name);
      g_array_sort (xs, compare_doubles);
      place = (guint) ceil (p * xs->len);
      if (place < 1)
        place = 1;
      if (place > xs->len)
        place = xs->len;
      return m42_value_number (g_array_index (xs, double, place - 1));
    }

  if ((name_is (name, "Mode", "mode") || name_is (name, "Skewness", "skewness") ||
       name_is (name, "Kurtosis", "kurtosis") || name_is (name, "RootMeanSquare", "rms")) &&
      args->len == 1)
    {
      g_autoptr (GArray) xs = g_array_new (FALSE, FALSE, sizeof (double));
      double mean = 0, variance = 0, third = 0, fourth = 0, squares = 0;
      guint n;

      if (!collect_numbers (args, xs) || xs->len == 0)
        return m42_value_error ("%s expects numbers", name);
      n = xs->len;
      for (guint i = 0; i < n; i++)
        {
          double x = g_array_index (xs, double, i);

          mean += x;
          squares += x * x;
        }
      mean /= n;
      for (guint i = 0; i < n; i++)
        {
          double d = g_array_index (xs, double, i) - mean;

          variance += d * d;
          third += d * d * d;
          fourth += d * d * d * d;
        }
      variance /= n;

      if (name_is (name, "RootMeanSquare", "rms"))
        return m42_value_real (sqrt (squares / n));
      if (name_is (name, "Mode", "mode"))
        {
          /* The value that turns up most often, the smallest of them
           * when several tie. */
          double best = g_array_index (xs, double, 0);
          guint most = 0;

          g_array_sort (xs, compare_doubles);
          for (guint i = 0; i < n; i++)
            {
              guint count = 0;

              for (guint j = 0; j < n; j++)
                if (g_array_index (xs, double, j) == g_array_index (xs, double, i))
                  count++;
              if (count > most)
                {
                  most = count;
                  best = g_array_index (xs, double, i);
                }
            }
          return m42_value_number (best);
        }
      if (variance < 1e-300)
        return m42_value_error ("%s: a list with no spread has none", name);
      if (name_is (name, "Skewness", "skewness"))
        return m42_value_real (third / n / pow (variance, 1.5));
      return m42_value_real (fourth / n / (variance * variance));
    }

  /* --- distributions ---------------------------------------------------
   *
   * A distribution is written as it is in Mathematica --
   * NormalDistribution[0, 1] -- which arrives here as a symbol with
   * arguments, and PDF, CDF and RandomVariate read it.
   */
  if ((name_is (name, "PDF", NULL) || name_is (name, "CDF", NULL) ||
       name_is (name, "RandomVariate", NULL) || name_is (name, "Mean", "mean") ||
       name_is (name, "Variance", "var")) && args->len >= 1 &&
      ARG (0)->kind == M42_VALUE_EXPR && ARG (0)->u.expr->kind == M42_NODE_CALL)
    {
      const M42Node *d = ARG (0)->u.expr;
      const char *kind = d->name;
      double p[3] = { 0, 1, 0 };
      double x = 0;
      gboolean have_x = args->len >= 2 && is_numeric (ARG (1));

      for (guint i = 0; i < d->children->len && i < 3; i++)
        if (!constant_fold (m42_node_child (d, i), &p[i]))
          return m42_value_error ("%s: that distribution has something odd in it", name);
      if (have_x)
        x = as_double (ARG (1));

      if (name_is (name, "Mean", "mean") || name_is (name, "Variance", "var"))
        {
          gboolean want_mean = name[0] == 'M' || name[0] == 'm';

          if (!strcmp (kind, "NormalDistribution"))
            return m42_value_real (want_mean ? p[0] : p[1] * p[1]);
          if (!strcmp (kind, "UniformDistribution"))
            return m42_value_real (want_mean ? (p[0] + p[1]) / 2
                                             : (p[1] - p[0]) * (p[1] - p[0]) / 12);
          if (!strcmp (kind, "ExponentialDistribution"))
            return m42_value_real (want_mean ? 1 / p[0] : 1 / (p[0] * p[0]));
          if (!strcmp (kind, "PoissonDistribution"))
            return m42_value_real (p[0]);
          if (!strcmp (kind, "BinomialDistribution"))
            return m42_value_real (want_mean ? p[0] * p[1] : p[0] * p[1] * (1 - p[1]));
          return m42_value_error ("%s: math42 does not know that distribution", name);
        }

      if (name_is (name, "RandomVariate", NULL))
        {
          int count = args->len >= 2 && is_num (ARG (1)) ? (int) ARG (1)->u.number : 1;
          M42Value *out = count > 1 ? m42_value_list_new () : NULL;

          if (count < 1 || count > 100000)
            return m42_value_error ("RandomVariate: that many is out of range");
          for (int i = 0; i < count; i++)
            {
              double draw;

              if (!strcmp (kind, "NormalDistribution"))
                draw = p[0] + p[1] * sqrt (-2 * log (g_random_double_range (1e-12, 1))) *
                       cos (2 * G_PI * g_random_double ());
              else if (!strcmp (kind, "UniformDistribution"))
                draw = g_random_double_range (p[0], p[1]);
              else if (!strcmp (kind, "ExponentialDistribution"))
                draw = -log (g_random_double_range (1e-12, 1)) / p[0];
              else if (!strcmp (kind, "PoissonDistribution"))
                {
                  /* Knuth's way: multiply until the product falls below
                   * e to the minus lambda. */
                  double limit = exp (-p[0]), product = 1;
                  int k = 0;

                  do
                    {
                      k++;
                      product *= g_random_double ();
                    }
                  while (product > limit && k < 100000);
                  draw = k - 1;
                }
              else if (!strcmp (kind, "BinomialDistribution"))
                {
                  int successes = 0;

                  for (int t = 0; t < (int) p[0]; t++)
                    successes += g_random_double () < p[1];
                  draw = successes;
                }
              else
                {
                  g_clear_pointer (&out, m42_value_unref);
                  return m42_value_error ("RandomVariate: math42 does not know that distribution");
                }

              if (out == NULL)
                return m42_value_real (draw);
              m42_value_list_append (out, m42_value_real (draw));
            }
          return out;
        }

      if (!have_x)
        return m42_value_error ("%s wants a distribution and a value", name);

      {
        gboolean want_pdf = name[0] == 'P' && name[1] == 'D';

        if (!strcmp (kind, "NormalDistribution"))
          {
            double z = (x - p[0]) / p[1];

            return m42_value_real (want_pdf
              ? exp (-z * z / 2) / (p[1] * sqrt (2 * G_PI))
              : 0.5 * (1 + erf (z / sqrt (2))));
          }
        if (!strcmp (kind, "UniformDistribution"))
          {
            if (want_pdf)
              return m42_value_real (x >= p[0] && x <= p[1] ? 1 / (p[1] - p[0]) : 0);
            return m42_value_real (x < p[0] ? 0 : x > p[1] ? 1 : (x - p[0]) / (p[1] - p[0]));
          }
        if (!strcmp (kind, "ExponentialDistribution"))
          {
            if (x < 0)
              return m42_value_number (0);
            return m42_value_real (want_pdf ? p[0] * exp (-p[0] * x) : 1 - exp (-p[0] * x));
          }
        if (!strcmp (kind, "PoissonDistribution"))
          {
            int k = (int) round (x);
            double term = exp (-p[0]), total = term;

            if (k < 0)
              return m42_value_number (0);
            for (int i = 1; i <= k; i++)
              {
                term *= p[0] / i;
                total += term;
              }
            return m42_value_real (want_pdf ? term : total);
          }
        if (!strcmp (kind, "BinomialDistribution"))
          {
            int k = (int) round (x), n = (int) p[0];
            double q = p[1], total = 0, term;

            if (k < 0 || k > n)
              return m42_value_number (0);
            term = pow (1 - q, n);
            for (int i = 0; i <= k; i++)
              {
                if (i > 0)
                  term = term * (n - i + 1) / i * q / (1 - q);
                total += term;
              }
            return m42_value_real (want_pdf ? term : total);
          }
        return m42_value_error ("%s: math42 does not know that distribution", name);
      }
    }

  /* MATLAB's names for the normal distribution. */
  if ((name_is (name, "normpdf", NULL) || name_is (name, "normcdf", NULL) ||
       name_is (name, "norminv", NULL)) && args->len >= 1 && is_num (ARG (0)))
    {
      double x = ARG (0)->u.number;
      double mu = args->len >= 2 && is_num (ARG (1)) ? ARG (1)->u.number : 0;
      double sigma = args->len >= 3 && is_num (ARG (2)) ? ARG (2)->u.number : 1;

      if (sigma <= 0)
        return m42_value_error ("%s: the spread must be more than nothing", name);
      if (name[4] == 'p')
        {
          double z = (x - mu) / sigma;

          return m42_value_real (exp (-z * z / 2) / (sigma * sqrt (2 * G_PI)));
        }
      if (name[4] == 'c')
        return m42_value_real (0.5 * (1 + erf ((x - mu) / (sigma * sqrt (2)))));
      {
        /* The value below which a given share of the distribution
         * falls, found by closing in on it. */
        double lo = mu - 40 * sigma, hi = mu + 40 * sigma;

        if (x <= 0 || x >= 1)
          return m42_value_error ("norminv wants a share between nothing and one");
        for (int i = 0; i < 200; i++)
          {
            double mid = (lo + hi) / 2;
            double share = 0.5 * (1 + erf ((mid - mu) / (sigma * sqrt (2))));

            if (share < x)
              lo = mid;
            else
              hi = mid;
          }
        return m42_value_real ((lo + hi) / 2);
      }
    }

  /* --- numbers from a curve -------------------------------------------- */

  if ((name_is (name, "trapz", NULL) || name_is (name, "cumtrapz", NULL)) && args->len >= 1)
    {
      gboolean running = name[0] == 'c';
      M42Value *xs = args->len == 2 ? ARG (0) : NULL;
      M42Value *ys = args->len == 2 ? ARG (1) : ARG (0);
      guint n;
      double total = 0;
      M42Value *out = running ? m42_value_list_new () : NULL;

      if (!m42_value_is_vector (ys) || (xs != NULL && !m42_value_is_vector (xs)))
        return m42_value_error ("%s wants a list of numbers", name);
      n = m42_value_list_length (ys);
      if (xs != NULL && m42_value_list_length (xs) != n)
        return m42_value_error ("%s wants two lists of the same length", name);

      if (running)
        m42_value_list_append (out, m42_value_number (0));
      for (guint i = 1; i < n; i++)
        {
          double step = xs != NULL
            ? m42_value_list_nth (xs, i)->u.number - m42_value_list_nth (xs, i - 1)->u.number
            : 1;

          total += step * (m42_value_list_nth (ys, i)->u.number +
                           m42_value_list_nth (ys, i - 1)->u.number) / 2;
          if (running)
            m42_value_list_append (out, m42_value_real (total));
        }
      return running ? out : m42_value_number (fabs (total - round (total)) < 1e-12
                                               ? round (total) : total);
    }

  /* MATLAB's gradient of a list, which is not Mathematica's
   * Differences: the one gives a slope at every point and the other
   * the step between them, and this used to answer to both names. */
  if (name_is (name, "gradient", NULL) && args->len == 1 &&
      m42_value_is_vector (ARG (0)) && m42_value_list_length (ARG (0)) >= 2)
    {
      /* The slope at each point: one-sided at the ends, the middle
       * difference in between, as MATLAB gives it. */
      guint n = m42_value_list_length (ARG (0));
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < n; i++)
        {
          double slope;

          if (i == 0)
            slope = m42_value_list_nth (ARG (0), 1)->u.number -
                    m42_value_list_nth (ARG (0), 0)->u.number;
          else if (i == n - 1)
            slope = m42_value_list_nth (ARG (0), n - 1)->u.number -
                    m42_value_list_nth (ARG (0), n - 2)->u.number;
          else
            slope = (m42_value_list_nth (ARG (0), i + 1)->u.number -
                     m42_value_list_nth (ARG (0), i - 1)->u.number) / 2;
          m42_value_list_append (out, m42_value_number (slope));
        }
      return out;
    }

  if ((name_is (name, "polyder", NULL) || name_is (name, "polyint", NULL)) &&
      args->len == 1 && m42_value_is_vector (ARG (0)))
    {
      /* Highest power first, as MATLAB keeps a polynomial. */
      guint n = m42_value_list_length (ARG (0));
      M42Value *out = m42_value_list_new ();

      if (name[4] == 'd')
        {
          for (guint i = 0; i + 1 < n; i++)
            m42_value_list_append (out, m42_value_number (
              m42_value_list_nth (ARG (0), i)->u.number * (n - 1 - i)));
          if (n < 2)
            m42_value_list_append (out, m42_value_number (0));
          return out;
        }
      for (guint i = 0; i < n; i++)
        m42_value_list_append (out, m42_value_rational (
          (gint64) m42_value_list_nth (ARG (0), i)->u.number, (gint64) (n - i)));
      m42_value_list_append (out, m42_value_number (0));
      return out;
    }

  /* --- whole numbers, as a discrete course takes them ----------------- */

  if (name_is (name, "PowerMod", "powermod") && args->len == 3)
    {
      double base, power, modulus;
      M42Value *err;
      gint64 acc = 1, b, e, m;

      if (!need_number (ARG (0), name, &base, &err) ||
          !need_number (ARG (1), name, &power, &err) ||
          !need_number (ARG (2), name, &modulus, &err))
        return err;
      if (modulus < 1 || power < 0 || fabs (base) > 1e15)
        return m42_value_error ("%s wants whole numbers, with a positive modulus", name);

      /* Squaring as it goes, so that big powers stay small. */
      m = (gint64) modulus;
      b = ((gint64) base % m + m) % m;
      e = (gint64) power;
      while (e > 0)
        {
          if (e & 1)
            acc = (__int128) acc * b % m;
          b = (__int128) b * b % m;
          e >>= 1;
        }
      return m42_value_number ((double) acc);
    }

  if ((name_is (name, "ExtendedGCD", "gcdex") || name_is (name, "ModularInverse", "modinv")) &&
      args->len == 2)
    {
      double x, y;
      M42Value *err;
      gint64 old_r, r, old_s, s_, old_t, t;

      if (!need_number (ARG (0), name, &x, &err) || !need_number (ARG (1), name, &y, &err))
        return err;
      old_r = (gint64) x;
      r = (gint64) y;
      old_s = 1; s_ = 0;
      old_t = 0; t = 1;
      while (r != 0)
        {
          gint64 q = old_r / r, tmp;

          tmp = old_r - q * r; old_r = r; r = tmp;
          tmp = old_s - q * s_; old_s = s_; s_ = tmp;
          tmp = old_t - q * t; old_t = t; t = tmp;
        }

      if (name_is (name, "ModularInverse", "modinv"))
        {
          gint64 m = (gint64) y;

          if (old_r != 1 && old_r != -1)
            return m42_value_error ("%s: %g and %g have a factor in common", name, x, y);
          return m42_value_number ((double) (((old_s % m) + m) % m));
        }
      {
        /* {g, {s, t}} with s x + t y = g, as Mathematica gives it. */
        M42Value *out = m42_value_list_new ();
        M42Value *pair = m42_value_list_new ();

        m42_value_list_append (out, m42_value_number ((double) (old_r < 0 ? -old_r : old_r)));
        m42_value_list_append (pair, m42_value_number ((double) (old_r < 0 ? -old_s : old_s)));
        m42_value_list_append (pair, m42_value_number ((double) (old_r < 0 ? -old_t : old_t)));
        m42_value_list_append (out, pair);
        return out;
      }
    }

  if (name_is (name, "ChineseRemainder", "crt") && args->len == 2 &&
      m42_value_is_vector (ARG (0)) && m42_value_is_vector (ARG (1)))
    {
      guint n = m42_value_list_length (ARG (0));
      gint64 x = 0, product = 1;

      if (n != m42_value_list_length (ARG (1)))
        return m42_value_error ("%s wants as many remainders as moduli", name);
      for (guint i = 0; i < n; i++)
        product *= (gint64) m42_value_list_nth (ARG (1), i)->u.number;
      if (product <= 0 || product > (gint64) 1e15)
        return m42_value_error ("%s: those moduli multiply to too much", name);

      /* One step at a time, searching within what is settled so far. */
      {
        gint64 settled = 1;

        for (guint i = 0; i < n; i++)
          {
            gint64 m = (gint64) m42_value_list_nth (ARG (1), i)->u.number;
            gint64 want = (gint64) m42_value_list_nth (ARG (0), i)->u.number;
            gint64 tries = 0;

            want = ((want % m) + m) % m;
            while (x % m != want)
              {
                x += settled;
                if (++tries > m)
                  return m42_value_error ("%s: those congruences cannot all hold", name);
              }
            settled *= m;
          }
      }
      return m42_value_number ((double) x);
    }

  if ((name_is (name, "FactorInteger", "factor") || name_is (name, "Divisors", "divisors") ||
       name_is (name, "EulerPhi", "totient") || name_is (name, "NextPrime", NULL) ||
       name_is (name, "PrimePi", NULL)) && args->len == 1 && is_num (ARG (0)))
    {
      double x = ARG (0)->u.number;
      gint64 n = (gint64) x;

      if (x != floor (x) || n < 1 || n > 100000000)
        return m42_value_error ("%s wants a whole number under a hundred million", name);

      if (name_is (name, "FactorInteger", "factor"))
        {
          M42Value *out = m42_value_list_new ();
          gint64 left = n;

          for (gint64 d = 2; d * d <= left; d++)
            {
              int count = 0;

              while (left % d == 0)
                {
                  left /= d;
                  count++;
                }
              if (count > 0)
                {
                  M42Value *pair = m42_value_list_new ();

                  m42_value_list_append (pair, m42_value_number ((double) d));
                  m42_value_list_append (pair, m42_value_number (count));
                  m42_value_list_append (out, pair);
                }
            }
          if (left > 1)
            {
              M42Value *pair = m42_value_list_new ();

              m42_value_list_append (pair, m42_value_number ((double) left));
              m42_value_list_append (pair, m42_value_number (1));
              m42_value_list_append (out, pair);
            }
          return out;
        }

      if (name_is (name, "Divisors", "divisors"))
        {
          M42Value *out = m42_value_list_new ();

          for (gint64 d = 1; d <= n; d++)
            if (n % d == 0)
              m42_value_list_append (out, m42_value_number ((double) d));
          return out;
        }

      if (name_is (name, "EulerPhi", "totient"))
        {
          gint64 left = n, result = n;

          for (gint64 d = 2; d * d <= left; d++)
            if (left % d == 0)
              {
                while (left % d == 0)
                  left /= d;
                result -= result / d;
              }
          if (left > 1)
            result -= result / left;
          return m42_value_number ((double) result);
        }

      if (name_is (name, "NextPrime", NULL))
        {
          for (gint64 candidate = n + 1; candidate < n + 1000000; candidate++)
            {
              gboolean prime = candidate >= 2;

              for (gint64 d = 2; prime && d * d <= candidate; d++)
                if (candidate % d == 0)
                  prime = FALSE;
              if (prime)
                return m42_value_number ((double) candidate);
            }
          return m42_value_error ("NextPrime: nothing found near there");
        }

      {
        /* PrimePi: how many primes there are up to n. */
        gint64 count = 0;

        if (n > 2000000)
          return m42_value_error ("PrimePi: that is further than math42 will count");
        for (gint64 candidate = 2; candidate <= n; candidate++)
          {
            gboolean prime = TRUE;

            for (gint64 d = 2; d * d <= candidate; d++)
              if (candidate % d == 0)
                {
                  prime = FALSE;
                  break;
                }
            count += prime;
          }
        return m42_value_number ((double) count);
      }
    }

  /* --- counting ------------------------------------------------------- */

  if (name_is (name, "Subsets", "powerset") && args->len >= 1 &&
      ARG (0)->kind == M42_VALUE_LIST)
    {
      guint n = m42_value_list_length (ARG (0));
      int want = args->len == 2 && is_num (ARG (1)) ? (int) ARG (1)->u.number : -1;
      M42Value *out;

      if (n > 16)
        return m42_value_error ("Subsets: sixteen things at most");
      out = m42_value_list_new ();
      for (guint mask = 0; mask < (1u << n); mask++)
        {
          M42Value *subset;
          int size = 0;

          for (guint i = 0; i < n; i++)
            size += (mask >> i) & 1;
          if (want >= 0 && size != want)
            continue;
          subset = m42_value_list_new ();
          for (guint i = 0; i < n; i++)
            if ((mask >> i) & 1)
              m42_value_list_append (subset, m42_value_ref (m42_value_list_nth (ARG (0), i)));
          m42_value_list_append (out, subset);
        }
      return out;
    }

  if (name_is (name, "Permutations", "perms") && args->len == 1 &&
      ARG (0)->kind == M42_VALUE_LIST)
    {
      guint n = m42_value_list_length (ARG (0));
      g_autofree guint *order = NULL;
      M42Value *out;

      if (n > 8)
        return m42_value_error ("Permutations: eight things at most");
      order = g_new (guint, n);
      for (guint i = 0; i < n; i++)
        order[i] = i;
      out = m42_value_list_new ();

      for (;;)
        {
          M42Value *one = m42_value_list_new ();
          guint i, j;

          for (i = 0; i < n; i++)
            m42_value_list_append (one, m42_value_ref (m42_value_list_nth (ARG (0), order[i])));
          m42_value_list_append (out, one);

          /* The next arrangement, in the order a dictionary would use. */
          if (n < 2)
            break;
          i = n - 1;
          while (i > 0 && order[i - 1] >= order[i])
            i--;
          if (i == 0)
            break;
          j = n - 1;
          while (order[j] <= order[i - 1])
            j--;
          {
            guint t = order[i - 1];
            order[i - 1] = order[j];
            order[j] = t;
          }
          for (guint a = i, b = n - 1; a < b; a++, b--)
            {
              guint t = order[a];
              order[a] = order[b];
              order[b] = t;
            }
        }
      return out;
    }

  if (name_is (name, "CatalanNumber", NULL) && args->len == 1 && is_num (ARG (0)))
    {
      double n = ARG (0)->u.number, acc = 1;

      if (n < 0 || n > 30 || n != floor (n))
        return m42_value_error ("CatalanNumber wants a whole number up to thirty");
      for (int i = 0; i < (int) n; i++)
        acc = acc * 2 * (2 * i + 1) / (i + 2);
      return m42_value_number (round (acc));
    }

  if ((name_is (name, "StirlingS2", NULL) || name_is (name, "BellB", NULL)) &&
      args->len >= 1 && is_num (ARG (0)))
    {
      /* The number of ways a set of n things falls into k parts, and
       * the total over every k. */
      int n = (int) ARG (0)->u.number;
      int k = args->len == 2 && is_num (ARG (1)) ? (int) ARG (1)->u.number : -1;
      double table[41][41] = { { 0 } };

      if (n < 0 || n > 40 || (k >= 0 && k > n))
        return m42_value_error ("%s wants whole numbers up to forty", name);
      table[0][0] = 1;
      for (int i = 1; i <= n; i++)
        for (int j = 1; j <= i; j++)
          table[i][j] = j * table[i - 1][j] + table[i - 1][j - 1];

      if (k >= 0)
        return m42_value_number (table[n][k]);
      {
        double total = 0;

        for (int j = 0; j <= n; j++)
          total += table[n][j];
        return m42_value_number (n == 0 ? 1 : total);
      }
    }

  if (name_is (name, "Multinomial", NULL) && args->len >= 1)
    {
      g_autoptr (GArray) xs = g_array_new (FALSE, FALSE, sizeof (double));
      double total = 0, acc = 1;

      if (!collect_numbers (args, xs))
        return m42_value_error ("Multinomial wants whole numbers");
      for (guint i = 0; i < xs->len; i++)
        {
          double k = g_array_index (xs, double, i);

          for (int j = 1; j <= (int) k; j++)
            {
              total++;
              acc = acc * total / j;
            }
        }
      return m42_value_number (round (acc));
    }

  /* --- logic ---------------------------------------------------------- */

  if (name_is (name, "Xor", "xor") && args->len == 2 && is_num (ARG (0)) && is_num (ARG (1)))
    return m42_value_number ((ARG (0)->u.number != 0) != (ARG (1)->u.number != 0));
  if (name_is (name, "Implies", NULL) && args->len == 2 && is_num (ARG (0)) && is_num (ARG (1)))
    return m42_value_number (ARG (0)->u.number == 0 || ARG (1)->u.number != 0);
  if (name_is (name, "Nand", NULL) && args->len == 2 && is_num (ARG (0)) && is_num (ARG (1)))
    return m42_value_number (!(ARG (0)->u.number != 0 && ARG (1)->u.number != 0));

  /* --- graphs, held as the matrix of what joins what ------------------ */

  if ((name_is (name, "VertexDegrees", "degree") ||
       name_is (name, "TransitiveClosure", NULL) ||
       name_is (name, "ConnectedComponents", NULL)) && args->len == 1)
    {
      guint n, cols;
      g_autoptr (M42Matrix) g = NULL;

      if (!m42_value_is_matrix (ARG (0), &n, &cols) || n != cols)
        return m42_value_error ("%s wants a square matrix of what joins what", name);
      g = m42_matrix_from_value (ARG (0), FALSE);

      if (name_is (name, "VertexDegrees", "degree"))
        {
          M42Value *out = m42_value_list_new ();

          for (guint i = 0; i < n; i++)
            {
              double sum = 0;

              for (guint j = 0; j < n; j++)
                sum += *m42_matrix_at (g, i, j) != 0 ? 1 : 0;
              m42_value_list_append (out, m42_value_number (sum));
            }
          return out;
        }

      if (name_is (name, "TransitiveClosure", NULL))
        {
          /* Warshall: everything reachable, in three loops. */
          for (guint k = 0; k < n; k++)
            for (guint i = 0; i < n; i++)
              for (guint j = 0; j < n; j++)
                if (*m42_matrix_at (g, i, k) != 0 && *m42_matrix_at (g, k, j) != 0)
                  *m42_matrix_at (g, i, j) = 1;
          for (guint i = 0; i < n * n; i++)
            g->a[i] = g->a[i] != 0 ? 1 : 0;
          return m42_matrix_to_value (g, FALSE);
        }

      {
        /* The pieces the graph falls into. */
        g_autofree int *label = g_new0 (int, n);
        M42Value *out = m42_value_list_new ();
        int next = 0;

        for (guint start = 0; start < n; start++)
          {
            g_autoptr (GArray) queue = NULL;
            M42Value *piece;

            if (label[start] != 0)
              continue;
            next++;
            queue = g_array_new (FALSE, FALSE, sizeof (guint));
            g_array_append_val (queue, start);
            label[start] = next;

            for (guint at = 0; at < queue->len; at++)
              {
                guint v = g_array_index (queue, guint, at);

                for (guint w = 0; w < n; w++)
                  if (label[w] == 0 &&
                      (*m42_matrix_at (g, v, w) != 0 || *m42_matrix_at (g, w, v) != 0))
                    {
                      label[w] = next;
                      g_array_append_val (queue, w);
                    }
              }

            piece = m42_value_list_new ();
            for (guint v = 0; v < n; v++)
              if (label[v] == next)
                m42_value_list_append (piece, m42_value_number (v + 1));
            m42_value_list_append (out, piece);
          }
        return out;
      }
    }

  if (name_is (name, "GraphDistance", "shortestpath") && args->len == 3 &&
      is_num (ARG (1)) && is_num (ARG (2)))
    {
      guint n, cols;
      g_autoptr (M42Matrix) g = NULL;
      guint from, to;
      g_autofree double *best = NULL;

      if (!m42_value_is_matrix (ARG (0), &n, &cols) || n != cols)
        return m42_value_error ("%s wants a square matrix of what joins what", name);
      g = m42_matrix_from_value (ARG (0), FALSE);
      from = (guint) ARG (1)->u.number;
      to = (guint) ARG (2)->u.number;
      if (from < 1 || to < 1 || from > n || to > n)
        return m42_value_error ("%s: there is no such vertex", name);

      /* Dijkstra: the nearest unsettled vertex each time round, with
       * the weight taken from the matrix. */
      {
        g_autofree gboolean *settled = g_new0 (gboolean, n);

        best = g_new (double, n);
        for (guint i = 0; i < n; i++)
          best[i] = INFINITY;
        best[from - 1] = 0;

        for (guint step = 0; step < n; step++)
          {
            guint at = n;
            double least = INFINITY;

            for (guint i = 0; i < n; i++)
              if (!settled[i] && best[i] < least)
                {
                  least = best[i];
                  at = i;
                }
            if (at == n)
              break;
            settled[at] = TRUE;

            for (guint w = 0; w < n; w++)
              {
                double edge = *m42_matrix_at (g, at, w);

                if (edge == 0 || settled[w])
                  continue;
                if (best[at] + edge < best[w])
                  best[w] = best[at] + edge;
              }
          }
      }
      return isfinite (best[to - 1]) ? m42_value_number (best[to - 1])
                                     : m42_value_real (INFINITY);
    }

  /* --- the list and matrix toolbox both languages carry around ------- */

  if (name_is (name, "Take", NULL) && args->len == 2 &&
      ARG (0)->kind == M42_VALUE_LIST && is_num (ARG (1)))
    {
      guint n = m42_value_list_length (ARG (0));
      int k = (int) ARG (1)->u.number;
      M42Value *out = m42_value_list_new ();

      /* A negative count takes from the end, as it does in Mathematica. */
      for (guint i = 0; i < n; i++)
        if (k >= 0 ? (int) i < k : (int) i >= (int) n + k)
          m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i)));
      return out;
    }
  if (name_is (name, "Drop", NULL) && args->len == 2 &&
      ARG (0)->kind == M42_VALUE_LIST && is_num (ARG (1)))
    {
      guint n = m42_value_list_length (ARG (0));
      int k = (int) ARG (1)->u.number;
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < n; i++)
        if (k >= 0 ? (int) i >= k : (int) i < (int) n + k)
          m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i)));
      return out;
    }
  if (name_is (name, "Partition", NULL) && args->len == 2 &&
      ARG (0)->kind == M42_VALUE_LIST && is_num (ARG (1)))
    {
      guint n = m42_value_list_length (ARG (0));
      guint k = (guint) MAX (1.0, ARG (1)->u.number);
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i + k <= n; i += k)
        {
          M42Value *part = m42_value_list_new ();
          for (guint j = 0; j < k; j++)
            m42_value_list_append (part, m42_value_ref (m42_value_list_nth (ARG (0), i + j)));
          m42_value_list_append (out, part);
        }
      return out;
    }

  /* Counting and matching, by what a value prints as. */
  if ((name_is (name, "Count", NULL) || name_is (name, "Position", "find")) &&
      args->len >= 1 && ARG (0)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();
      guint found = 0;
      g_autofree char *wanted = args->len == 2 ? m42_value_to_string (ARG (1)) : NULL;

      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          M42Value *e = m42_value_list_nth (ARG (0), i);
          gboolean hit;

          if (wanted != NULL)
            hit = value_matches (s, e, ARG (1));
          else
            hit = !is_num (e) || e->u.number != 0;    /* find: what is not zero */

          if (hit)
            {
              found++;
              m42_value_list_append (out, m42_value_number (i + 1));
            }
        }
      if (name_is (name, "Count", NULL))
        {
          m42_value_unref (out);
          return m42_value_number (found);
        }
      return out;
    }
  if ((name_is (name, "AnyTrue", "any") || name_is (name, "AllTrue", "all")) &&
      args->len == 1 && ARG (0)->kind == M42_VALUE_LIST)
    {
      /* AllTrue and all against AnyTrue and any: the second letter is
       * what tells them apart in either spelling. */
      gboolean want_all = g_ascii_tolower (name[1]) == 'l';
      gboolean acc = want_all;

      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          M42Value *e = m42_value_list_nth (ARG (0), i);
          gboolean truth = is_num (e) ? e->u.number != 0 : TRUE;

          acc = want_all ? (acc && truth) : (acc || truth);
        }
      return m42_value_number (acc);
    }

  /* Set operations, on numbers. */
  if ((name_is (name, "Union", "union") || name_is (name, "Intersection", "intersect") ||
       name_is (name, "Complement", "setdiff")) && args->len == 2 &&
      ARG (0)->kind == M42_VALUE_LIST && ARG (1)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();
      g_autoptr (GPtrArray) seen = g_ptr_array_new_with_free_func (g_free);
      gboolean want_union = name[0] == 'U' || name[0] == 'u';
      gboolean want_diff = name[0] == 'C' || name[0] == 's';

      for (guint side = 0; side < (want_union ? 2u : 1u); side++)
        for (guint i = 0; i < m42_value_list_length (ARG (side)); i++)
          {
            M42Value *e = m42_value_list_nth (ARG (side), i);
            g_autofree char *key = m42_value_to_string (e);
            gboolean in_other = FALSE, already = FALSE;

            for (guint k = 0; k < m42_value_list_length (ARG (1)); k++)
              {
                g_autofree char *other = m42_value_to_string (m42_value_list_nth (ARG (1), k));
                if (strcmp (other, key) == 0)
                  in_other = TRUE;
              }
            for (guint k = 0; k < seen->len; k++)
              if (strcmp (g_ptr_array_index (seen, k), key) == 0)
                already = TRUE;

            if (already || (!want_union && (want_diff ? in_other : !in_other)))
              continue;
            g_ptr_array_add (seen, g_strdup (key));
            m42_value_list_append (out, m42_value_ref (e));
          }
      return out;
    }
  if (name_is (name, "Tally", NULL) && args->len == 1 && ARG (0)->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();
      g_autoptr (GPtrArray) keys = g_ptr_array_new_with_free_func (g_free);

      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          M42Value *e = m42_value_list_nth (ARG (0), i);
          g_autofree char *key = m42_value_to_string (e);
          gboolean already = FALSE;
          guint count = 0;

          for (guint k = 0; k < keys->len; k++)
            if (strcmp (g_ptr_array_index (keys, k), key) == 0)
              already = TRUE;
          if (already)
            continue;
          for (guint k = 0; k < m42_value_list_length (ARG (0)); k++)
            {
              g_autofree char *other = m42_value_to_string (m42_value_list_nth (ARG (0), k));
              if (strcmp (other, key) == 0)
                count++;
            }
          g_ptr_array_add (keys, g_strdup (key));
          {
            M42Value *pair = m42_value_list_new ();
            m42_value_list_append (pair, m42_value_ref (e));
            m42_value_list_append (pair, m42_value_number (count));
            m42_value_list_append (out, pair);
          }
        }
      return out;
    }

  /* Running totals and differences. */
  if ((name_is (name, "Accumulate", "cumsum") || name_is (name, "Differences", "diff")) &&
      args->len == 1 && m42_value_is_vector (ARG (0)))
    {
      M42Value *out = m42_value_list_new ();
      guint n = m42_value_list_length (ARG (0));
      g_autoptr (M42Value) acc = m42_value_number (0);

      /* Added up and taken apart through the arithmetic that keeps an
       * exact number exact, so a list of thirds stays thirds. */
      for (guint i = 0; i < n; i++)
        {
          M42Value *x = m42_value_list_nth (ARG (0), i);

          if (name[0] == 'A' || name[0] == 'c')
            {
              M42Value *next = map2 (M42_TOK_PLUS, acc, x);

              m42_value_unref (acc);
              acc = next;
              m42_value_list_append (out, m42_value_ref (acc));
            }
          else if (i > 0)
            m42_value_list_append (out, map2 (M42_TOK_MINUS, x,
                                              m42_value_list_nth (ARG (0), i - 1)));
        }
      return out;
    }

  /* Matrices by shape. */
  if (name_is (name, "ArrayReshape", "reshape") && args->len >= 2)
    {
      g_autoptr (GArray) xs = g_array_new (FALSE, FALSE, sizeof (double));
      GPtrArray *first = g_ptr_array_new ();
      double r, c;
      M42Value *out;

      g_ptr_array_add (first, ARG (0));
      if (!collect_numbers (first, xs))
        {
          g_ptr_array_unref (first);
          return m42_value_error ("%s expects numbers", name);
        }
      g_ptr_array_unref (first);

      if (args->len == 3 && is_num (ARG (1)) && is_num (ARG (2)))
        {
          r = ARG (1)->u.number;
          c = ARG (2)->u.number;
        }
      else if (args->len == 2 && m42_value_is_vector (ARG (1)) &&
               m42_value_list_length (ARG (1)) == 2)
        {
          r = m42_value_list_nth (ARG (1), 0)->u.number;
          c = m42_value_list_nth (ARG (1), 1)->u.number;
        }
      else
        return m42_value_error ("%s expects a shape, as {rows, columns}", name);

      if (r < 1 || c < 1 || r * c > 1e6)
        return m42_value_error ("%s: that shape is out of range", name);
      out = m42_value_list_new ();
      for (guint i = 0; i < (guint) r; i++)
        {
          M42Value *row = m42_value_list_new ();
          for (guint j = 0; j < (guint) c; j++)
            {
              guint k = i * (guint) c + j;
              m42_value_list_append (row, m42_value_number (k < xs->len ? g_array_index (xs, double, k) : 0));
            }
          m42_value_list_append (out, row);
        }
      return out;
    }
  if (name_is (name, "DiagonalMatrix", "diag") && args->len == 1)
    {
      guint rows, cols;

      /* diag of a vector makes a matrix; diag of a matrix takes its
       * diagonal out, as MATLAB has it both ways. */
      if (m42_value_is_matrix (ARG (0), &rows, &cols))
        {
          M42Value *out = m42_value_list_new ();
          for (guint i = 0; i < MIN (rows, cols); i++)
            m42_value_list_append (out, m42_value_ref (m42_value_list_nth (m42_value_list_nth (ARG (0), i), i)));
          return out;
        }
      if (m42_value_is_vector (ARG (0)))
        {
          guint n = m42_value_list_length (ARG (0));
          M42Value *out = m42_value_list_new ();

          for (guint i = 0; i < n; i++)
            {
              M42Value *row = m42_value_list_new ();
              for (guint j = 0; j < n; j++)
                m42_value_list_append (row, i == j
                                       ? m42_value_ref (m42_value_list_nth (ARG (0), i))
                                       : m42_value_number (0));
              m42_value_list_append (out, row);
            }
          return out;
        }
      return m42_value_error ("%s expects a vector or a matrix", name);
    }

  /* Whole numbers and the pieces of a real one. */
  if (name_is (name, "IntegerPart", "fix") && args->len == 1 && is_num (ARG (0)))
    return m42_value_number (trunc (ARG (0)->u.number));
  if (name_is (name, "FractionalPart", NULL) && args->len == 1 && is_num (ARG (0)))
    return m42_value_number (ARG (0)->u.number - trunc (ARG (0)->u.number));
  if (name_is (name, "Quotient", "idivide") && args->len == 2 &&
      is_num (ARG (0)) && is_num (ARG (1)) && ARG (1)->u.number != 0)
    return m42_value_number (floor (ARG (0)->u.number / ARG (1)->u.number));
  if (name_is (name, "UnitStep", "heaviside") && args->len == 1 && is_num (ARG (0)))
    return m42_value_number (ARG (0)->u.number >= 0);
  if (name_is (name, "Boole", NULL) && args->len == 1 && is_num (ARG (0)))
    return m42_value_number (ARG (0)->u.number != 0);
  if (name_is (name, "Clip", NULL) && args->len == 2 && is_num (ARG (0)) &&
      m42_value_is_vector (ARG (1)) && m42_value_list_length (ARG (1)) == 2)
    return m42_value_number (CLAMP (ARG (0)->u.number,
                                    m42_value_list_nth (ARG (1), 0)->u.number,
                                    m42_value_list_nth (ARG (1), 1)->u.number));

  /* MATLAB's sprintf, and the two that write what it makes.  The
   * conversions it knows are the ones people use: d, i, f, e, g and s,
   * with a width and precision if they are given. */
  if ((name_is (name, "sprintf", NULL) || name_is (name, "fprintf", NULL) ||
       name_is (name, "printf", NULL) || name_is (name, "StringForm", NULL)) &&
      args->len >= 1 && ARG (0)->kind == M42_VALUE_STRING)
    {
      const char *at = ARG (0)->u.string;
      g_autoptr (GString) out = g_string_new (NULL);
      guint next = 1;

      while (*at != 0)
        {
          const char *start = at;
          char spec[32];
          gsize len = 0;
          char kind;

          if (*at != '%')
            {
              g_string_append_c (out, *at++);
              continue;
            }
          if (at[1] == '%')
            {
              g_string_append_c (out, '%');
              at += 2;
              continue;
            }

          /* Copy the flags, width and precision through as they are. */
          at++;
          while (*at != 0 && strchr ("-+ 0#.0123456789", *at) != NULL && len < sizeof spec - 4)
            spec[len++] = *at++;
          kind = *at;
          if (kind == 0)
            {
              g_string_append (out, start);
              break;
            }
          at++;

          if (next >= args->len)
            {
              g_string_append_len (out, start, at - start);
              continue;
            }

          {
            g_autofree char *piece = format_one (spec, len, kind, ARG (next++));
            g_string_append (out, piece);
          }
        }

      if (name[0] == 's' || name[0] == 'S')
        return m42_value_string (out->str);
      g_string_append (s->printed, out->str);
      return m42_value_null ();
    }

  /* --- what a thing is ------------------------------------------------ */

  if (name_is (name, "Head", "class") && args->len == 1)
    {
      M42Value *v = ARG (0);
      const char *head = "Symbol";

      switch (v->kind)
        {
        case M42_VALUE_NUMBER:
          head = v->exact ? (v->den == 1 ? "Integer" : "Rational") : "Real";
          break;
        case M42_VALUE_BIGINT: head = "Integer"; break;
        case M42_VALUE_COMPLEX: head = "Complex"; break;
        case M42_VALUE_STRING:  head = "String"; break;
        case M42_VALUE_LIST:    head = "List"; break;
        case M42_VALUE_FUNC:    head = "Function"; break;
        case M42_VALUE_PLOT:    head = "Graphics"; break;
        case M42_VALUE_NULL:    head = "Null"; break;
        case M42_VALUE_ERROR:   head = "Error"; break;
        case M42_VALUE_EXPR:
          head = v->u.expr->kind == M42_NODE_IDENT ? "Symbol" : "Expression";
          break;
        }
      return m42_value_string (head);
    }

  if (args->len == 1)
    {
      M42Value *v = ARG (0);

      if (name_is (name, "NumberQ", "isnumeric") || name_is (name, "NumericQ", NULL))
        return m42_value_number (is_numeric (v));
      if (name_is (name, "IntegerQ", "isinteger"))
        return m42_value_number (is_whole (v));

      /* Which side of nothing a number is on, which is mostly wanted
       * as the question in a pattern: x_?Positive. */
      if (name_is (name, "Positive", NULL) || name_is (name, "Negative", NULL) ||
          name_is (name, "NonNegative", NULL) || name_is (name, "NonPositive", NULL))
        {
          double x;

          if (!value_number (v, &x))
            return m42_value_number (0);
          if (name_is (name, "Positive", NULL))     return m42_value_number (x > 0);
          if (name_is (name, "Negative", NULL))     return m42_value_number (x < 0);
          if (name_is (name, "NonNegative", NULL))  return m42_value_number (x >= 0);
          return m42_value_number (x <= 0);
        }
      if (name_is (name, "ListQ", "iscell") || name_is (name, "VectorQ", "isvector"))
        return m42_value_number (v->kind == M42_VALUE_LIST);
      if (name_is (name, "MatrixQ", "ismatrix"))
        return m42_value_number (m42_value_is_matrix (v, NULL, NULL));
      if (name_is (name, "StringQ", "ischar"))
        return m42_value_number (v->kind == M42_VALUE_STRING);
      if (name_is (name, "TrueQ", "logical"))
        return m42_value_number (is_num (v) && v->u.number != 0);
      if (name_is (name, "EvenQ", NULL) || name_is (name, "OddQ", NULL))
        {
          gboolean even = is_num (v) && v->exact && v->den == 1 && v->num % 2 == 0;
          return m42_value_number (name[0] == 'E' ? even
                                   : (is_num (v) && v->exact && v->den == 1 && !even));
        }
      if (name_is (name, "isempty", NULL))
        return m42_value_number (v->kind == M42_VALUE_LIST ? m42_value_list_length (v) == 0
                                 : v->kind == M42_VALUE_STRING ? *v->u.string == 0 : FALSE);
      if (name_is (name, "ndims", NULL))
        {
          guint depth = 0;
          const M42Value *w = v;
          while (w->kind == M42_VALUE_LIST && m42_value_list_length (w) > 0)
            {
              depth++;
              w = m42_value_list_nth (w, 0);
            }
          return m42_value_number (depth);
        }
      if (name_is (name, "Chop", NULL) && is_num (v))
        return m42_value_number (fabs (v->u.number) < 1e-10 ? 0 : v->u.number);
    }

  if (name_is (name, "SameQ", "isequal") && args->len == 2)
    {
      g_autofree char *a = m42_value_to_string (ARG (0));
      g_autofree char *b = m42_value_to_string (ARG (1));
      return m42_value_number (strcmp (a, b) == 0);
    }

  /* Round[x, dx] rounds to a multiple of dx, as Mathematica does. */
  if (name_is (name, "Round", "round") && args->len == 2 &&
      is_num (ARG (0)) && is_num (ARG (1)) && ARG (1)->u.number != 0)
    return m42_value_number (round (ARG (0)->u.number / ARG (1)->u.number) * ARG (1)->u.number);

  /* --- lists, the MATLAB way ------------------------------------------ */

  if ((name_is (name, "fliplr", NULL) || name_is (name, "flipud", NULL)) &&
      args->len == 1 && ARG (0)->kind == M42_VALUE_LIST)
    {
      guint rows, cols;
      M42Value *out = m42_value_list_new ();

      /* flipud turns the rows around; fliplr turns each row around. */
      if (name[4] == 'u' || !m42_value_is_matrix (ARG (0), &rows, &cols))
        {
          for (guint i = m42_value_list_length (ARG (0)); i > 0; i--)
            m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i - 1)));
          return out;
        }
      for (guint i = 0; i < rows; i++)
        {
          M42Value *row = m42_value_list_new ();
          M42Value *from = m42_value_list_nth (ARG (0), i);

          for (guint j = cols; j > 0; j--)
            m42_value_list_append (row, m42_value_ref (m42_value_list_nth (from, j - 1)));
          m42_value_list_append (out, row);
        }
      return out;
    }

  /* --- the MATLAB names that were still missing --------------------------
   *
   * Most of them are a line of work over what is already here; what
   * they were missing was somewhere to be looked up.
   */

  /* --- reading and writing a file --------------------------------------- */

  if ((name_is (name, "Import", NULL) || name_is (name, "csvread", NULL) ||
       name_is (name, "readmatrix", NULL) || name_is (name, "ReadString", NULL)) &&
      args->len >= 1 && ARG (0)->kind == M42_VALUE_STRING)
    return import_file (ARG (0)->u.string, name);

  if ((name_is (name, "Export", NULL) || name_is (name, "csvwrite", NULL) ||
       name_is (name, "writematrix", NULL)) && args->len >= 2)
    {
      /* Export["file", data] the Mathematica way round, and MATLAB's
       * csvwrite("file", data) the same; writematrix(data, "file")
       * puts them the other way, so whichever is the string is the
       * name. */
      const M42Value *where = ARG (0)->kind == M42_VALUE_STRING ? ARG (0) : ARG (1);
      const M42Value *what = ARG (0)->kind == M42_VALUE_STRING ? ARG (1) : ARG (0);

      if (where->kind != M42_VALUE_STRING)
        return m42_value_error ("%s wants a file name", name);
      return export_file (where->u.string, what, name);
    }

  if (name_is (name, "nnz", NULL) && args->len == 1)
    {
      /* How many are not zero, as deep as the nesting goes. */
      GPtrArray *stack = g_ptr_array_new ();
      guint how_many = 0;

      g_ptr_array_add (stack, ARG (0));
      while (stack->len > 0)
        {
          const M42Value *v = g_ptr_array_index (stack, stack->len - 1);

          g_ptr_array_set_size (stack, stack->len - 1);
          if (v->kind == M42_VALUE_LIST)
            for (guint i = 0; i < m42_value_list_length (v); i++)
              g_ptr_array_add (stack, m42_value_list_nth (v, i));
          else if (!is_num (v) || v->u.number != 0)
            how_many++;
        }
      g_ptr_array_unref (stack);
      return m42_value_number (how_many);
    }

  if (name_is (name, "hypot", NULL) && args->len == 2)
    {
      double a, b;

      if (value_number (ARG (0), &a) && value_number (ARG (1), &b))
        return m42_value_real (hypot (a, b));
    }

  if (name_is (name, "nthroot", NULL) && args->len == 2)
    {
      double x, n;

      if (value_number (ARG (0), &x) && value_number (ARG (1), &n))
        {
          if (n == 0)
            return m42_value_error ("nthroot: there is no zeroth root");
          /* The real root of a negative number, when there is one. */
          if (x < 0 && fmod (n, 2) == 1)
            return m42_value_real (-pow (-x, 1 / n));
          return m42_value_real (pow (x, 1 / n));
        }
    }

  if ((name_is (name, "deg2rad", NULL) || name_is (name, "rad2deg", NULL)) &&
      args->len == 1)
    {
      double x;

      /* Pi is a symbol until it is asked for a number, and this is
       * asking. */
      if (value_number (ARG (0), &x))
        return m42_value_real (name[0] == 'd' ? x * G_PI / 180 : x * 180 / G_PI);
    }

  if (name_is (name, "primes", NULL) && args->len == 1 && is_num (ARG (0)))
    {
      /* Every prime up to n, by the sieve. */
      gint64 top = (gint64) ARG (0)->u.number;
      M42Value *out = m42_value_list_new ();
      g_autofree gboolean *composite = NULL;

      if (top < 2)
        return out;
      if (top > 5000000)
        return m42_value_error ("primes: that is further than math42 will go");
      composite = g_new0 (gboolean, top + 1);
      for (gint64 i = 2; i * i <= top; i++)
        if (!composite[i])
          for (gint64 k = i * i; k <= top; k += i)
            composite[k] = TRUE;
      for (gint64 i = 2; i <= top; i++)
        if (!composite[i])
          m42_value_list_append (out, m42_value_number ((double) i));
      return out;
    }

  /* The triangle above the diagonal, or below it, with the rest zero. */
  if ((name_is (name, "UpperTriangularize", "triu") ||
       name_is (name, "LowerTriangularize", "tril")) && args->len >= 1)
    {
      guint rows, cols;
      gboolean upper = name_is (name, "UpperTriangularize", "triu");
      gint64 offset = args->len > 1 && is_num (ARG (1)) ? (gint64) ARG (1)->u.number : 0;
      M42Value *out;

      if (!m42_value_is_matrix (ARG (0), &rows, &cols))
        return m42_value_error ("%s wants a matrix", name);
      out = m42_value_list_new ();
      for (guint i = 0; i < rows; i++)
        {
          M42Value *row = m42_value_list_new ();
          M42Value *was = m42_value_list_nth (ARG (0), i);

          for (guint k = 0; k < cols; k++)
            {
              gboolean keep = upper ? (gint64) k - (gint64) i >= offset
                                    : (gint64) k - (gint64) i <= offset;

              m42_value_list_append (row, keep ? m42_value_ref (m42_value_list_nth (was, k))
                                               : m42_value_number (0));
            }
          m42_value_list_append (out, row);
        }
      return out;
    }

  /* Every element of one matrix times the whole of the other. */
  if (name_is (name, "KroneckerProduct", "kron") && args->len == 2)
    {
      guint ar, ac, br, bc;
      M42Value *out;

      if (!m42_value_is_matrix (ARG (0), &ar, &ac) ||
          !m42_value_is_matrix (ARG (1), &br, &bc))
        return m42_value_error ("%s wants two matrices", name);
      out = m42_value_list_new ();
      for (guint i = 0; i < ar * br; i++)
        {
          M42Value *row = m42_value_list_new ();

          for (guint k = 0; k < ac * bc; k++)
            {
              M42Value *x = m42_value_list_nth (m42_value_list_nth (ARG (0), i / br), k / bc);
              M42Value *y = m42_value_list_nth (m42_value_list_nth (ARG (1), i % br), k % bc);

              m42_value_list_append (row, map2 (M42_TOK_STAR, x, y));
            }
          m42_value_list_append (out, row);
        }
      return out;
    }

  /* A matrix turned a quarter turn to the left. */
  if (name_is (name, "rot90", NULL) && args->len >= 1)
    {
      guint rows, cols;
      int turns = args->len > 1 && is_num (ARG (1)) ? (int) ARG (1)->u.number : 1;
      g_autoptr (M42Value) held = m42_value_ref (ARG (0));

      if (!m42_value_is_matrix (ARG (0), &rows, &cols))
        return m42_value_error ("rot90 wants a matrix");
      turns = ((turns % 4) + 4) % 4;
      for (int t = 0; t < turns; t++)
        {
          M42Value *turned = m42_value_list_new ();

          m42_value_is_matrix (held, &rows, &cols);
          for (guint k = cols; k > 0; k--)
            {
              M42Value *row = m42_value_list_new ();

              for (guint i = 0; i < rows; i++)
                m42_value_list_append (row,
                  m42_value_ref (m42_value_list_nth (m42_value_list_nth (held, i), k - 1)));
              m42_value_list_append (turned, row);
            }
          m42_value_unref (held);
          held = turned;
        }
      return m42_value_ref (held);
    }

  /* The rows of a matrix put in order of one of its columns. */
  if (name_is (name, "sortrows", NULL) && args->len >= 1 &&
      ARG (0)->kind == M42_VALUE_LIST)
    {
      guint rows, cols;
      gint64 by = args->len > 1 && is_num (ARG (1)) ? (gint64) ARG (1)->u.number : 1;
      g_autoptr (GPtrArray) order = g_ptr_array_new ();
      M42Value *out;

      if (!m42_value_is_matrix (ARG (0), &rows, &cols) || by < 1 || (guint) by > cols)
        return m42_value_error ("sortrows wants a matrix and a column in it");
      for (guint i = 0; i < rows; i++)
        g_ptr_array_add (order, m42_value_list_nth (ARG (0), i));

      /* Small matrices only, so the plainest sort will do. */
      for (guint i = 0; i + 1 < order->len; i++)
        for (guint k = 0; k + 1 < order->len - i; k++)
          {
            double x, y;

            if (value_number (m42_value_list_nth (g_ptr_array_index (order, k), by - 1), &x) &&
                value_number (m42_value_list_nth (g_ptr_array_index (order, k + 1), by - 1), &y) &&
                x > y)
              {
                gpointer swap = g_ptr_array_index (order, k);

                g_ptr_array_index (order, k) = g_ptr_array_index (order, k + 1);
                g_ptr_array_index (order, k + 1) = swap;
              }
          }
      out = m42_value_list_new ();
      for (guint i = 0; i < order->len; i++)
        m42_value_list_append (out, m42_value_ref (g_ptr_array_index (order, i)));
      return out;
    }

  /* A straight line drawn between the points either side, which is
   * what MATLAB means by interpolation unless it is told otherwise. */
  if (name_is (name, "Interpolation", NULL) && args->len == 1)
    return interpolation_function (s, ARG (0));

  if (name_is (name, "interp1", NULL) && args->len >= 3 &&
      m42_value_is_vector (ARG (0)) && m42_value_is_vector (ARG (1)))
    {
      guint n = m42_value_list_length (ARG (0));
      M42Value *out;
      gboolean many = ARG (2)->kind == M42_VALUE_LIST;
      guint asked = many ? m42_value_list_length (ARG (2)) : 1;

      if (m42_value_list_length (ARG (1)) != n || n < 2)
        return m42_value_error ("interp1 wants two lists of the same length");
      out = many ? m42_value_list_new () : NULL;
      for (guint q = 0; q < asked; q++)
        {
          const M42Value *where = many ? m42_value_list_nth (ARG (2), q) : ARG (2);
          double at, answer = NAN;

          if (!value_number (where, &at))
            return m42_value_error ("interp1 wants numbers to look up");
          for (guint i = 0; i + 1 < n; i++)
            {
              double x0, x1, y0, y1;

              if (!value_number (m42_value_list_nth (ARG (0), i), &x0) ||
                  !value_number (m42_value_list_nth (ARG (0), i + 1), &x1) ||
                  !value_number (m42_value_list_nth (ARG (1), i), &y0) ||
                  !value_number (m42_value_list_nth (ARG (1), i + 1), &y1))
                return m42_value_error ("interp1 wants numbers");
              if ((at >= x0 && at <= x1) || (at <= x0 && at >= x1))
                {
                  answer = x1 == x0 ? y0 : y0 + (y1 - y0) * (at - x0) / (x1 - x0);
                  break;
                }
            }
          /* Outside the points there is nothing to draw between. */
          if (many)
            m42_value_list_append (out, m42_value_real (answer));
          else
            return m42_value_real (answer);
        }
      return out;
    }

  if (name_is (name, "repmat", NULL) && args->len >= 2 && is_num (ARG (1)))
    {
      int times = (int) ARG (1)->u.number;
      M42Value *out = m42_value_list_new ();

      /* repmat(A, m, n) lays it out m down and n across; for a list
       * with no rows to it that is m n copies end to end. */
      if (args->len > 2 && is_num (ARG (2)))
        times *= (int) ARG (2)->u.number;
      if (times < 0 || times > 100000)
        return m42_value_error ("repmat: that many copies is out of range");
      for (int t = 0; t < times; t++)
        {
          if (ARG (0)->kind == M42_VALUE_LIST)
            for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
              m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), i)));
          else
            m42_value_list_append (out, m42_value_ref (ARG (0)));
        }
      return out;
    }

  if (name_is (name, "cumprod", NULL) && args->len == 1 && m42_value_is_vector (ARG (0)))
    {
      M42Value *out = m42_value_list_new ();
      double acc = 1;

      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          acc *= m42_value_list_nth (ARG (0), i)->u.number;
          m42_value_list_append (out, m42_value_number (acc));
        }
      return out;
    }

  if (name_is (name, "rem", NULL) && args->len == 2 && is_num (ARG (0)) && is_num (ARG (1)))
    return m42_value_number (fmod (ARG (0)->u.number, ARG (1)->u.number));

  if (name_is (name, "logspace", NULL) && args->len == 3 &&
      is_num (ARG (0)) && is_num (ARG (1)) && is_num (ARG (2)))
    {
      double a = ARG (0)->u.number, b = ARG (1)->u.number;
      int n = (int) ARG (2)->u.number;
      M42Value *out = m42_value_list_new ();

      if (n < 1 || n > 100000)
        return m42_value_error ("logspace: that many points is out of range");
      for (int i = 0; i < n; i++)
        m42_value_list_append (out, m42_value_real (pow (10, a + (b - a) * i / MAX (n - 1, 1))));
      return out;
    }

  if (name_is (name, "randn", NULL) && args->len <= 1)
    {
      /* Box-Muller, which turns two flat numbers into one normal one. */
      int n = args->len == 1 && is_num (ARG (0)) ? (int) ARG (0)->u.number : 0;
      M42Value *out;

      if (n <= 0)
        return m42_value_real (sqrt (-2 * log (g_random_double_range (1e-12, 1))) *
                               cos (2 * G_PI * g_random_double ()));
      out = m42_value_list_new ();
      for (int i = 0; i < MIN (n, 100000); i++)
        m42_value_list_append (out, m42_value_real (sqrt (-2 * log (g_random_double_range (1e-12, 1))) *
                                                    cos (2 * G_PI * g_random_double ())));
      return out;
    }

  if ((name_is (name, "RandomChoice", "randsample") || name_is (name, "RandomSample", NULL)) &&
      args->len >= 1 && ARG (0)->kind == M42_VALUE_LIST)
    {
      guint n = m42_value_list_length (ARG (0));
      int want = args->len == 2 && is_num (ARG (1)) ? (int) ARG (1)->u.number : 1;
      gboolean without = name[6] == 'S' || name[0] == 'r';
      M42Value *out;
      g_autofree gboolean *taken = g_new0 (gboolean, n + 1);

      if (n == 0)
        return m42_value_error ("%s: the list is empty", name);
      if (want < 1 || (without && (guint) want > n))
        return m42_value_error ("%s: cannot take that many", name);
      if (args->len == 1)
        return m42_value_ref (m42_value_list_nth (ARG (0), g_random_int_range (0, n)));

      out = m42_value_list_new ();
      for (int i = 0; i < want; i++)
        {
          guint k = g_random_int_range (0, n);
          while (without && taken[k])
            k = (k + 1) % n;
          taken[k] = TRUE;
          m42_value_list_append (out, m42_value_ref (m42_value_list_nth (ARG (0), k)));
        }
      return out;
    }

  /* --- polynomials, as MATLAB keeps them: highest power first --------- */

  if (name_is (name, "polyval", NULL) && args->len == 2 && m42_value_is_vector (ARG (0)))
    {
      guint n = m42_value_list_length (ARG (0));
      double acc = 0, x;
      M42Value *err;

      if (!need_number (ARG (1), name, &x, &err))
        return err;
      for (guint i = 0; i < n; i++)
        acc = acc * x + m42_value_list_nth (ARG (0), i)->u.number;
      return m42_value_number (acc);
    }

  if (name_is (name, "roots", NULL) && args->len == 1 && m42_value_is_vector (ARG (0)))
    {
      guint n = m42_value_list_length (ARG (0));
      g_autoptr (GArray) coeffs = g_array_new (FALSE, TRUE, sizeof (double));
      g_autoptr (GArray) found = g_array_new (FALSE, FALSE, sizeof (double _Complex));
      M42Value *out;

      /* MATLAB writes the highest power first; the solver wants it last. */
      for (guint i = 0; i < n; i++)
        {
          double c = m42_value_list_nth (ARG (0), n - 1 - i)->u.number;
          g_array_append_val (coeffs, c);
        }
      if (!polynomial_roots (coeffs, found))
        return m42_value_error ("roots: that is not a polynomial with roots to find");
      g_array_sort (found, compare_roots);
      out = m42_value_list_new ();
      for (guint i = 0; i < found->len; i++)
        {
          double _Complex r = g_array_index (found, double _Complex, i);
          m42_value_list_append (out, m42_value_complex (creal (r), cimag (r)));
        }
      return out;
    }

  if (name_is (name, "conv", NULL) && args->len == 2 &&
      m42_value_is_vector (ARG (0)) && m42_value_is_vector (ARG (1)))
    {
      guint na = m42_value_list_length (ARG (0)), nb = m42_value_list_length (ARG (1));
      g_autofree double *acc = g_new0 (double, na + nb - 1);
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < na; i++)
        for (guint j = 0; j < nb; j++)
          acc[i + j] += m42_value_list_nth (ARG (0), i)->u.number *
                        m42_value_list_nth (ARG (1), j)->u.number;
      for (guint i = 0; i < na + nb - 1; i++)
        m42_value_list_append (out, m42_value_number (acc[i]));
      return out;
    }

  if (name_is (name, "polyfit", "Fit") && args->len == 3 &&
      m42_value_is_vector (ARG (0)) && m42_value_is_vector (ARG (1)) && is_num (ARG (2)))
    {
      /* Least squares through the normal equations, which is enough for
       * the small degrees anyone fits by hand. */
      guint n = m42_value_list_length (ARG (0));
      int degree = (int) ARG (2)->u.number;
      guint m = degree + 1;
      g_autoptr (M42Matrix) ata = NULL;
      g_autoptr (M42Matrix) atb = NULL;
      g_autoptr (M42Matrix) sol = NULL;
      M42Value *out;

      if (degree < 0 || degree > 12 || n != m42_value_list_length (ARG (1)))
        return m42_value_error ("polyfit expects two lists of the same length and a small degree");
      if (n < m)
        return m42_value_error ("polyfit: not enough points for that degree");

      ata = m42_matrix_new (m, m);
      atb = m42_matrix_new (m, 1);
      for (guint k = 0; k < n; k++)
        {
          double x = m42_value_list_nth (ARG (0), k)->u.number;
          double y = m42_value_list_nth (ARG (1), k)->u.number;

          for (guint i = 0; i < m; i++)
            {
              for (guint j = 0; j < m; j++)
                *m42_matrix_at (ata, i, j) += pow (x, (double) (i + j));
              *m42_matrix_at (atb, i, 0) += y * pow (x, (double) i);
            }
        }
      sol = m42_matrix_solve (ata, atb);
      if (sol == NULL)
        return m42_value_error ("polyfit: those points do not settle on one answer");

      /* Highest power first, as MATLAB hands it back. */
      out = m42_value_list_new ();
      for (guint i = m; i > 0; i--)
        {
          double c = *m42_matrix_at (sol, i - 1, 0);
          m42_value_list_append (out, m42_value_real (fabs (c) < 1e-12 ? 0 : c));
        }
      return out;
    }

  if (name_is (name, "interp1", "Interpolation") && args->len == 3 &&
      m42_value_is_vector (ARG (0)) && m42_value_is_vector (ARG (1)) && is_num (ARG (2)))
    {
      guint n = m42_value_list_length (ARG (0));
      double x = ARG (2)->u.number;

      if (n != m42_value_list_length (ARG (1)) || n < 2)
        return m42_value_error ("interp1 expects two lists of the same length");
      for (guint i = 0; i + 1 < n; i++)
        {
          double x0 = m42_value_list_nth (ARG (0), i)->u.number;
          double x1 = m42_value_list_nth (ARG (0), i + 1)->u.number;

          if ((x >= x0 && x <= x1) || (x <= x0 && x >= x1))
            {
              double y0 = m42_value_list_nth (ARG (1), i)->u.number;
              double y1 = m42_value_list_nth (ARG (1), i + 1)->u.number;

              if (x1 == x0)
                return m42_value_number (y0);
              return m42_value_real (y0 + (y1 - y0) * (x - x0) / (x1 - x0));
            }
        }
      return m42_value_error ("interp1: %g is outside the points given", x);
    }

  /* --- strings -------------------------------------------------------- */

  if (name_is (name, "Characters", NULL) && args->len == 1 && ARG (0)->kind == M42_VALUE_STRING)
    {
      M42Value *out = m42_value_list_new ();
      const char *at = ARG (0)->u.string;

      while (*at != 0)
        {
          const char *next = g_utf8_next_char (at);
          g_autofree char *one = g_strndup (at, next - at);
          m42_value_list_append (out, m42_value_string (one));
          at = next;
        }
      return out;
    }

  if ((name_is (name, "StringTake", NULL) || name_is (name, "StringDrop", NULL)) &&
      args->len == 2 && ARG (0)->kind == M42_VALUE_STRING && is_num (ARG (1)))
    {
      glong len = g_utf8_strlen (ARG (0)->u.string, -1);
      glong k = (glong) ARG (1)->u.number;
      glong from, count;
      g_autofree char *piece = NULL;

      if (k >= 0)
        {
          from = name[6] == 'T' ? 0 : k;
          count = name[6] == 'T' ? MIN (k, len) : len - MIN (k, len);
        }
      else
        {
          from = name[6] == 'T' ? MAX (len + k, 0) : 0;
          count = name[6] == 'T' ? MIN (-k, len) : MAX (len + k, 0);
        }
      piece = g_utf8_substring (ARG (0)->u.string, from, from + count);
      return m42_value_string (piece);
    }

  if (name_is (name, "StringReverse", "fliplr") && args->len == 1 &&
      ARG (0)->kind == M42_VALUE_STRING)
    {
      g_autofree char *back = g_utf8_strreverse (ARG (0)->u.string, -1);
      return m42_value_string (back);
    }

  /* StringReplace[s, from, to] the MATLAB way, and Mathematica's
   * StringReplace[s, from -> to], with a list of rules if you like. */
  if (name_is (name, "StringReplace", "strrep") && args->len >= 2 &&
      ARG (0)->kind == M42_VALUE_STRING)
    {
      g_autofree char *text = g_strdup (ARG (0)->u.string);
      g_autoptr (GPtrArray) rules = g_ptr_array_new ();

      if (args->len == 3 && ARG (1)->kind == M42_VALUE_STRING &&
          ARG (2)->kind == M42_VALUE_STRING)
        {
          g_auto (GStrv) pieces = g_strsplit (text, ARG (1)->u.string, -1);

          if (ARG (1)->u.string[0] == 0)
            return m42_value_string (text);
          return m42_value_string (g_strjoinv (ARG (2)->u.string, pieces));
        }

      if (ARG (1)->kind == M42_VALUE_LIST)
        for (guint i = 0; i < m42_value_list_length (ARG (1)); i++)
          g_ptr_array_add (rules, m42_value_list_nth (ARG (1), i));
      else
        g_ptr_array_add (rules, ARG (1));

      for (guint i = 0; i < rules->len; i++)
        {
          const M42Value *rule = g_ptr_array_index (rules, i);
          const M42Node *from, *to;
          g_auto (GStrv) pieces = NULL;

          if (rule->kind != M42_VALUE_EXPR || rule->u.expr->kind != M42_NODE_RULE)
            return m42_value_error ("StringReplace wants \"a\" -> \"b\"");
          from = m42_node_child (rule->u.expr, 0);
          to = m42_node_child (rule->u.expr, 1);
          if (from->kind != M42_NODE_STRING || to->kind != M42_NODE_STRING ||
              from->name[0] == 0)
            return m42_value_error ("StringReplace wants two strings in its rule");
          pieces = g_strsplit (text, from->name, -1);
          g_free (text);
          text = g_strjoinv (to->name, pieces);
        }
      return m42_value_string (text);
    }

  /* The pieces of a list of strings with something between them. */
  if (name_is (name, "StringRiffle", "strjoin") && args->len >= 1 &&
      ARG (0)->kind == M42_VALUE_LIST)
    {
      const char *between = args->len == 2 && ARG (1)->kind == M42_VALUE_STRING
        ? ARG (1)->u.string : " ";
      g_autoptr (GString) out = g_string_new (NULL);

      for (guint i = 0; i < m42_value_list_length (ARG (0)); i++)
        {
          M42Value *e = m42_value_list_nth (ARG (0), i);
          g_autofree char *piece = e->kind == M42_VALUE_STRING ? g_strdup (e->u.string)
                                                               : m42_value_to_string (e);

          if (i > 0)
            g_string_append (out, between);
          g_string_append (out, piece);
        }
      return m42_value_string (out->str);
    }

  if (name_is (name, "StringTrim", "strtrim") && args->len == 1 &&
      ARG (0)->kind == M42_VALUE_STRING)
    {
      g_autofree char *copy = g_strdup (ARG (0)->u.string);

      return m42_value_string (g_strstrip (copy));
    }

  if ((name_is (name, "StringContainsQ", "contains") ||
       name_is (name, "StringCount", NULL) ||
       name_is (name, "StringStartsQ", "startsWith") ||
       name_is (name, "StringEndsQ", "endsWith")) &&
      args->len == 2 && ARG (0)->kind == M42_VALUE_STRING &&
      ARG (1)->kind == M42_VALUE_STRING)
    {
      const char *text = ARG (0)->u.string, *piece = ARG (1)->u.string;

      if (name_is (name, "StringStartsQ", "startsWith"))
        return m42_value_number (g_str_has_prefix (text, piece));
      if (name_is (name, "StringEndsQ", "endsWith"))
        return m42_value_number (g_str_has_suffix (text, piece));
      if (name_is (name, "StringContainsQ", "contains"))
        return m42_value_number (piece[0] == 0 || strstr (text, piece) != NULL);
      {
        /* How many times it appears, counting from where the last one
         * ended so that "aaa" holds one "aa" and not two. */
        const char *at = text;
        const char *found;
        guint how_many = 0;

        while (piece[0] != 0 && (found = strstr (at, piece)) != NULL)
          {
            how_many++;
            at = found + strlen (piece);
          }
        return m42_value_number (how_many);
      }
    }

  if ((name_is (name, "StringPadLeft", NULL) || name_is (name, "StringPadRight", NULL)) &&
      args->len >= 2 && ARG (0)->kind == M42_VALUE_STRING && is_num (ARG (1)))
    {
      const char *text = ARG (0)->u.string;
      long want = (long) ARG (1)->u.number;
      long have = (long) g_utf8_strlen (text, -1);
      char with = args->len > 2 && ARG (2)->kind == M42_VALUE_STRING &&
                  ARG (2)->u.string[0] != 0 ? ARG (2)->u.string[0] : ' ';
      g_autoptr (GString) out = g_string_new (NULL);

      if (want <= have)
        return m42_value_string (text);
      if (name_is (name, "StringPadRight", NULL))
        g_string_append (out, text);
      for (long i = have; i < want; i++)
        g_string_append_c (out, with);
      if (name_is (name, "StringPadLeft", NULL))
        g_string_append (out, text);
      return m42_value_string (out->str);
    }

  /* 255 in base 16 is ff, and the other way round. */
  if (name_is (name, "IntegerString", "dec2base") && args->len >= 1 && is_num (ARG (0)))
    {
      static const char DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";
      gint64 x = (gint64) ARG (0)->u.number;
      int base = args->len > 1 && is_num (ARG (1)) ? (int) ARG (1)->u.number : 10;
      char buffer[80];
      int at = (int) sizeof buffer - 1;
      gboolean negative = x < 0;

      if (base < 2 || base > 36)
        return m42_value_error ("IntegerString: the base is between 2 and 36");
      buffer[at] = 0;
      if (x == 0)
        buffer[--at] = DIGITS[0];
      for (gint64 left = negative ? -x : x; left > 0; left /= base)
        buffer[--at] = DIGITS[left % base];
      if (negative)
        buffer[--at] = '-';
      return m42_value_string (buffer + at);
    }

  if (name_is (name, "FromDigits", "base2dec") && args->len >= 1 &&
      ARG (0)->kind == M42_VALUE_STRING)
    {
      int base = args->len > 1 && is_num (ARG (1)) ? (int) ARG (1)->u.number : 10;
      const char *at = ARG (0)->u.string;
      gint64 total = 0;
      gboolean negative = *at == '-';

      if (base < 2 || base > 36)
        return m42_value_error ("FromDigits: the base is between 2 and 36");
      if (negative)
        at++;
      for (; *at != 0; at++)
        {
          int digit = g_ascii_isdigit (*at) ? *at - '0'
                    : g_ascii_isalpha (*at) ? g_ascii_tolower (*at) - 'a' + 10 : -1;

          if (digit < 0 || digit >= base)
            return m42_value_error ("FromDigits: %c is not a digit in base %d", *at, base);
          total = total * base + digit;
        }
      return m42_value_number (negative ? -total : total);
    }

  if (name_is (name, "StringSplit", "strsplit") && args->len >= 1 &&
      ARG (0)->kind == M42_VALUE_STRING)
    {
      const char *sep = args->len == 2 && ARG (1)->kind == M42_VALUE_STRING
        ? ARG (1)->u.string : " ";
      g_auto (GStrv) pieces = g_strsplit (ARG (0)->u.string, sep, -1);
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; pieces[i] != NULL; i++)
        if (*pieces[i] != 0)
          m42_value_list_append (out, m42_value_string (pieces[i]));
      return out;
    }

  if (name_is (name, "StringPosition", "strfind") && args->len == 2 &&
      ARG (0)->kind == M42_VALUE_STRING && ARG (1)->kind == M42_VALUE_STRING)
    {
      M42Value *out = m42_value_list_new ();
      const char *at = ARG (0)->u.string;
      const char *found;

      while ((found = strstr (at, ARG (1)->u.string)) != NULL && *ARG (1)->u.string != 0)
        {
          m42_value_list_append (out, m42_value_number (found - ARG (0)->u.string + 1));
          at = found + 1;
        }
      return out;
    }

  if (name_is (name, "strcmp", NULL) && args->len == 2 &&
      ARG (0)->kind == M42_VALUE_STRING && ARG (1)->kind == M42_VALUE_STRING)
    return m42_value_number (strcmp (ARG (0)->u.string, ARG (1)->u.string) == 0);

  if (name_is (name, "ToExpression", "str2num") && args->len == 1 &&
      ARG (0)->kind == M42_VALUE_STRING)
    {
      /* The string read as though it had been typed. */
      g_autofree char *error = NULL;
      g_autoptr (M42Node) tree = m42_parse (ARG (0)->u.string, &error);

      if (tree == NULL)
        return m42_value_error ("%s: %s", name, error);
      return eval (s, tree);
    }

  if (name_is (name, "str2double", NULL) && args->len == 1 &&
      ARG (0)->kind == M42_VALUE_STRING)
    return m42_value_real (g_ascii_strtod (ARG (0)->u.string, NULL));

  /* The discrete transform, under either name. */
  if (name_is (name, "Fourier", NULL))
    return discrete_fourier (args, FALSE, FALSE);
  if (name_is (name, "InverseFourier", NULL))
    return discrete_fourier (args, TRUE, FALSE);
  if (name_is (name, "fft", NULL))
    return discrete_fourier (args, FALSE, TRUE);
  if (name_is (name, "ifft", NULL))
    return discrete_fourier (args, TRUE, TRUE);

  /* What math42 knows about itself. */
  if (name_is (name, "Names", NULL) && args->len == 0)
    {
      M42Value *out = m42_value_list_new ();
      for (const M42Function *f = m42_functions (); f->name != NULL; f++)
        m42_value_list_append (out, m42_value_string (f->name));
      return out;
    }
  if (name_is (name, "Information", "Help") && args->len == 1)
    {
      const M42Function *f;
      const char *wanted = ARG (0)->kind == M42_VALUE_STRING ? ARG (0)->u.string : NULL;

      /* ?Sin passes the name itself, which arrives as a symbol. */
      if (wanted == NULL && ARG (0)->kind == M42_VALUE_EXPR &&
          ARG (0)->u.expr->kind == M42_NODE_IDENT)
        wanted = ARG (0)->u.expr->name;
      if (wanted == NULL)
        return m42_value_error ("%s expects the name of a function", name);

      f = m42_function_find (wanted);
      if (f == NULL)
        return m42_value_error ("math42 has nothing called %s", wanted);
      {
        g_autofree char *text = f->matlab != NULL
          ? g_strdup_printf ("%s -- %s (MATLAB: %s)", f->usage, f->summary, f->matlab)
          : g_strdup_printf ("%s -- %s", f->usage, f->summary);
        return m42_value_string (text);
      }
    }

  /* Strings, as far as a notebook needs them. */
  if (name_is (name, "StringJoin", "strcat") || name_is (name, "StringLength", "strlength") ||
      name_is (name, "ToString", "num2str") || name_is (name, "ToUpperCase", "upper") ||
      name_is (name, "ToLowerCase", "lower"))
    {
      if (name_is (name, "StringJoin", "strcat"))
        {
          g_autoptr (GString) joined = g_string_new (NULL);
          for (guint i = 0; i < args->len; i++)
            {
              if (ARG (i)->kind == M42_VALUE_STRING)
                g_string_append (joined, ARG (i)->u.string);
              else
                {
                  g_autofree char *text = m42_value_to_string (ARG (i));
                  g_string_append (joined, text);
                }
            }
          return m42_value_string (joined->str);
        }
      if (args->len != 1)
        return m42_value_error ("%s takes one argument", name);
      if (name_is (name, "ToString", "num2str"))
        {
          g_autofree char *text = m42_value_to_string (ARG (0));
          return m42_value_string (ARG (0)->kind == M42_VALUE_STRING ? ARG (0)->u.string : text);
        }
      if (ARG (0)->kind != M42_VALUE_STRING)
        return m42_value_error ("%s expects a string", name);
      if (name_is (name, "StringLength", "strlength"))
        return m42_value_number (g_utf8_strlen (ARG (0)->u.string, -1));
      {
        g_autofree char *changed = name_is (name, "ToUpperCase", "upper")
          ? g_utf8_strup (ARG (0)->u.string, -1)
          : g_utf8_strdown (ARG (0)->u.string, -1);
        return m42_value_string (changed);
      }
    }

  /* Span survives as itself when it is not indexing anything. */
  if (name_is (name, "Span", NULL))
    {
      M42Node *span = m42_node_new (M42_NODE_CALL);

      span->name = g_strdup ("Span");
      for (guint i = 0; i < args->len; i++)
        {
          M42Node *c = value_to_node (ARG (i));
          g_ptr_array_add (span->children, c != NULL ? c : m42_node_ident ("All"));
        }
      return m42_value_expr (span);
    }

  if ((name_is (name, "Simplify", "simplify") ||
       name_is (name, "FullSimplify", NULL)) && args->len == 1)
    {
      /* The identities that hold whatever the letters stand for,
       * written as the patterns they are and looked for the same way
       * a rule would look for them.  Nothing here needs to know that a
       * number is positive or a name is real, because none of these
       * do. */
      static const char *const IDENTITIES[] = {
        "Sin[u_]^2 + Cos[u_]^2 -> 1",
        "Cos[u_]^2 + Sin[u_]^2 -> 1",
        "Cosh[u_]^2 - Sinh[u_]^2 -> 1",
        "2 Sin[u_] Cos[u_] -> Sin[2 u]",
        "Exp[u_] Exp[v_] -> Exp[u + v]",
        "Sqrt[u_] Sqrt[u_] -> u",
      };
      g_autoptr (M42Node) tree = value_to_node (ARG (0));

      if (tree == NULL)
        return m42_value_ref (ARG (0));
      for (guint i = 0; i < G_N_ELEMENTS (IDENTITIES); i++)
        {
          g_autofree char *complaint = NULL;
          g_autoptr (M42Node) rule = m42_parse (IDENTITIES[i], &complaint);
          const M42Node *use = rule;
          M42Node *shorter;

          /* A line of source comes back wrapped when it is a statement. */
          while (use != NULL && use->kind == M42_NODE_SEQ && use->children->len == 1)
            use = m42_node_child (use, 0);
          if (use == NULL || use->kind != M42_NODE_RULE)
            continue;
          shorter = m42_node_replace_all (tree, m42_node_child (use, 0),
                                          m42_node_child (use, 1), pattern_test, s, NULL);
          m42_node_free (tree);
          tree = shorter;
        }

      /* A fraction with something dividing both halves loses it. */
      {
        M42Node *shorter = cancel_common_factor (tree);

        if (shorter != NULL)
          {
            m42_node_free (tree);
            tree = shorter;
          }
      }

      /* Multiplying out is sometimes the simpler answer and sometimes
       * not, so both are written down and the shorter one wins, which
       * is how Mathematica chooses too.  It is what turns x + x into
       * 2 x and leaves (x + 1)^10 alone. */
      {
        M42Node *wide = m42_node_expand (tree);
        g_autoptr (GString) as_is = g_string_new (NULL);
        g_autoptr (GString) opened = g_string_new (NULL);

        m42_node_to_string (as_is, tree);
        m42_node_to_string (opened, wide);
        if (opened->len < as_is->len)
          {
            m42_node_free (tree);
            tree = wide;
          }
        else
          m42_node_free (wide);
      }
      return expr_result (m42_node_copy (tree));
    }

  /* An unknown function of symbolic arguments stays symbolic, so that
   * f[x] can be written before f is defined. */
  {
    M42Node *n = m42_node_new (M42_NODE_CALL);
    n->name = g_strdup (name);
    for (guint i = 0; i < args->len; i++)
      {
        M42Node *c = value_to_node (ARG (i));
        if (c == NULL)
          {
            m42_node_free (n);
            return m42_value_error ("Unknown function %s", name);
          }
        g_ptr_array_add (n->children, c);
      }
    return m42_value_expr (n);
  }
}

/* --- indexing --------------------------------------------------------------- */

/* A symbol standing on its own, by name: All, end, or a Span. */
static gboolean
symbol_is (const M42Value *v, const char *name)
{
  return v->kind == M42_VALUE_EXPR && v->u.expr->kind == M42_NODE_IDENT &&
         strcmp (v->u.expr->name, name) == 0;
}

static const M42Node *
span_of (const M42Value *v)
{
  if (v->kind == M42_VALUE_EXPR && v->u.expr->kind == M42_NODE_CALL &&
      strcmp (v->u.expr->name, "Span") == 0 && v->u.expr->children->len == 2)
    return v->u.expr;
  return NULL;
}

/* One end of a span, against a list of n items: All at the start is 1,
 * All at the end is n, and a negative number counts back from n. */
static gboolean
span_end (const M42Node *e, guint n, gboolean upper, gint64 *out)
{
  double x;

  if (e->kind == M42_NODE_IDENT &&
      (strcmp (e->name, "All") == 0 || strcmp (e->name, "end") == 0))
    {
      *out = upper ? (gint64) n : 1;
      return TRUE;
    }
  if (e->kind == M42_NODE_NUMBER)
    x = e->number;
  else if (e->kind == M42_NODE_UNARY && e->op == M42_TOK_MINUS &&
           m42_node_child (e, 0)->kind == M42_NODE_NUMBER)
    x = -m42_node_child (e, 0)->number;
  else
    return FALSE;

  if (x != floor (x))
    return FALSE;
  *out = x < 0 ? (gint64) n + (gint64) x + 1 : (gint64) x;
  return TRUE;
}

/* Part, as both languages mean it: x[[2]], the last with x[[-1]], a
 * span with x[[2 ;; 4]], a chosen few with x[[{1, 3}]], a whole level
 * with All (which MATLAB writes as a bare colon), and MATLAB's end. */
static M42Value *
index_value (M42Value *v, GPtrArray *indices, guint from)
{
  M42Value *index;
  guint n;
  double k;
  M42Value *err;

  if (from == indices->len)
    return m42_value_ref (v);
  if (v->kind != M42_VALUE_LIST)
    return m42_value_error ("Part: this is not a list");
  n = m42_value_list_length (v);
  index = g_ptr_array_index (indices, from);

  /* Everything at this level, each item indexed by what is left. */
  if (symbol_is (index, "All"))
    {
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < n; i++)
        {
          M42Value *part = index_value (m42_value_list_nth (v, i), indices, from + 1);
          if (is_error (part))
            {
              m42_value_unref (out);
              return part;
            }
          m42_value_list_append (out, part);
        }
      return out;
    }

  /* The last item, which MATLAB calls end. */
  if (symbol_is (index, "end"))
    return n > 0 ? index_value (m42_value_list_nth (v, n - 1), indices, from + 1)
                 : m42_value_error ("Part: the list is empty");

  {
    const M42Node *span = span_of (index);

    if (span != NULL)
      {
        gint64 lo, hi;
        M42Value *out;

        if (!span_end (m42_node_child (span, 0), n, FALSE, &lo) ||
            !span_end (m42_node_child (span, 1), n, TRUE, &hi))
          return m42_value_error ("Part: that span cannot be worked out");
        if (lo < 1 || hi > (gint64) n)
          return m42_value_error ("Part: %" G_GINT64_FORMAT " to %" G_GINT64_FORMAT
                                  " is out of range for a list of %u", lo, hi, n);
        out = m42_value_list_new ();
        for (gint64 i = lo; i <= hi; i++)
          {
            M42Value *part = index_value (m42_value_list_nth (v, (guint) i - 1),
                                          indices, from + 1);
            if (is_error (part))
              {
                m42_value_unref (out);
                return part;
              }
            m42_value_list_append (out, part);
          }
        return out;
      }
  }

  /* A list of indices picks out those items, in that order. */
  if (index->kind == M42_VALUE_LIST)
    {
      M42Value *out = m42_value_list_new ();

      for (guint i = 0; i < m42_value_list_length (index); i++)
        {
          GPtrArray *one = g_ptr_array_new ();
          M42Value *part;

          g_ptr_array_add (one, m42_value_list_nth (index, i));
          for (guint j = from + 1; j < indices->len; j++)
            g_ptr_array_add (one, g_ptr_array_index (indices, j));
          part = index_value (v, one, 0);
          g_ptr_array_unref (one);
          if (is_error (part))
            {
              m42_value_unref (out);
              return part;
            }
          m42_value_list_append (out, part);
        }
      return out;
    }

  if (!need_number (index, "Part", &k, &err))
    return err;
  if (k < 0)
    k = n + k + 1;      /* -1 is the last */
  if (k < 1 || k > n || k != floor (k))
    return m42_value_error ("Part: index %g is out of range for a list of %u", k, n);
  return index_value (m42_value_list_nth (v, (guint) k - 1), indices, from + 1);
}

/* --- the evaluator ----------------------------------------------------------- */

/* --- changing one place, and defining a name ------------------------------
 *
 * v[[2]] = 9 leaves the rest of the list alone; MATLAB writes the same
 * thing v(2) = 9, and will make the list longer if the place asked for
 * is past the end.  f[0] = 1 and f[x_] := ... are not about a place at
 * all: they are kept as they were written, and tried in turn when f is
 * called.
 */

/* A copy of the list with one place changed, as deep as the indices go. */
static M42Value *
with_part_set (const M42Value *v, GPtrArray *index, guint at, M42Value *what,
               gboolean grow, M42Value **error)
{
  guint length = v != NULL && v->kind == M42_VALUE_LIST ? m42_value_list_length (v) : 0;
  double where;
  gint64 i;
  M42Value *out;

  if (at == index->len)
    return m42_value_ref (what);
  if (v != NULL && v->kind != M42_VALUE_LIST)
    {
      *error = m42_value_error ("that is not a list, so it has no places in it");
      return NULL;
    }
  if (!value_number (g_ptr_array_index (index, at), &where) || where != floor (where))
    {
      *error = m42_value_error ("a place in a list is a whole number");
      return NULL;
    }
  i = (gint64) where;
  if (i < 0)
    i += (gint64) length + 1;                 /* -1 is the last one */
  if (i < 1 || (!grow && (guint) i > length))
    {
      *error = m42_value_error ("there is no place %g in a list of %u", where, length);
      return NULL;
    }

  out = m42_value_list_new ();
  for (guint k = 0; k < MAX (length, (guint) i); k++)
    {
      M42Value *old = k < length ? m42_value_list_nth (v, k) : NULL;

      if (k + 1 == (guint) i)
        {
          M42Value *inner = with_part_set (old, index, at + 1, what, grow, error);

          if (inner == NULL)
            {
              m42_value_unref (out);
              return NULL;
            }
          m42_value_list_append (out, inner);
        }
      else
        /* A list made longer is filled out with zeros, as MATLAB does. */
        m42_value_list_append (out, old != NULL ? m42_value_ref (old) : m42_value_number (0));
    }
  return out;
}

/* v[[2]] = 9 and v(2) = 9.  The indices start at first among the
 * children of the left side. */
static M42Value *
assign_part (M42Session *s, const char *name, const M42Node *lhs, guint first,
             const M42Node *right, gboolean matlab)
{
  g_autoptr (M42Value) what = eval (s, right);
  g_autoptr (GPtrArray) index = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
  M42Value *old = lookup (s, name);
  M42Value *error = NULL;
  M42Value *fresh;

  if (is_error (what))
    return g_steal_pointer (&what);
  if (old == NULL && !matlab)
    return m42_value_error ("%s has nothing in it to change", name);
  for (guint i = first; i < lhs->children->len; i++)
    {
      M42Value *where = eval (s, m42_node_child (lhs, i));

      if (is_error (where))
        {
          M42Value *bad = m42_value_ref (where);

          m42_value_unref (where);
          return bad;
        }
      g_ptr_array_add (index, where);
    }
  if (index->len == 0)
    return m42_value_error ("%s: which place?", name);

  fresh = with_part_set (old, index, 0, what, matlab, &error);
  if (fresh == NULL)
    return error != NULL ? error : m42_value_error ("%s: that place cannot be set", name);
  g_hash_table_insert (s->globals, g_strdup (name), fresh);
  /* MATLAB shows the whole thing again; Mathematica shows what was put
   * in the place. */
  return matlab ? m42_value_ref (fresh) : m42_value_ref (what);
}

/* Keeps a definition under the name it is for, in the order it was
 * given, with one of the same shape replaced rather than added. */
static M42Value *
define_rule (M42Session *s, const M42Node *n)
{
  const M42Node *lhs = m42_node_child (n, 0);
  const M42Node *shape = lhs;
  GPtrArray *list;
  M42Node *rule;

  /* {a, b} = {1, 2}: each name takes its piece.  A MATLAB [a, b] is
   * read as a matrix of one row, so that shape counts too. */
  if (lhs->kind == M42_NODE_LIST ||
      (lhs->kind == M42_NODE_MATRIX && lhs->children->len == 1))
    {
      const M42Node *names = lhs->kind == M42_NODE_LIST ? lhs : m42_node_child (lhs, 0);
      g_autoptr (M42Value) pieces = eval (s, m42_node_child (n, 1));

      if (is_error (pieces))
        return g_steal_pointer (&pieces);
      if (pieces->kind != M42_VALUE_LIST)
        return m42_value_error ("that is one value, not %u to hand out", names->children->len);
      if (m42_value_list_length (pieces) != names->children->len)
        return m42_value_error ("there are %u names and %u values",
                                names->children->len, m42_value_list_length (pieces));
      for (guint i = 0; i < names->children->len; i++)
        {
          const M42Node *who = m42_node_child (names, i);

          if (who->kind != M42_NODE_IDENT)
            return m42_value_error ("only names can be handed a value");
          g_hash_table_insert (s->globals, g_strdup (who->name),
                               m42_value_ref (m42_value_list_nth (pieces, i)));
        }
      return m42_value_ref (pieces);
    }

  if (lhs->kind == M42_NODE_PART && lhs->children->len > 1 &&
      m42_node_child (lhs, 0)->kind == M42_NODE_IDENT)
    return assign_part (s, m42_node_child (lhs, 0)->name, lhs, 1,
                        m42_node_child (n, 1), FALSE);

  if (shape->kind == M42_NODE_CONDITION)
    shape = m42_node_child (shape, 0);
  if (shape->kind != M42_NODE_CALL)
    return m42_value_error ("math42 cannot define that");

  /* v(2) = 9: a name that already holds a list is being changed in one
   * place, not given a meaning as a function. */
  if (shape == lhs && !m42_node_has_pattern (shape))
    {
      M42Value *held = lookup (s, shape->name);

      if (held != NULL && held->kind == M42_VALUE_LIST)
        return assign_part (s, shape->name, shape, 0, m42_node_child (n, 1), TRUE);
    }

  rule = m42_node_new (M42_NODE_RULE);
  g_ptr_array_add (rule->children, m42_node_copy (lhs));
  g_ptr_array_add (rule->children, m42_node_copy (m42_node_child (n, 1)));

  list = g_hash_table_lookup (s->defined, shape->name);
  if (list == NULL)
    {
      list = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_node_free);
      g_hash_table_insert (s->defined, g_strdup (shape->name), list);
    }
  for (guint i = 0; i < list->len; i++)
    if (m42_node_same (m42_node_child (g_ptr_array_index (list, i), 0), lhs))
      {
        g_ptr_array_remove_index (list, i);
        break;
      }
  g_ptr_array_add (list, rule);
  return m42_value_null ();
}

/* The definitions for a name, tried in turn: the ones with no hole in
 * them first, so that f[0] = 1 is found before f[n_] := ... whichever
 * order they were given in. */
static M42Value *
try_definitions (M42Session *s, const char *name, GPtrArray *args, gboolean *matched)
{
  GPtrArray *list = g_hash_table_lookup (s->defined, name);
  g_autoptr (M42Node) probe = NULL;

  *matched = FALSE;
  if (list == NULL || list->len == 0)
    return NULL;

  probe = m42_node_new (M42_NODE_CALL);
  probe->name = g_strdup (name);
  for (guint i = 0; i < args->len; i++)
    {
      M42Node *a = value_to_node (g_ptr_array_index (args, i));

      if (a == NULL)
        return NULL;
      g_ptr_array_add (probe->children, a);
    }

  for (int pass = 0; pass < 2; pass++)
    for (guint i = 0; i < list->len; i++)
      {
        const M42Node *rule = g_ptr_array_index (list, i);
        const M42Node *shape = m42_node_child (rule, 0);
        g_autoptr (GHashTable) names = m42_pattern_names_new ();

        if (m42_node_has_pattern (shape) != (pass == 1))
          continue;
        if (!m42_node_match (shape, probe, names, pattern_test, s))
          continue;
        {
          g_autoptr (M42Node) body = m42_node_bind (m42_node_child (rule, 1), names);
          M42Value *answer;

          if (s->depth > 200)
            return m42_value_error ("Recursion too deep");
          *matched = TRUE;
          s->depth++;
          answer = eval (s, body);
          s->depth--;
          return answer;
        }
      }
  return NULL;
}

/* --- fitting and interpolating, the Mathematica way ----------------------
 *
 * Fit[data, {1, x, x^2}, x] is the combination of those functions
 * closest to the points, which is least squares against a matrix whose
 * columns are the functions worked out at each x.  Interpolation[data]
 * hands back something you can call, which is done by making a
 * function whose body asks interp1 about the points it was given.
 */

/* The points of a data list: {{x, y}, ...} as it is written, or a list
 * of heights with the places running 1, 2, 3.  FALSE when it is
 * neither. */
static gboolean
data_points (const M42Value *data, GArray *xs, GArray *ys)
{
  guint n;

  if (data == NULL || data->kind != M42_VALUE_LIST)
    return FALSE;
  n = m42_value_list_length (data);
  if (n == 0)
    return FALSE;
  for (guint i = 0; i < n; i++)
    {
      M42Value *e = m42_value_list_nth (data, i);
      double x, y;

      if (e->kind == M42_VALUE_LIST && m42_value_list_length (e) == 2)
        {
          if (!value_number (m42_value_list_nth (e, 0), &x) ||
              !value_number (m42_value_list_nth (e, 1), &y))
            return FALSE;
        }
      else if (value_number (e, &y))
        x = i + 1;
      else
        return FALSE;
      g_array_append_val (xs, x);
      g_array_append_val (ys, y);
    }
  return TRUE;
}

static M42Value *
fit_to_basis (M42Session *s, const M42Node *call)
{
  g_autoptr (M42Value) data = eval (s, m42_node_child (call, 0));
  const M42Node *basis = m42_node_child (call, 1);
  const M42Node *var_node = m42_node_child (call, 2);
  g_autoptr (GArray) xs = g_array_new (FALSE, FALSE, sizeof (double));
  g_autoptr (GArray) ys = g_array_new (FALSE, FALSE, sizeof (double));
  g_autoptr (M42Value) a = NULL;
  g_autoptr (M42Value) b = NULL;
  g_autoptr (M42Value) found = NULL;
  const char *var;
  guint terms;

  if (is_error (data))
    return g_steal_pointer (&data);
  if (basis->kind != M42_NODE_LIST || basis->children->len == 0 ||
      var_node->kind != M42_NODE_IDENT)
    return m42_value_error ("Fit expects data, a list of functions, and a name");
  var = var_node->name;
  terms = basis->children->len;
  if (!data_points (data, xs, ys))
    return m42_value_error ("Fit expects {{x, y}, ...} or a list of heights");
  if (xs->len < terms)
    return m42_value_error ("Fit: %u points cannot settle %u terms", xs->len, terms);

  /* One row for each point, one column for each function in the list. */
  a = m42_value_list_new ();
  b = m42_value_list_new ();
  for (guint i = 0; i < xs->len; i++)
    {
      M42Value *row = m42_value_list_new ();

      for (guint j = 0; j < terms; j++)
        {
          double at = number_at (s, m42_node_child (basis, j), var,
                                 g_array_index (xs, double, i));

          if (!isfinite (at))
            return m42_value_error ("Fit: one of those functions has no value at %g",
                                    g_array_index (xs, double, i));
          m42_value_list_append (row, m42_value_real (at));
        }
      m42_value_list_append (a, row);
      m42_value_list_append (b, m42_value_real (g_array_index (ys, double, i)));
    }

  found = m42_value_least_squares (a, b);
  if (is_error (found) || found->kind != M42_VALUE_LIST ||
      m42_value_list_length (found) != terms)
    return is_error (found) ? g_steal_pointer (&found)
                            : m42_value_error ("Fit: those points do not settle it");

  /* The answer written out: the coefficients against their functions. */
  {
    M42Node *out = NULL;

    for (guint j = 0; j < terms; j++)
      {
        double c;
        M42Node *term;

        if (!value_number (m42_value_list_nth (found, j), &c))
          return m42_value_error ("Fit: that came out badly");
        if (fabs (c) < 1e-12)
          continue;
        term = m42_node_binary (M42_TOK_STAR, coefficient_node (c),
                                m42_node_copy (m42_node_child (basis, j)));
        out = out == NULL ? term : m42_node_binary (M42_TOK_PLUS, out, term);
      }
    return expr_result (out != NULL ? out : m42_node_number (0));
  }
}

/* Interpolation[data] as something to call: a function of one argument
 * whose body hands the points and that argument to interp1. */
static M42Value *
interpolation_function (M42Session *s, const M42Value *data)
{
  g_autoptr (GArray) xs = g_array_new (FALSE, FALSE, sizeof (double));
  g_autoptr (GArray) ys = g_array_new (FALSE, FALSE, sizeof (double));
  M42Node *places, *heights, *body;
  GStrv params;

  (void) s;
  if (!data_points (data, xs, ys) || xs->len < 2)
    return m42_value_error ("Interpolation wants {{x, y}, ...} or a list of heights");

  places = m42_node_new (M42_NODE_LIST);
  heights = m42_node_new (M42_NODE_LIST);
  for (guint i = 0; i < xs->len; i++)
    {
      g_ptr_array_add (places->children, coefficient_node (g_array_index (xs, double, i)));
      g_ptr_array_add (heights->children, coefficient_node (g_array_index (ys, double, i)));
    }

  body = m42_node_new (M42_NODE_CALL);
  body->name = g_strdup ("interp1");
  g_ptr_array_add (body->children, places);
  g_ptr_array_add (body->children, heights);
  g_ptr_array_add (body->children, m42_node_ident ("$1"));

  params = g_new0 (char *, 2);
  params[0] = g_strdup ("$1");
  return m42_value_func (params, body);
}

/* --- data on disk ---------------------------------------------------------
 *
 * A table of numbers is the one thing every other program of this kind
 * can read and write, and it is what a notebook most often needs from
 * outside itself.  Import reads one and Export writes one; the ending
 * of the name says whether it is a table or plain text.
 */

/* One line of a comma-separated file as a list: a number where the
 * field is one, and the text otherwise. */
static M42Value *
csv_row (const char *line, char separator)
{
  M42Value *row = m42_value_list_new ();
  g_auto (GStrv) fields = g_strsplit (line, separator == 0 ? "," : (char[]) { separator, 0 }, -1);

  for (guint i = 0; fields[i] != NULL; i++)
    {
      char *field = g_strstrip (fields[i]);
      char *end = NULL;
      double x;

      if (*field == 0)
        continue;
      x = g_ascii_strtod (field, &end);
      if (end != NULL && *end == 0)
        m42_value_list_append (row, m42_value_real (x));
      else
        m42_value_list_append (row, m42_value_string (field));
    }
  return row;
}

static M42Value *
import_file (const char *path, const char *what)
{
  g_autofree char *contents = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *lower = g_ascii_strdown (path, -1);
  gboolean as_table;

  if (!g_file_get_contents (path, &contents, NULL, &error))
    return m42_value_error ("%s: %s", what, error->message);

  as_table = g_str_has_suffix (lower, ".csv") || g_str_has_suffix (lower, ".tsv") ||
             g_str_has_suffix (lower, ".dat") || strcmp (what, "csvread") == 0 ||
             strcmp (what, "readmatrix") == 0;
  if (!as_table)
    return m42_value_string (contents);

  {
    char separator = g_str_has_suffix (lower, ".tsv") ? '\t' : ',';
    g_auto (GStrv) lines = g_strsplit (contents, "\n", -1);
    M42Value *out = m42_value_list_new ();

    for (guint i = 0; lines[i] != NULL; i++)
      {
        M42Value *row;

        g_strchomp (lines[i]);
        if (lines[i][0] == 0)
          continue;
        row = csv_row (lines[i], separator);
        if (m42_value_list_length (row) == 0)
          m42_value_unref (row);
        else
          m42_value_list_append (out, row);
      }
    /* A file of one column comes back as a plain list, which is what
     * anyone reading a column of numbers wants. */
    {
      gboolean thin = m42_value_list_length (out) > 0;

      for (guint i = 0; i < m42_value_list_length (out) && thin; i++)
        thin = m42_value_list_length (m42_value_list_nth (out, i)) == 1;
      if (thin)
        {
          M42Value *flat = m42_value_list_new ();

          for (guint i = 0; i < m42_value_list_length (out); i++)
            m42_value_list_append (flat,
              m42_value_ref (m42_value_list_nth (m42_value_list_nth (out, i), 0)));
          m42_value_unref (out);
          return flat;
        }
    }
    return out;
  }
}

/* A value written to disk: a table as rows of fields, anything else as
 * the text math42 would print. */
static M42Value *
export_file (const char *path, const M42Value *v, const char *what)
{
  g_autofree char *lower = g_ascii_strdown (path, -1);
  g_autoptr (GString) text = g_string_new (NULL);
  g_autoptr (GError) error = NULL;
  gboolean as_table = g_str_has_suffix (lower, ".csv") || g_str_has_suffix (lower, ".tsv") ||
                      g_str_has_suffix (lower, ".dat") || strcmp (what, "csvwrite") == 0 ||
                      strcmp (what, "writematrix") == 0;
  char separator = g_str_has_suffix (lower, ".tsv") ? '\t' : ',';

  if (as_table && v->kind == M42_VALUE_LIST)
    {
      for (guint i = 0; i < m42_value_list_length (v); i++)
        {
          M42Value *row = m42_value_list_nth (v, i);

          if (row->kind == M42_VALUE_LIST)
            for (guint j = 0; j < m42_value_list_length (row); j++)
              {
                g_autofree char *field = m42_value_to_string (m42_value_list_nth (row, j));

                if (j > 0)
                  g_string_append_c (text, separator);
                g_string_append (text, field);
              }
          else
            {
              g_autofree char *field = m42_value_to_string (row);

              g_string_append (text, field);
            }
          g_string_append_c (text, '\n');
        }
    }
  else if (v->kind == M42_VALUE_STRING)
    g_string_append (text, v->u.string);
  else
    {
      g_autofree char *written = m42_value_to_string (v);

      g_string_append (text, written);
      g_string_append_c (text, '\n');
    }

  if (!g_file_set_contents (path, text->str, -1, &error))
    return m42_value_error ("%s: %s", what, error->message);
  return m42_value_string (path);
}

static M42Value *
eval_call (M42Session *s, const M42Node *n)
{
  const char *name = n->name;
  M42Value *fv;
  GPtrArray *args;
  M42Value *r;

  /* Forms that need their arguments unevaluated. */
  if (name_is (name, "D", "Derivative"))    return differentiate (s, n);
  /* quad(f, a, b) is MATLAB's spelling: the bounds side by side rather
   * than in a list, which is what integral does too.  It has to be
   * told apart from the iterated integral below, whose ranges are all
   * lists. */
  if (name_is (name, "NIntegrate", "quad") && n->children->len == 3 &&
      m42_node_child (n, 1)->kind != M42_NODE_LIST)
    return integral_matlab (s, n);

  if ((name_is (name, "Integrate", "int") || name_is (name, "NIntegrate", "quad")) &&
      n->children->len > 2)
    {
      /* Integrate[f, {x, a, b}, {y, c, d}]: the inner integral first,
       * then the outer one over what it gives, as it is done by hand. */
      M42Node *inner = m42_node_new (M42_NODE_CALL);
      M42Value *iterated;

      inner->name = g_strdup (name);
      g_ptr_array_add (inner->children, m42_node_copy (m42_node_child (n, 0)));
      g_ptr_array_add (inner->children,
                       m42_node_copy (m42_node_child (n, n->children->len - 1)));
      for (guint i = 1; i + 1 < n->children->len; i++)
        {
          M42Node *outer = m42_node_new (M42_NODE_CALL);

          outer->name = g_strdup (name);
          g_ptr_array_add (outer->children, inner);
          g_ptr_array_add (outer->children, m42_node_copy (m42_node_child (n, i)));
          inner = outer;
        }
      iterated = eval (s, inner);
      m42_node_free (inner);
      return iterated;
    }
  if (name_is (name, "Integrate", "int"))   return integrate (s, n, FALSE);
  if (name_is (name, "NIntegrate", "quad")) return integrate (s, n, TRUE);
  if (name_is (name, "integral", NULL))     return integral_matlab (s, n);
  if (name_is (name, "Sum", NULL))          return sum_or_product (s, n, FALSE);
  if (name_is (name, "Product", NULL))      return sum_or_product (s, n, TRUE);
  if (name_is (name, "Table", NULL) && n->children->len >= 2) return table (s, n, 1);
  if (name_is (name, "Plot", "fplot"))      return plot (s, n, FALSE, FALSE);
  if (name_is (name, "LogPlot", "semilogy")) return plot (s, n, FALSE, TRUE);
  if (name_is (name, "LogLinearPlot", "semilogx")) return plot (s, n, TRUE, FALSE);
  if (name_is (name, "LogLogPlot", "loglog")) return plot (s, n, TRUE, TRUE);
  if (name_is (name, "ContourPlot", "contour")) return contour_plot (s, n);
  if (name_is (name, "ParametricPlot", NULL)) return parametric_plot (s, n, FALSE);
  if (name_is (name, "ParametricPlot3D", NULL))
    return parametric_plot3d (s, n);

  /* quiver is MATLAB's name for the same picture.  StreamPlot is not:
   * it draws the lines a flow follows, not the arrows at each place,
   * and math42 does not have it. */
  if (name_is (name, "VectorPlot", "quiver"))
    return vector_plot (s, n);

  if (name_is (name, "ListPlot3D", NULL) || name_is (name, "ListContourPlot", NULL) ||
      name_is (name, "ListDensityPlot", NULL))
    {
      g_autoptr (M42Value) data = n->children->len >= 1 ? eval (s, m42_node_child (n, 0))
                                                        : m42_value_null ();

      if (is_error (data))
        return m42_value_ref (data);
      return list_grid_plot (data, name, name_is (name, "ListContourPlot", NULL),
                             name_is (name, "ListDensityPlot", NULL));
    }

  if (name_is (name, "Plot3D", "surf") || name_is (name, "mesh", NULL) ||
      name_is (name, "DensityPlot", NULL))
    return plot3d (s, n, name_is (name, "DensityPlot", NULL));
  if (name_is (name, "PolarPlot", "polarplot")) return parametric_plot (s, n, TRUE);
  if (name_is (name, "FindMinimum", "fminsearch") || name_is (name, "FindMaximum", NULL) ||
      name_is (name, "fminbnd", NULL) || name_is (name, "ArgMin", "argmin"))
    return find_extremum (s, n, name);
  if (name_is (name, "Piecewise", NULL) && n->children->len >= 1 &&
      m42_node_child (n, 0)->kind == M42_NODE_LIST)
    {
      /* Piecewise[{{value, when}, {value, when}}, otherwise]. */
      const M42Node *pieces = m42_node_child (n, 0);

      for (guint i = 0; i < pieces->children->len; i++)
        {
          const M42Node *piece = m42_node_child (pieces, i);
          g_autoptr (M42Value) holds = NULL;

          if (piece->kind != M42_NODE_LIST || piece->children->len != 2)
            return m42_value_error ("Piecewise wants pairs of a value and when it holds");
          holds = eval (s, m42_node_child (piece, 1));
          if (is_error (holds))
            return g_steal_pointer (&holds);
          /* A condition that is not yet a yes or a no leaves the whole
           * thing as it was written, so that a rule can fill the names
           * in later. */
          if (!is_num (holds))
            return m42_value_expr (m42_node_copy (n));
          if (holds->u.number != 0)
            return eval (s, m42_node_child (piece, 0));
        }
      return n->children->len >= 2 ? eval (s, m42_node_child (n, 1)) : m42_value_number (0);
    }
  /* Clear[f] forgets a name: what it held, and anything defined for
   * it.  Bare clear, the MATLAB way, forgets the lot. */
  if (name_is (name, "Clear", "clear") || name_is (name, "ClearAll", NULL))
    {
      if (n->children->len == 0)
        {
          g_hash_table_remove_all (s->globals);
          g_hash_table_remove_all (s->defined);
          return m42_value_null ();
        }
      for (guint i = 0; i < n->children->len; i++)
        {
          const M42Node *which = m42_node_child (n, i);
          const char *gone = which->kind == M42_NODE_IDENT ? which->name :
                             which->kind == M42_NODE_STRING ? which->name : NULL;

          if (gone == NULL)
            return m42_value_error ("Clear wants names");
          g_hash_table_remove (s->globals, gone);
          g_hash_table_remove (s->defined, gone);
        }
      return m42_value_null ();
    }

  /* Fit is given a list of functions of a name, which have to stay as
   * they are written until each is worked out at a point. */
  if (name_is (name, "Fit", NULL) && n->children->len == 3 &&
      m42_node_child (n, 1)->kind == M42_NODE_LIST)
    return fit_to_basis (s, n);

  if (name_is (name, "FindRoot", NULL))     return find_root (s, n, FALSE);
  if (name_is (name, "fzero", NULL))        return find_root (s, n, TRUE);
  if (name_is (name, "Solve", "NSolve"))
    {
      /* A list of equations and a list of unknowns is a system. */
      if (n->children->len == 2 && m42_node_child (n, 0)->kind == M42_NODE_LIST &&
          m42_node_child (n, 1)->kind == M42_NODE_LIST)
        return solve_system (s, m42_node_child (n, 0), m42_node_child (n, 1));
      return solve (s, n);
    }
  if (name_is (name, "Coefficient", NULL) || name_is (name, "CoefficientList", NULL) ||
      name_is (name, "Exponent", NULL) || name_is (name, "Collect", "collect") ||
      name_is (name, "Together", "simplifyfraction") || name_is (name, "Cancel", NULL) ||
      name_is (name, "Apart", "partfrac") || name_is (name, "PolynomialQuotient", "deconv") ||
      name_is (name, "PolynomialRemainder", NULL) || name_is (name, "PolynomialGCD", NULL))
    return polynomial_algebra (s, n, name);
  if (name_is (name, "Limit", NULL))        return limit (s, n);
  if (name_is (name, "Timing", "tic") && n->children->len == 1)
    {
      /* {seconds, answer}, as Mathematica hands it back. */
      gint64 before = g_get_monotonic_time ();
      M42Value *answer = eval (s, m42_node_child (n, 0));
      M42Value *timed;

      if (is_error (answer))
        return answer;
      timed = m42_value_list_new ();
      m42_value_list_append (timed, m42_value_real ((g_get_monotonic_time () - before) / 1e6));
      m42_value_list_append (timed, answer);
      return timed;
    }
  if (name_is (name, "NDSolve", "ode45"))   return ndsolve (s, n);
  if (name_is (name, "DSolve", "dsolve"))   return dsolve (s, n);
  if (name_is (name, "RSolve", "rsolve"))   return rsolve (s, n);
  if ((name_is (name, "Grad", "gradient") || name_is (name, "Div", "divergence") ||
       name_is (name, "Curl", "curl") || name_is (name, "Laplacian", "laplacian") ||
       name_is (name, "Hessian", "hessian") || name_is (name, "Jacobian", "jacobian")) &&
      n->children->len == 2)
    return vector_calculus (s, n, name);
  if (name_is (name, "FourierSeries", NULL))      return fourier_series (s, n, FALSE);
  if (name_is (name, "FourierCoefficient", NULL)) return fourier_series (s, n, TRUE);

  /* The Laplace pair: the function is looked at as it was written, so
   * that t and s stay symbols throughout. */
  if ((name_is (name, "LaplaceTransform", "laplace") ||
       name_is (name, "InverseLaplaceTransform", "ilaplace")) && n->children->len == 3)
    {
      gboolean forward = name[0] == 'L' || name[0] == 'l';
      const M42Node *from = m42_node_child (n, 1), *to = m42_node_child (n, 2);
      g_autoptr (M42Node) f = NULL;
      M42Node *transformed;

      if (from->kind != M42_NODE_IDENT || to->kind != M42_NODE_IDENT)
        return m42_value_error ("%s expects two names, as in %s[f, t, s]", name, name);
      f = symbolic_argument (s, m42_node_child (n, 0), from->name);
      transformed = forward ? m42_node_laplace (f, from->name, to->name)
                            : m42_node_inverse_laplace (f, from->name, to->name);
      if (transformed == NULL)
        return m42_value_error ("%s: that one is not in the table", name);
      return expr_result (transformed);
    }
  /* ZTransform[f, n, z]: the sequence is looked at as it was written,
   * so that n and z both stay names. */
  if (name_is (name, "FourierTransform", NULL) ||
      name_is (name, "InverseFourierTransform", NULL))
    return fourier_transform (s, n, name_is (name, "InverseFourierTransform", NULL));

  if (name_is (name, "InverseZTransform", NULL) && n->children->len == 3)
    {
      const M42Node *from = m42_node_child (n, 1), *to = m42_node_child (n, 2);
      g_autoptr (M42Node) f = NULL;
      M42Node *back;

      if (from->kind != M42_NODE_IDENT || to->kind != M42_NODE_IDENT)
        return m42_value_error ("InverseZTransform expects two names");
      f = symbolic_argument (s, m42_node_child (n, 0), from->name);
      back = m42_node_inverse_ztransform (f, from->name, to->name);
      if (back == NULL)
        return m42_value_error ("InverseZTransform: that one will not come apart");
      return expr_result (back);
    }

  if (name_is (name, "ZTransform", NULL) && n->children->len == 3)
    {
      const M42Node *from = m42_node_child (n, 1), *to = m42_node_child (n, 2);
      g_autoptr (M42Node) f = NULL;
      M42Node *transformed;

      if (from->kind != M42_NODE_IDENT || to->kind != M42_NODE_IDENT)
        return m42_value_error ("ZTransform expects two names, as in ZTransform[f, n, z]");
      f = symbolic_argument (s, m42_node_child (n, 0), from->name);
      transformed = m42_node_ztransform (f, from->name, to->name);
      if (transformed == NULL)
        return m42_value_error ("ZTransform: that one is not in the table");
      return expr_result (transformed);
    }

  if (name_is (name, "Factor", "factor") && n->children->len == 1)
    return factor (s, n);
  if (name_is (name, "Series", "Taylor"))   return series (s, n);
  if (name_is (name, "Expand", "expand") && n->children->len == 1)
    {
      g_autoptr (M42Value) v = eval (s, m42_node_child (n, 0));
      M42Node *tree;

      if (is_error (v))
        return g_steal_pointer (&v);
      tree = value_to_node (v);
      if (tree == NULL)
        return g_steal_pointer (&v);
      {
        M42Node *expanded = m42_node_expand (tree);
        m42_node_free (tree);
        return expr_result (expanded);
      }
    }
  {
    gboolean handled;
    M42Value *flow = control_flow (s, n, &handled);
    if (handled)
      return flow;
  }

  args = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
  for (guint i = 0; i < n->children->len; i++)
    {
      M42Value *v = eval (s, m42_node_child (n, i));
      if (is_error (v))
        {
          g_ptr_array_unref (args);
          return v;
        }
      g_ptr_array_add (args, v);
    }

  {
    gboolean matched = FALSE;
    M42Value *answer = try_definitions (s, name, args, &matched);

    if (matched)
      {
        g_ptr_array_unref (args);
        return answer;
      }
    g_clear_pointer (&answer, m42_value_unref);
  }

  fv = lookup (s, name);

  /* s(s + 1) with s a symbol and no function of that name anywhere: it
   * is a multiplication, which is what it means in Mathematica.  With
   * round brackets only -- f[x] is always a call. */
  if ((fv == NULL || fv->kind == M42_VALUE_EXPR) && n->op == 1 && args->len == 1 &&
      m42_function_find (name) == NULL)
    {
      g_autoptr (M42Value) symbol = fv != NULL ? m42_value_ref (fv)
                                              : m42_value_expr (m42_node_ident (name));
      M42Value *product = map2 (M42_TOK_STAR, symbol, ARG (0));

      g_ptr_array_unref (args);
      return product;
    }

  if (fv != NULL && fv->kind == M42_VALUE_FUNC)
    r = apply_function (s, fv, args);
  else if (fv != NULL && fv->kind == M42_VALUE_LIST)
    r = index_value (fv, args, 0);              /* MATLAB: x(2) */
  else if (name_is (name, "ListPlot", "scatter"))
    r = list_plot_with_options (args, M42_SERIES_POINTS, FALSE);
  else if (name_is (name, "ListLinePlot", NULL))
    r = list_plot_with_options (args, M42_SERIES_LINE, FALSE);
  else if (name_is (name, "BarChart", "bar"))
    r = list_plot_with_options (args, M42_SERIES_BARS, FALSE);
  else if (name_is (name, "StemPlot", "stem"))
    r = list_plot_with_options (args, M42_SERIES_STEM, FALSE);
  else if (name_is (name, "StairsPlot", "stairs"))
    r = list_plot_with_options (args, M42_SERIES_STAIRS, FALSE);
  else if (name_is (name, "Show", NULL))
    r = show_plots (args);
  else if (name_is (name, "Histogram", "hist"))
    r = list_plot_with_options (args, M42_SERIES_BARS, TRUE);
  else if (name_is (name, "plot", NULL))
    r = list_plot_with_options (args, M42_SERIES_LINE, TRUE);
  else
    r = call_builtin (s, name, args);
  g_ptr_array_unref (args);
  return r;
}

static M42Value *
eval (M42Session *s, const M42Node *n)
{
  switch (n->kind)
    {
    case M42_NODE_NUMBER:
      /* Written with more digits than a double holds: read exactly. */
      if (n->name != NULL)
        {
          M42Big *big = m42_big_from_string (n->name);

          if (big != NULL)
            return m42_value_bigint (big);
        }
      return m42_value_number (n->number);

    case M42_NODE_STRING:
      return m42_value_string (n->name);

    case M42_NODE_IDENT:
      {
        M42Value *v = lookup (s, n->name);
        if (v != NULL)
          return m42_value_ref (v);
        if (strcmp (n->name, "I") == 0 || strcmp (n->name, "ImaginaryI") == 0)
          return m42_value_complex (0, 1);
        for (guint i = 0; i < G_N_ELEMENTS (CONSTANTS); i++)
          if (strcmp (CONSTANTS[i].name, n->name) == 0)
            return CONSTANTS[i].symbolic ? m42_value_expr (m42_node_ident (n->name))
                                         : m42_value_number (CONSTANTS[i].value);
        return m42_value_expr (m42_node_ident (n->name));
      }

    case M42_NODE_LAST:
      return s->last != NULL ? m42_value_ref (s->last) : m42_value_error ("There is no previous output");

    case M42_NODE_UNARY:
      {
        g_autoptr (M42Value) a = eval (s, m42_node_child (n, 0));
        if (is_error (a))
          return g_steal_pointer (&a);
        switch (n->op)
          {
          case M42_TOK_MINUS: return negate (a);
          case M42_TOK_BANG:
            /* A factorial past twenty has outgrown a gint64 and is
             * worked out exactly, digit by digit. */
            if (is_whole (a) && a->kind == M42_VALUE_NUMBER && a->num >= 0 && a->num <= 20000)
              return m42_value_bigint (m42_big_factorial ((guint) a->num));
            return map1 (a, "Factorial", factorial);
          case M42_TOK_NOT:   return map1 (a, "Not", m42_not);
          case M42_TOK_QUOTE: return m42_value_transpose (a);
          default:            return m42_value_error ("Internal error: unknown unary operator");
          }
      }

    case M42_NODE_BINARY:
      {
        g_autoptr (M42Value) a = eval (s, m42_node_child (n, 0));
        g_autoptr (M42Value) b = NULL;
        if (is_error (a))
          return g_steal_pointer (&a);
        if ((n->op == M42_TOK_AND || n->op == M42_TOK_OR) && is_num (a))
          {
            if (n->op == M42_TOK_AND && a->u.number == 0)
              return m42_value_number (0);
            if (n->op == M42_TOK_OR && a->u.number != 0)
              return m42_value_number (1);
          }
        b = eval (s, m42_node_child (n, 1));
        if (is_error (b))
          return g_steal_pointer (&b);
        if (n->op == M42_TOK_DOT)
          return m42_value_dot (a, b);
        if (n->op == M42_TOK_BACKSLASH)
          return m42_value_linear_solve (a, b);   /* MATLAB's A \ b */
        return map2 (n->op, a, b);
      }

    case M42_NODE_LIST:
    case M42_NODE_MATRIX:
      {
        M42Value *out = m42_value_list_new ();
        for (guint i = 0; i < n->children->len; i++)
          {
            M42Value *v = eval (s, m42_node_child (n, i));
            if (is_error (v))
              {
                m42_value_unref (out);
                return v;
              }
            m42_value_list_append (out, v);
          }
        /* [1 2 3] is a vector, [1 2; 3 4] a matrix, [] empty. */
        if (n->kind == M42_NODE_MATRIX && m42_value_list_length (out) == 1)
          {
            M42Value *row = m42_value_ref (m42_value_list_nth (out, 0));
            m42_value_unref (out);
            return row;
          }
        return out;
      }

    case M42_NODE_RANGE:
      {
        GPtrArray *args = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
        M42Value *r;
        /* a:b, or a:step:b with the step in the middle. */
        g_ptr_array_add (args, eval (s, m42_node_child (n, 0)));
        g_ptr_array_add (args, eval (s, m42_node_child (n, n->children->len == 3 ? 2 : 1)));
        if (n->children->len == 3)
          g_ptr_array_add (args, eval (s, m42_node_child (n, 1)));
        for (guint i = 0; i < args->len; i++)
          if (is_error (ARG (i)))
            {
              r = m42_value_ref (ARG (i));
              g_ptr_array_unref (args);
              return r;
            }
        r = range_builtin ("Range", args);
        g_ptr_array_unref (args);
        return r;
      }

    case M42_NODE_PART:
      {
        g_autoptr (M42Value) target = eval (s, m42_node_child (n, 0));
        GPtrArray *idx = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
        M42Value *r;
        if (is_error (target))
          {
            g_ptr_array_unref (idx);
            return g_steal_pointer (&target);
          }
        for (guint i = 1; i < n->children->len; i++)
          g_ptr_array_add (idx, eval (s, m42_node_child (n, i)));
        r = index_value (target, idx, 0);
        g_ptr_array_unref (idx);
        return r;
      }

    case M42_NODE_CALL:
      return eval_call (s, n);

    case M42_NODE_APPLYFN:
      {
        g_autoptr (M42Value) f = eval (s, m42_node_child (n, 0));
        GPtrArray *args = g_ptr_array_new_with_free_func ((GDestroyNotify) m42_value_unref);
        M42Value *r;

        if (is_error (f))
          {
            g_ptr_array_unref (args);
            return g_steal_pointer (&f);
          }
        for (guint i = 1; i < n->children->len; i++)
          {
            M42Value *v = eval (s, m42_node_child (n, i));
            if (is_error (v))
              {
                g_ptr_array_unref (args);
                return v;
              }
            g_ptr_array_add (args, v);
          }
        r = apply_callable (s, f, args);
        g_ptr_array_unref (args);
        return r;
      }

    case M42_NODE_ASSIGN:
      {
        g_autoptr (M42Value) v = eval (s, m42_node_child (n, 0));

        if (is_error (v))
          return g_steal_pointer (&v);

        /* x += 2 and x++ work on what the name already holds. */
        if (n->op != 0)
          {
            M42Value *had = lookup (s, n->name);
            M42Value *now;

            if (had == NULL)
              return m42_value_error ("%s has nothing in it yet", n->name);
            now = map2 (n->op, had, v);
            if (is_error (now))
              return now;
            /* x++ answers with what x was, as it does in Mathematica;
             * x += 2 answers with what x has become. */
            {
              g_autoptr (M42Value) before = m42_value_ref (had);

              assign (s, n->name, now);
              return n->number != 0 ? g_steal_pointer (&before) : m42_value_ref (now);
            }
          }

        assign (s, n->name, m42_value_ref (v));
        return g_steal_pointer (&v);
      }

    case M42_NODE_DEFINE:
      return define_rule (s, n);

    case M42_NODE_FUNCDEF:
    case M42_NODE_LAMBDA:
      {
        guint np = n->children->len - 1;
        GStrv params = g_new0 (char *, np + 1);
        M42Value *f;
        for (guint i = 0; i < np; i++)
          params[i] = g_strdup (m42_node_child (n, i)->name);
        f = m42_value_func (params, m42_node_copy (m42_node_child (n, np)));
        if (n->kind == M42_NODE_FUNCDEF)
          {
            g_hash_table_insert (s->globals, g_strdup (n->name), f);
            return m42_value_null ();
          }
        return f;
      }

    case M42_NODE_PATTERN:
    case M42_NODE_CONDITION:
      /* A shape is not worked out; it is carried about as written,
       * until something is held up against it. */
      return m42_value_expr (m42_node_copy (n));

    case M42_NODE_RULE:
      {
        g_autoptr (M42Value) rhs = NULL;
        M42Node *r, *rn;

        /* A rule with a shape on its left keeps its right side as it
         * was written: the names in it stand for nothing yet.  So does
         * a rule written with :>, which is what that is for. */
        if (n->op || m42_node_has_pattern (m42_node_child (n, 0)))
          return m42_value_expr (m42_node_copy (n));

        rhs = eval (s, m42_node_child (n, 1));
        if (is_error (rhs))
          return g_steal_pointer (&rhs);
        rn = value_to_node (rhs);
        if (rn == NULL)
          return m42_value_error ("A rule needs a value on its right");
        r = m42_node_new (M42_NODE_RULE);
        g_ptr_array_add (r->children, m42_node_copy (m42_node_child (n, 0)));
        g_ptr_array_add (r->children, rn);
        return m42_value_expr (r);
      }

    case M42_NODE_REPLACE:
      {
        g_autoptr (M42Value) lhs = eval (s, m42_node_child (n, 0));
        M42Node *tree;
        /* //. goes round again as long as something changed; /. is one
         * pass, and the guard is there in case a rule feeds itself. */
        int passes = n->op ? 128 : 1;

        if (is_error (lhs))
          return g_steal_pointer (&lhs);
        tree = value_to_node (lhs);
        if (tree == NULL)
          return m42_value_error ("/. expects an expression on its left");

        for (int pass = 0; pass < passes; pass++)
          {
            gboolean changed = FALSE;

            for (guint i = 1; i < n->children->len; i++)
              {
                g_autoptr (M42Value) rv = eval (s, m42_node_child (n, i));
                GPtrArray *rules = g_ptr_array_new ();

                if (is_error (rv))
                  {
                    m42_node_free (tree);
                    g_ptr_array_unref (rules);
                    return g_steal_pointer (&rv);
                  }
                if (rv->kind == M42_VALUE_LIST)
                  for (guint k = 0; k < m42_value_list_length (rv); k++)
                    g_ptr_array_add (rules, m42_value_list_nth (rv, k));
                else
                  g_ptr_array_add (rules, rv);
                for (guint k = 0; k < rules->len; k++)
                  {
                    M42Value *rule = g_ptr_array_index (rules, k);
                    const M42Node *from, *to;
                    M42Node *replaced;

                    if (rule->kind != M42_VALUE_EXPR || rule->u.expr->kind != M42_NODE_RULE)
                      {
                        m42_node_free (tree);
                        g_ptr_array_unref (rules);
                        return m42_value_error ("/. expects rules like x -> 2");
                      }
                    from = m42_node_child (rule->u.expr, 0);
                    to = m42_node_child (rule->u.expr, 1);

                    /* A name is replaced wherever it stands; anything
                     * else is a shape to look for. */
                    if (from->kind == M42_NODE_IDENT)
                      {
                        replaced = m42_node_substitute (tree, from->name, to);
                        changed = TRUE;
                      }
                    else
                      replaced = m42_node_replace_all (tree, from, to, pattern_test, s,
                                                       &changed);
                    m42_node_free (tree);
                    tree = replaced;
                  }
                g_ptr_array_unref (rules);
              }
            if (!changed)
              break;
          }
        {
          M42Value *answer = eval (s, tree);

          m42_node_free (tree);
          return answer;
        }
      }

    case M42_NODE_SEQ:
      {
        M42Value *r = m42_value_null ();
        for (guint i = 0; i < n->children->len; i++)
          {
            m42_value_unref (r);
            r = eval (s, m42_node_child (n, i));
            if (is_error (r))
              return r;
          }
        if (n->op)
          {
            m42_value_unref (r);
            return m42_value_null ();
          }
        return r;
      }
    }
  return m42_value_error ("Internal error: unknown node");
}

M42Value *
m42_session_eval (M42Session *s, const char *src)
{
  g_autofree char *error = NULL;
  g_autoptr (M42Node) tree = m42_parse (src, &error);
  M42Value *result;

  if (tree == NULL)
    {
      /* A line with nothing on it but a comment is not a mistake: a
       * notebook that says what it is doing between its cells should
       * not be told off for it.  Nothing is evaluated and no number is
       * used up. */
      if (m42_source_is_blank (src))
        return m42_value_null ();

      /* A line that would not parse still used up its number. */
      s->line++;
      return m42_value_error ("Syntax error: %s", error);
    }

  result = eval (s, tree);
  s->line++;
  if (result->kind != M42_VALUE_ERROR && result->kind != M42_VALUE_NULL)
    {
      g_clear_pointer (&s->last, m42_value_unref);
      s->last = m42_value_ref (result);
    }
  return result;
}
