/* m42-mat.c - see m42-mat.h
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The MAT file, level 5, which is what MATLAB has written since 1996
 * and what Octave writes too.  It is a 128-byte header and then a run
 * of elements, each a kind and a length and that many bytes, padded to
 * eight.  A variable is an element of kind miMATRIX holding four of
 * them: its flags and class, its shape, its name, and its numbers,
 * which are laid down a column at a time.
 *
 * Version 7 is the same file with every variable deflated, which is
 * why an element of kind miCOMPRESSED holds a whole element inside it.
 * GLib's zlib decompressor does that part.
 *
 * math42 reads both and writes the uncompressed kind: a file MATLAB
 * loads without a word, and one a person can look at with a hex
 * editor when it does not.
 */

#include "m42-mat.h"

#include <gio/gio.h>

#include <string.h>

/* The kinds an element can be. */
enum {
  MI_INT8 = 1, MI_UINT8 = 2, MI_INT16 = 3, MI_UINT16 = 4,
  MI_INT32 = 5, MI_UINT32 = 6, MI_SINGLE = 7, MI_DOUBLE = 9,
  MI_INT64 = 12, MI_UINT64 = 13, MI_MATRIX = 14, MI_COMPRESSED = 15,
  MI_UTF8 = 16, MI_UTF16 = 17,
};

/* The classes a matrix can be, of which two matter here. */
enum {
  MX_CHAR = 4, MX_DOUBLE = 6,
};

/* A value as a double, for the kinds a MAT file can hold. */
static double
as_a_number (const M42Value *v)
{
  if (v == NULL)
    return 0;
  if (v->kind == M42_VALUE_NUMBER)
    return v->u.number;
  if (v->kind == M42_VALUE_COMPLEX)
    return v->u.cx.re;
  if (v->kind == M42_VALUE_BIGINT)
    return m42_big_to_double (v->u.big);
  return 0;
}

typedef struct {
  const guint8 *data;
  gsize         len;
  gsize         at;
  gboolean      swap;   /* the file was written the other way round */
} Reader;

static guint16
read16 (Reader *r)
{
  guint16 x;

  if (r->at + 2 > r->len)
    return 0;
  memcpy (&x, r->data + r->at, 2);
  r->at += 2;
  return r->swap ? GUINT16_SWAP_LE_BE (x) : x;
}

static guint32
read32 (Reader *r)
{
  guint32 x;

  if (r->at + 4 > r->len)
    return 0;
  memcpy (&x, r->data + r->at, 4);
  r->at += 4;
  return r->swap ? GUINT32_SWAP_LE_BE (x) : x;
}

/* One number of whatever kind the file says, as a double. */
static double
read_number (Reader *r, guint32 kind)
{
  switch (kind)
    {
    case MI_INT8:   { gint8 x = 0; if (r->at < r->len) x = (gint8) r->data[r->at]; r->at += 1; return x; }
    /* The letters of a string come as UTF-8 from anything modern and
     * as sixteen-bit numbers from MATLAB itself; both are read here,
     * and an unknown kind is stepped over rather than read as
     * nothing for ever. */
    case MI_UTF8:
    case MI_UINT8:  { guint8 x = 0; if (r->at < r->len) x = r->data[r->at]; r->at += 1; return x; }
    case MI_INT16:  return (gint16) read16 (r);
    case MI_UINT16: case MI_UTF16: return read16 (r);
    case MI_INT32:  return (gint32) read32 (r);
    case MI_UINT32: return read32 (r);
    case MI_SINGLE:
      {
        guint32 bits = read32 (r);
        float f;

        memcpy (&f, &bits, 4);
        return f;
      }
    case MI_DOUBLE:
      {
        guint32 lo = read32 (r), hi = read32 (r);
        guint64 bits = ((guint64) hi << 32) | lo;
        double d;

        memcpy (&d, &bits, 8);
        return d;
      }
    case MI_INT64: case MI_UINT64:
      {
        guint32 lo = read32 (r), hi = read32 (r);

        return (double) (((guint64) hi << 32) | lo);
      }
    default:
      r->at += 8;
      return 0;
    }
}

static gsize
size_of (guint32 kind)
{
  switch (kind)
    {
    case MI_INT8: case MI_UINT8: case MI_UTF8:   return 1;
    case MI_INT16: case MI_UINT16: case MI_UTF16: return 2;
    case MI_INT32: case MI_UINT32: case MI_SINGLE: return 4;
    default: return 8;
    }
}

/* The head of an element: its kind, how many bytes of data it has, and
 * where they begin.  A short element writes both in one word, which is
 * how MATLAB keeps a name of four letters in eight bytes. */
