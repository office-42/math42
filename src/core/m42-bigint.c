/* m42-bigint.c - see m42-bigint.h
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Schoolbook arithmetic, in base a thousand million: addition and
 * subtraction digit by digit with a carry, multiplication by the long
 * method, and powers by squaring.  Nothing clever, because nothing
 * clever is needed at the sizes a notebook produces -- and the digits
 * being a power of ten means printing is only a matter of writing them
 * out.
 */

#include "m42-bigint.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static M42Big *
big_alloc (guint len)
{
  M42Big *a = g_new0 (M42Big, 1);

  a->len = MAX (len, 1);
  a->digit = g_new0 (guint32, a->len);
  return a;
}

/* Leading zeros dropped, and zero given the sign it deserves. */
static M42Big *
big_trim (M42Big *a)
{
  while (a->len > 1 && a->digit[a->len - 1] == 0)
    a->len--;
  if (a->len == 1 && a->digit[0] == 0)
    a->sign = 0;
  return a;
}

void
m42_big_free (M42Big *a)
{
  if (a == NULL)
    return;
  g_free (a->digit);
  g_free (a);
}

M42Big *
m42_big_copy (const M42Big *a)
{
  M42Big *out = big_alloc (a->len);

  out->sign = a->sign;
  memcpy (out->digit, a->digit, sizeof (guint32) * a->len);
  return out;
}

M42Big *
m42_big_from_int64 (gint64 x)
{
  M42Big *out = big_alloc (3);
  guint64 left;

  out->sign = x == 0 ? 0 : (x < 0 ? -1 : 1);
  /* Taken as unsigned so that the most negative number still works. */
  left = x < 0 ? (guint64) (-(x + 1)) + 1 : (guint64) x;
  for (guint i = 0; i < 3 && left > 0; i++)
    {
      out->digit[i] = (guint32) (left % M42_BIG_BASE);
      left /= M42_BIG_BASE;
    }
  return big_trim (out);
}

M42Big *
m42_big_from_string (const char *text)
{
  gsize length = strlen (text);
  gsize start = 0;
  int sign = 1;
  M42Big *out;
  gsize digits;

  if (length > 0 && (text[0] == '-' || text[0] == '+'))
    {
      sign = text[0] == '-' ? -1 : 1;
      start = 1;
    }
  digits = length - start;
  if (digits == 0)
    return NULL;
  for (gsize i = start; i < length; i++)
    if (text[i] < '0' || text[i] > '9')
      return NULL;

  out = big_alloc ((digits + 8) / 9);
  for (gsize i = 0; i < out->len; i++)
    {
      /* Nine digits at a time, from the right. */
      gsize end = length - i * 9;
      gsize begin = end > start + 9 ? end - 9 : start;
      guint32 piece = 0;

      for (gsize k = begin; k < end; k++)
        piece = piece * 10 + (text[k] - '0');
      out->digit[i] = piece;
    }
  out->sign = sign;
  return big_trim (out);
}

gboolean
m42_big_is_zero (const M42Big *a)
{
  return a->sign == 0;
}

/* Which is the larger, sign and all. */
int
m42_big_compare (const M42Big *a, const M42Big *b)
{
  if (a->sign != b->sign)
    return a->sign < b->sign ? -1 : 1;
  {
    int order = 0;

    if (a->len != b->len)
      order = a->len < b->len ? -1 : 1;
    else
      for (guint i = a->len; i > 0 && order == 0; i--)
        if (a->digit[i - 1] != b->digit[i - 1])
          order = a->digit[i - 1] < b->digit[i - 1] ? -1 : 1;
    return a->sign < 0 ? -order : order;
  }
}

/* The size of a against the size of b, signs ignored. */
static int
compare_size (const M42Big *a, const M42Big *b)
{
  if (a->len != b->len)
    return a->len < b->len ? -1 : 1;
  for (guint i = a->len; i > 0; i--)
    if (a->digit[i - 1] != b->digit[i - 1])
      return a->digit[i - 1] < b->digit[i - 1] ? -1 : 1;
  return 0;
}

static M42Big *add_sizes (const M42Big *a, const M42Big *b);
static M42Big *subtract_sizes (const M42Big *a, const M42Big *b);

/* The two of them added, signs ignored. */
static M42Big *
add_sizes (const M42Big *a, const M42Big *b)
{
  M42Big *out = big_alloc (MAX (a->len, b->len) + 1);
  guint32 carry = 0;

  for (guint i = 0; i < out->len; i++)
    {
      guint32 total = carry;

      if (i < a->len)
        total += a->digit[i];
      if (i < b->len)
        total += b->digit[i];
      out->digit[i] = total % M42_BIG_BASE;
      carry = total / M42_BIG_BASE;
    }
  out->sign = 1;
  return big_trim (out);
}

/* The smaller taken from the larger, signs ignored; a must be the
 * larger of the two. */
static M42Big *
subtract_sizes (const M42Big *a, const M42Big *b)
{
  M42Big *out = big_alloc (a->len);
  int borrow = 0;

  for (guint i = 0; i < a->len; i++)
    {
      gint64 total = (gint64) a->digit[i] - borrow - (i < b->len ? b->digit[i] : 0);

      if (total < 0)
        {
          total += M42_BIG_BASE;
          borrow = 1;
        }
      else
        borrow = 0;
      out->digit[i] = (guint32) total;
    }
  out->sign = 1;
  return big_trim (out);
}