static gboolean
element_head (Reader *r, guint32 *kind, guint32 *bytes, gboolean *is_short)
{
  guint32 first;

  if (r->at + 8 > r->len)
    return FALSE;
  first = read32 (r);
  if ((first >> 16) != 0)
    {
      *is_short = TRUE;
      *kind = first & 0xffff;
      *bytes = first >> 16;
      return TRUE;
    }
  *is_short = FALSE;
  *kind = first;
  *bytes = read32 (r);
  return TRUE;
}

static void
skip_padding (Reader *r, guint32 bytes, gboolean is_short)
{
  if (is_short)
    r->at += 4 - MIN (bytes, 4u);
  else if (bytes % 8 != 0)
    r->at += 8 - bytes % 8;
}

/* A matrix laid down a column at a time, written back as a list of
 * rows -- which is how math42 holds one. */
static M42Value *
rows_from_columns (const double *numbers, guint rows, guint cols)
{
  M42Value *out = m42_value_list_new ();

  for (guint i = 0; i < rows; i++)
    {
      M42Value *row = m42_value_list_new ();

      for (guint j = 0; j < cols; j++)
        m42_value_list_append (row, m42_value_number (numbers[j * rows + i]));
      m42_value_list_append (out, row);
    }
  return out;
}

static M42Value *read_matrix (Reader *r, gsize end, char **name);

/* Everything a compressed element holds, which is one element of its
 * own once it has been let out. */
static M42Value *
read_compressed (Reader *r, guint32 bytes, char **name)
{
  g_autoptr (GZlibDecompressor) out = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_ZLIB);
  g_autoptr (GByteArray) plain = g_byte_array_new ();
  gsize taken = 0;
  M42Value *value = NULL;

  while (taken < bytes)
    {
      guint8 buffer[16384];
      gsize used = 0, made = 0;
      g_autoptr (GError) trouble = NULL;
      GConverterResult step =
        g_converter_convert (G_CONVERTER (out), r->data + r->at + taken, bytes - taken,
                             buffer, sizeof buffer, G_CONVERTER_INPUT_AT_END,
                             &used, &made, &trouble);

      if (made > 0)
        g_byte_array_append (plain, buffer, (guint) made);
      taken += used;
      if (step == G_CONVERTER_FINISHED)
        break;
      if (step == G_CONVERTER_ERROR)
        return NULL;
      if (used == 0 && made == 0)
        break;
    }
  {
    Reader inner = { plain->data, plain->len, 0, r->swap };
    guint32 kind, inner_bytes;
    gboolean is_short;

    if (element_head (&inner, &kind, &inner_bytes, &is_short) && kind == MI_MATRIX)
      value = read_matrix (&inner, MIN (inner.at + inner_bytes, inner.len), name);
  }
  return value;
}

/* One variable: its flags, its shape, its name and its numbers. */
static M42Value *
read_matrix (Reader *r, gsize end, char **name)
{
  guint32 kind, bytes;
  gboolean is_short;
  guint32 class_of = 0;
  g_autofree guint32 *shape = NULL;
  guint ndims = 0;
  M42Value *value = NULL;

  *name = NULL;

  /* Array flags. */
  if (!element_head (r, &kind, &bytes, &is_short))
    return NULL;
  {
    guint32 flags = read32 (r);

    read32 (r);
    class_of = flags & 0xff;
  }

  /* The shape. */
  if (!element_head (r, &kind, &bytes, &is_short))
    return NULL;
  ndims = bytes / 4;
  shape = g_new0 (guint32, MAX (ndims, 2u));
  for (guint i = 0; i < ndims; i++)
    shape[i] = read32 (r);
  skip_padding (r, bytes, is_short);

  /* The name. */
  if (!element_head (r, &kind, &bytes, &is_short))
    return NULL;
  if (r->at + bytes <= r->len)
    *name = g_strndup ((const char *) r->data + r->at, bytes);
  r->at += bytes;
  skip_padding (r, bytes, is_short);

  /* The numbers, or the letters. */
  if (!element_head (r, &kind, &bytes, &is_short))
    return NULL;
  {
    guint rows = ndims > 0 ? shape[0] : 0;
    guint cols = ndims > 1 ? shape[1] : 1;
    gsize how_many = (gsize) rows * cols;
    gsize have = size_of (kind) > 0 ? bytes / size_of (kind) : 0;

    if (how_many > have)
      how_many = have;
    if (class_of == MX_CHAR)
      {
        g_autoptr (GString) text = g_string_new (NULL);

        for (gsize i = 0; i < how_many; i++)
          {
            gunichar c = (gunichar) read_number (r, kind);

            if (c != 0)
              g_string_append_unichar (text, c);
          }
        value = m42_value_string (text->str);
      }
    else
      {
        g_autofree double *numbers = g_new0 (double, MAX (how_many, (gsize) 1));

        for (gsize i = 0; i < how_many; i++)
          numbers[i] = read_number (r, kind);
        if (rows == 1 || cols == 1)
          {
            /* A row or a column of numbers is a plain list, as it is
             * everywhere else in math42. */
            M42Value *flat = m42_value_list_new ();

            for (gsize i = 0; i < how_many; i++)
              m42_value_list_append (flat, m42_value_number (numbers[i]));
            value = how_many == 1 ? m42_value_number (numbers[0]) : flat;
            if (how_many == 1)
              m42_value_unref (flat);
          }
        else
          value = rows_from_columns (numbers, rows, cols);
      }
  }
  r->at = end;
  return value;
}

M42Value *
m42_mat_read (const char *path, GStrv *names, GError **error)
{
  g_autofree char *contents = NULL;
  gsize length = 0;
  Reader r;
  M42Value *found;
  guint how_many = 0;

  if (!g_file_get_contents (path, &contents, &length, error))
    return NULL;
  if (length < 128)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "%s is too short to be a MAT file", path);
      return NULL;
    }
  if (strncmp (contents, "MATLAB 5.0 MAT-file", 19) != 0)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "%s is not a level 5 MAT file; math42 does not read the older "
                   "level 4 kind or the HDF5 files -v7.3 writes", path);
      return NULL;
    }

  r.data = (const guint8 *) contents;
  r.len = length;
  r.at = 126;
  r.swap = FALSE;
  {
    /* Two letters near the end of the header say which way round the
     * numbers are: IM as written, MI when they need turning. */
    guint16 marker;

    memcpy (&marker, contents + 126, 2);
    r.swap = ((const guint8 *) contents)[126] == 'M';
  }
  r.at = 128;

  found = m42_value_list_new ();
  {
    g_autoptr (GPtrArray) got_names = g_ptr_array_new ();

  while (r.at + 8 <= r.len)
    {
      guint32 kind, bytes;
      gboolean is_short;
      gsize head = r.at;
      g_autofree char *name = NULL;
      M42Value *value = NULL;
      gboolean skipped_padding = FALSE;

      if (!element_head (&r, &kind, &bytes, &is_short))
        break;
      if (bytes == 0 || head + bytes > r.len + 8)
        break;
      if (kind == MI_MATRIX)
        value = read_matrix (&r, MIN (r.at + bytes, r.len), &name);
      else if (kind == MI_COMPRESSED)
        {
          value = read_compressed (&r, bytes, &name);
          r.at += bytes;
          /* A compressed element is the one kind that is not padded
           * out to eight bytes.  Padding it anyway put the reader a
           * few bytes past the next one, and the last variable in a
           * version 7 file went missing. */
          skipped_padding = TRUE;
        }
      else
        r.at += bytes;
      if (!skipped_padding)
        skip_padding (&r, bytes, is_short);

      if (value != NULL)
        {
          g_ptr_array_add (got_names,
                           g_strdup (name != NULL && *name != '\0' ? name : "data"));
          m42_value_list_append (found, value);
          how_many++;
        }
    }
  g_ptr_array_add (got_names, NULL);
  if (names != NULL)
    *names = (GStrv) g_ptr_array_free (g_steal_pointer (&got_names), FALSE);
  }

  if (how_many == 0)
    {
      m42_value_unref (found);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "%s holds nothing math42 can read: only matrices of numbers and "
                   "strings, and not cells, structures or objects", path);
      return NULL;
    }
  return found;
}

/* --- writing ------------------------------------------------------------ */

static void
put32 (GByteArray *out, guint32 x)
{
  guint8 bytes[4];

  bytes[0] = x & 0xff;
  bytes[1] = (x >> 8) & 0xff;
  bytes[2] = (x >> 16) & 0xff;
  bytes[3] = (x >> 24) & 0xff;
  g_byte_array_append (out, bytes, 4);
}

static void
put_double (GByteArray *out, double x)
{
  guint64 bits;
  guint8 bytes[8];

  memcpy (&bits, &x, 8);
  for (int i = 0; i < 8; i++)
    bytes[i] = (bits >> (8 * i)) & 0xff;
  g_byte_array_append (out, bytes, 8);
}

static void
pad_to_eight (GByteArray *out)
{
  static const guint8 nothing[8] = { 0 };

  if (out->len % 8 != 0)
    g_byte_array_append (out, nothing, 8 - out->len % 8);
}