M42Big *
m42_big_add (const M42Big *a, const M42Big *b)
{
  M42Big *out;

  if (a->sign == 0)
    return m42_big_copy (b);
  if (b->sign == 0)
    return m42_big_copy (a);

  if (a->sign == b->sign)
    {
      out = add_sizes (a, b);
      out->sign = a->sign;
      return big_trim (out);
    }

  /* One of each: the smaller comes off the larger. */
  {
    int order = compare_size (a, b);

    if (order == 0)
      return m42_big_from_int64 (0);
    if (order > 0)
      {
        out = subtract_sizes (a, b);
        out->sign = a->sign;
      }
    else
      {
        out = subtract_sizes (b, a);
        out->sign = b->sign;
      }
    return big_trim (out);
  }
}

M42Big *
m42_big_subtract (const M42Big *a, const M42Big *b)
{
  g_autoptr (M42Big) negated = m42_big_copy (b);

  negated->sign = -negated->sign;
  return m42_big_add (a, negated);
}

M42Big *
m42_big_multiply (const M42Big *a, const M42Big *b)
{
  M42Big *out;

  if (a->sign == 0 || b->sign == 0)
    return m42_big_from_int64 (0);
  out = big_alloc (a->len + b->len);

  for (guint i = 0; i < a->len; i++)
    {
      guint64 carry = 0;

      for (guint j = 0; j < b->len || carry > 0; j++)
        {
          guint64 total = out->digit[i + j] + carry;

          if (j < b->len)
            total += (guint64) a->digit[i] * b->digit[j];
          out->digit[i + j] = (guint32) (total % M42_BIG_BASE);
          carry = total / M42_BIG_BASE;
        }
    }
  out->sign = a->sign * b->sign;
  return big_trim (out);
}

M42Big *
m42_big_power (const M42Big *a, guint64 e)
{
  M42Big *out = m42_big_from_int64 (1);
  g_autoptr (M42Big) base = m42_big_copy (a);

  /* Squaring as it goes, with a guard so that a runaway power cannot
   * eat the machine: a hundred thousand digits is already far past
   * anything anyone reads. */
  while (e > 0)
    {
      if (e & 1)
        {
          M42Big *next = m42_big_multiply (out, base);

          m42_big_free (out);
          out = next;
        }
      e >>= 1;
      if (e > 0)
        {
          M42Big *squared = m42_big_multiply (base, base);

          m42_big_free (base);
          base = squared;
        }
      if (out->len > 12000 || base->len > 12000)
        {
          m42_big_free (out);
          return NULL;
        }
    }
  return out;
}

M42Big *
m42_big_divide_small (const M42Big *a, gint64 d, gint64 *remainder)
{
  M42Big *out;
  gint64 carry = 0;
  gint64 size = d < 0 ? -d : d;

  if (d == 0)
    return NULL;
  out = big_alloc (a->len);
  for (guint i = a->len; i > 0; i--)
    {
      gint64 current = carry * M42_BIG_BASE + a->digit[i - 1];

      out->digit[i - 1] = (guint32) (current / size);
      carry = current % size;
    }
  out->sign = a->sign == 0 ? 0 : (d < 0 ? -a->sign : a->sign);
  if (remainder != NULL)
    *remainder = a->sign < 0 ? -carry : carry;
  return big_trim (out);
}

M42Big *
m42_big_factorial (guint n)
{
  M42Big *out = m42_big_from_int64 (1);

  for (guint i = 2; i <= n; i++)
    {
      g_autoptr (M42Big) step = m42_big_from_int64 (i);
      M42Big *next = m42_big_multiply (out, step);

      m42_big_free (out);
      out = next;
      if (out->len > 12000)
        {
          m42_big_free (out);
          return NULL;
        }
    }
  return out;
}

gboolean
m42_big_fits_int64 (const M42Big *a, gint64 *out)
{
  gint64 total = 0;

  if (a->len > 3)
    return FALSE;
  for (guint i = a->len; i > 0; i--)
    {
      if (total > (G_MAXINT64 - a->digit[i - 1]) / M42_BIG_BASE)
        return FALSE;
      total = total * M42_BIG_BASE + a->digit[i - 1];
    }
  *out = a->sign < 0 ? -total : total;
  return TRUE;
}

double
m42_big_to_double (const M42Big *a)
{
  double total = 0;

  for (guint i = a->len; i > 0; i--)
    total = total * M42_BIG_BASE + a->digit[i - 1];
  return a->sign < 0 ? -total : total;
}

char *
m42_big_to_string (const M42Big *a)
{
  GString *out = g_string_new (NULL);

  if (a->sign < 0)
    g_string_append_c (out, '-');
  g_string_append_printf (out, "%u", a->digit[a->len - 1]);
  for (guint i = a->len - 1; i > 0; i--)
    g_string_append_printf (out, "%09u", a->digit[i - 1]);
  return g_string_free (out, FALSE);
}