/* The shape of a value as MATLAB counts it, and its numbers a column
 * at a time.  Returns FALSE for anything that is not a number, a list
 * of them, or a list of equal rows. */
static gboolean
numbers_of (const M42Value *v, GArray *out, guint *rows, guint *cols)
{
  if (m42_value_is_matrix (v, rows, cols))
    {
      for (guint j = 0; j < *cols; j++)
        for (guint i = 0; i < *rows; i++)
          {
            const M42Value *e = m42_value_list_nth (m42_value_list_nth (v, i), j);
            double x = as_a_number (e);

            g_array_append_val (out, x);
          }
      return TRUE;
    }
  if (m42_value_is_vector (v))
    {
      *rows = 1;
      *cols = m42_value_list_length (v);
      for (guint i = 0; i < *cols; i++)
        {
          double x = as_a_number (m42_value_list_nth (v, i));

          g_array_append_val (out, x);
        }
      return TRUE;
    }
  if (v->kind == M42_VALUE_NUMBER || v->kind == M42_VALUE_BIGINT)
    {
      double x = as_a_number (v);

      *rows = 1;
      *cols = 1;
      g_array_append_val (out, x);
      return TRUE;
    }
  return FALSE;
}

/* One variable, as the element MATLAB looks for. */
static gboolean
append_variable (GByteArray *out, const char *name, const M42Value *value)
{
  g_autoptr (GByteArray) body = g_byte_array_new ();
  g_autoptr (GArray) numbers = g_array_new (FALSE, FALSE, sizeof (double));
  gboolean is_text = value->kind == M42_VALUE_STRING;
  guint rows = 1, cols = 1;

  if (!is_text && !numbers_of (value, numbers, &rows, &cols))
    return FALSE;
  if (is_text)
    {
      rows = 1;
      cols = (guint) g_utf8_strlen (value->u.string, -1);
    }

  /* Array flags. */
  put32 (body, MI_UINT32);
  put32 (body, 8);
  put32 (body, is_text ? MX_CHAR : MX_DOUBLE);
  put32 (body, 0);

  /* The shape. */
  put32 (body, MI_INT32);
  put32 (body, 8);
  put32 (body, rows);
  put32 (body, cols);

  /* The name. */
  put32 (body, MI_INT8);
  put32 (body, (guint32) strlen (name));
  g_byte_array_append (body, (const guint8 *) name, (guint) strlen (name));
  pad_to_eight (body);

  /* The numbers, or the letters, a column at a time. */
  if (is_text)
    {
      put32 (body, MI_UINT16);
      put32 (body, cols * 2);
      for (const char *p = value->u.string; *p != '\0'; p = g_utf8_next_char (p))
        {
          gunichar c = g_utf8_get_char (p);
          guint8 two[2] = { (guint8) (c & 0xff), (guint8) ((c >> 8) & 0xff) };

          g_byte_array_append (body, two, 2);
        }
      pad_to_eight (body);
    }
  else
    {
      put32 (body, MI_DOUBLE);
      put32 (body, (guint32) (numbers->len * 8));
      for (guint i = 0; i < numbers->len; i++)
        put_double (body, g_array_index (numbers, double, i));
    }

  put32 (out, MI_MATRIX);
  put32 (out, body->len);
  g_byte_array_append (out, body->data, body->len);
  return TRUE;
}

gboolean
m42_mat_write (const char *path, const char *const *names, const M42Value *values,
               GError **error)
{
  g_autoptr (GByteArray) out = g_byte_array_new ();
  guint written = 0;

  /* The header: a line of text, then the version and which way round
   * the numbers are. */
  {
    char header[128];
    g_autofree char *text =
      g_strdup_printf ("MATLAB 5.0 MAT-file, written by math42, %s",
                       "https://github.com/office-42/math42");

    memset (header, ' ', sizeof header);
    memcpy (header, text, MIN (strlen (text), (gsize) 116));
    memset (header + 116, 0, 8);
    header[124] = 0x00;
    header[125] = 0x01;
    header[126] = 'I';
    header[127] = 'M';
    g_byte_array_append (out, (const guint8 *) header, sizeof header);
  }

  for (guint i = 0; i < m42_value_list_length (values); i++)
    {
      const char *its_name = names != NULL && names[i] != NULL ? names[i] : NULL;
      g_autofree char *made_up = its_name == NULL ? g_strdup_printf ("v%u", i + 1)
                                                  : NULL;

      if (append_variable (out, its_name != NULL ? its_name : made_up,
                           m42_value_list_nth (values, i)))
        written++;
    }

  if (written == 0)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "a MAT file holds numbers and strings, and that is neither");
      return FALSE;
    }
  return g_file_set_contents (path, (const char *) out->data, out->len, error);
}
