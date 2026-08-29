/* m42-format.c - reading and writing the files other programs write
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A math42 notebook is a plain list of inputs, one to a line, and that
 * is what a .m42 file holds.  The three formats here are the ones a
 * person coming from either program already has on disk:
 *
 *   .m         a MATLAB script.  Comments begin with %, which math42
 *              reads as the previous output, so they are turned into
 *              (* ... *) on the way in and back on the way out.  A
 *              line ending in ... is continued.
 *   .wl, .wls  a Wolfram Language script: the same language math42
 *              speaks, so there is nothing to translate -- only lines
 *              to put back together where an expression was spread
 *              over several.
 *   .nb        a Mathematica notebook, which is itself a Wolfram
 *              expression: Notebook[{Cell[...], ...}].  math42 writes
 *              the small part of that a notebook of plain input cells
 *              needs, and reads the input cells out of one.
 *
 * Nothing here evaluates anything: a file becomes a list of inputs and
 * a list of inputs becomes a file, and the notebook does the rest.
 */

#include "m42-format.h"

#include <string.h>

M42Format
m42_format_for_path (const char *path)
{
  g_autofree char *lower = path != NULL ? g_ascii_strdown (path, -1) : NULL;

  if (lower == NULL)
    return M42_FORMAT_M42;
  if (g_str_has_suffix (lower, ".m"))
    return M42_FORMAT_MATLAB;
  if (g_str_has_suffix (lower, ".wl") || g_str_has_suffix (lower, ".wls") ||
      g_str_has_suffix (lower, ".m42w"))
    return M42_FORMAT_WOLFRAM;
  if (g_str_has_suffix (lower, ".nb"))
    return M42_FORMAT_NOTEBOOK;
  return M42_FORMAT_M42;
}

const char *
m42_format_name (M42Format format)
{
  switch (format)
    {
    case M42_FORMAT_MATLAB:   return "MATLAB script";
    case M42_FORMAT_WOLFRAM:  return "Wolfram Language script";
    case M42_FORMAT_NOTEBOOK: return "Mathematica notebook";
    default:                  return "math42 notebook";
    }
}

/* --- putting a broken line back together --------------------------------- */

/* How far the brackets are still open at the end of the text, ignoring
 * what is inside a string or a comment.  A line that ends with more
 * open than closed is a line that has not finished. */
static int
open_brackets (const char *text, int so_far)
{
  gboolean in_string = FALSE;
  int comment = 0;

  for (const char *at = text; *at != '\0'; at++)
    {
      if (comment > 0)
        {
          if (at[0] == '*' && at[1] == ')')
            {
              comment--;
              at++;
            }
          else if (at[0] == '(' && at[1] == '*')
            {
              comment++;
              at++;
            }
          continue;
        }
      if (in_string)
        {
          if (at[0] == '\\' && at[1] != '\0')
            at++;
          else if (at[0] == '"')
            in_string = FALSE;
          continue;
        }
      if (at[0] == '"')
        in_string = TRUE;
      else if (at[0] == '(' && at[1] == '*')
        {
          comment++;
          at++;
        }
      else if (at[0] == '[' || at[0] == '{' || at[0] == '(')
        so_far++;
      else if (at[0] == ']' || at[0] == '}' || at[0] == ')')
        so_far--;
    }
  return so_far;
}

/* Lines joined into inputs: one line is one input unless its brackets
 * are still open, in which case the next line belongs with it. */
static char *
join_broken_lines (const char *contents)
{
  g_auto (GStrv) lines = g_strsplit (contents, "\n", -1);
  GString *out = g_string_new (NULL);
  GString *held = g_string_new (NULL);
  int depth = 0;

  for (guint i = 0; lines[i] != NULL; i++)
    {
      char *line = g_strchomp (lines[i]);

      if (depth <= 0 && line[0] == '\0')
        continue;
      if (held->len > 0)
        g_string_append_c (held, ' ');
      g_string_append (held, line);
      depth = open_brackets (line, depth);
      if (depth <= 0)
        {
          g_string_append (out, held->str);
          g_string_append_c (out, '\n');
          g_string_truncate (held, 0);
          depth = 0;
        }
    }
  if (held->len > 0)
    {
      g_string_append (out, held->str);
      g_string_append_c (out, '\n');
    }
  g_string_free (held, TRUE);
  return g_string_free (out, FALSE);
}

/* --- MATLAB ------------------------------------------------------------- */

/* A MATLAB comment runs from an unquoted % to the end of the line.
 * The same character means the previous output in math42, so it is
 * written the other way here. */
static void
matlab_line_in (GString *out, const char *line)
{
  char opened_with = 0;
  const char *at;

  for (at = line; *at != '\0'; at++)
    {
      if (opened_with != 0)
        {
          if (at[0] == '\\' && at[1] != '\0')
            {
              g_string_append_c (out, *at++);
              g_string_append_c (out, *at);
              continue;
            }
          /* MATLAB writes an apostrophe inside its own kind of string
           * by doubling it, so 'it''s' is one string. */
          if (opened_with == '\'' && at[0] == '\'' && at[1] == '\'')
            {
              g_string_append_c (out, '\'');
              at++;
              continue;
            }
          /* Only the quote that opened the string closes it: "it's"
           * is one string with an apostrophe in it, and reading the
           * apostrophe as the end of it turned the line into
           * nonsense. */
          if (at[0] == opened_with)
            {
              opened_with = 0;
              g_string_append_c (out, '"');
              continue;
            }
          /* math42 writes its strings with double quotes, so one
           * inside has to be spelt out. */
          if (at[0] == '"')
            {
              g_string_append (out, "\\\"");
              continue;
            }
          g_string_append_c (out, *at);
          continue;
        }
      if (at[0] == '"' || at[0] == '\'')
        {
          /* A quote after a name or a bracket is MATLAB's transpose,
           * not the start of a string -- and so is a quote after
           * another one, which is how a second derivative is written.
           * Without that, y'' was read as y' and then a string that
           * swallowed the rest of the file. */
          gboolean transpose = at[0] == '\'' && at != line &&
                               (g_ascii_isalnum (at[-1]) || at[-1] == ')' ||
                                at[-1] == ']' || at[-1] == '}' || at[-1] == '\'');

          if (!transpose)
            opened_with = at[0];
          /* math42 writes a string one way only, so MATLAB's single
           * quotes become double ones.  A transpose is left as it is,
           * since math42 writes that with a quote as well. */
          g_string_append_c (out, transpose ? *at : '"');
          continue;
        }
      if (at[0] == '%')
        {
          const char *text = at + 1;

          while (*text == '%' || *text == ' ')
            text++;
          if (*text != '\0')
            g_string_append_printf (out, "(* %s *)", text);
          return;
        }
      g_string_append_c (out, *at);
    }
}

static char *
matlab_read (const char *contents)
{
  g_auto (GStrv) lines = g_strsplit (contents, "\n", -1);
  g_autoptr (GString) plain = g_string_new (NULL);

  for (guint i = 0; lines[i] != NULL; i++)
    {
      g_autoptr (GString) one = g_string_new (NULL);
      char *line = g_strstrip (lines[i]);

      matlab_line_in (one, line);
      g_strchomp (one->str);
      one->len = strlen (one->str);

      /* ... at the end carries on to the next line. */
      if (one->len >= 3 && strcmp (one->str + one->len - 3, "...") == 0)
        {
          g_string_truncate (one, one->len - 3);
          g_string_append (plain, one->str);
          continue;
        }
      g_string_append (plain, one->str);
      g_string_append_c (plain, '\n');
    }
  return join_broken_lines (plain->str);
}

static char *
matlab_write (const char *inputs)
{
  g_auto (GStrv) lines = g_strsplit (inputs, "\n", -1);
  /* No line of our own at the top: a file holds what the notebook
   * holds and nothing else, so that writing it and reading it back
   * gives the same thing however many times it goes round. */
  GString *out = g_string_new (NULL);

  for (guint i = 0; lines[i] != NULL; i++)
    {
      const char *line = lines[i];

      if (line[0] == '\0')
        continue;
      /* A math42 comment becomes a MATLAB one. */
      if (g_str_has_prefix (line, "(*"))
        {
          g_autofree char *inner = g_strdup (line + 2);
          char *close = g_strrstr (inner, "*)");

          if (close != NULL)
            *close = '\0';
          g_string_append_printf (out, "%% %s\n", g_strstrip (inner));
          continue;
        }
      g_string_append (out, line);
      g_string_append_c (out, '\n');
    }
  return g_string_free (out, FALSE);
}

/* --- Wolfram Language ---------------------------------------------------- */

static char *
wolfram_write (const char *inputs)
{
  g_auto (GStrv) lines = g_strsplit (inputs, "\n", -1);
  GString *out = g_string_new (NULL);

  for (guint i = 0; lines[i] != NULL; i++)
    if (lines[i][0] != '\0')
      {
        g_string_append (out, lines[i]);
        g_string_append_c (out, '\n');
      }
  return g_string_free (out, FALSE);
}

/* --- a Mathematica notebook ---------------------------------------------- */

/* The text of a string literal starting at *at, with the escapes
 * undone; *at is left after the closing quote. */
static char *
string_literal (const char **at)
{
  GString *text = g_string_new (NULL);
  const char *p = *at;

  if (*p != '"')
    return g_string_free (text, TRUE);
  for (p++; *p != '\0' && *p != '"'; p++)
    {
      if (p[0] == '\\' && p[1] != '\0')
        {
          p++;
          g_string_append_c (text, *p == 'n' ? '\n' : *p == 't' ? '\t' : *p);
        }
      else
        g_string_append_c (text, *p);
    }
  if (*p == '"')
    p++;
  *at = p;
  return g_string_free (text, FALSE);
}

/* The input cells of a notebook.  A cell is Cell[..., "Input"], and
 * what it holds is one or more string literals -- plainly written for
 * a simple cell, and wrapped in RowBox and its kind for one that
 * Mathematica has laid out.  Taking the strings in order and putting
 * them end to end gives the input back either way. */
/* --- the boxes a notebook is written in ---------------------------------
 *
 * Mathematica writes what was typed as boxes: RowBox for a run of
 * pieces, SuperscriptBox for a power, FractionBox for a fraction laid
 * out over a line, SqrtBox for a root.  Somebody typing x^2 in the
 * front end gets SuperscriptBox["x", "2"], and pasting the strings
 * together would give x2 -- a different thing entirely, and one that
 * would run without a word said.  So the boxes are read as what they
 * are and written back as a line.
 */

/* Whether the text is one piece that needs no brackets around it under
 * a power or over a line: a name, a number, or something already in
 * brackets of its own. */
static gboolean
stands_alone (const char *text)
{
  gsize n = strlen (text);

  if (n == 0)
    return TRUE;
  if (text[0] == '(' && text[n - 1] == ')')
    return TRUE;
  for (gsize i = 0; i < n; i++)
    if (!g_ascii_isalnum (text[i]) && text[i] != '.' && text[i] != '_')
      return FALSE;
  return TRUE;
}

static void
append_piece (GString *out, const char *text)
{
  if (stands_alone (text))
    g_string_append (out, text);
  else
    g_string_append_printf (out, "(%s)", text);
}

static void box_to_text (const char **at, GString *out);

/* The arguments of Name[...], each read into a string of its own.  The
 * bracket has been passed already. */
static GPtrArray *
box_arguments (const char **at)
{
  GPtrArray *args = g_ptr_array_new_with_free_func (g_free);

  for (;;)
    {
      GString *one = g_string_new (NULL);

      box_to_text (at, one);
      g_strstrip (one->str);
      one->len = strlen (one->str);
      g_ptr_array_add (args, g_string_free (one, FALSE));
      while (g_ascii_isspace (**at))
        (*at)++;
      if (**at == ',')
        {
          (*at)++;
          continue;
        }
      if (**at == ']' || **at == '}')
        (*at)++;
      break;
    }
  return args;
}

/* One box, or one piece of text, written back as source.  Stops at a
 * comma or a closing bracket that is not its own. */
static void
box_to_text (const char **at, GString *out)
{
  while (g_ascii_isspace (**at))
    (*at)++;

  if (**at == '"')
    {
      g_autofree char *piece = string_literal (at);

      g_string_append (out, piece);
      return;
    }

  /* A list, as RowBox[{...}] holds: its pieces one after another. */
  if (**at == '{')
    {
      g_autoptr (GPtrArray) args = NULL;

      (*at)++;
      args = box_arguments (at);
      for (guint i = 0; i < args->len; i++)
        g_string_append (out, g_ptr_array_index (args, i));
      return;
    }

  if (g_ascii_isalpha (**at) || **at == '\\')
    {
      const char *start = *at;
      g_autofree char *name = NULL;

      while (g_ascii_isalnum (**at) || **at == '$')
        (*at)++;
      name = g_strndup (start, (gsize) (*at - start));
      while (g_ascii_isspace (**at))
        (*at)++;
      if (**at != '[')
        {
          /* A name standing on its own, which is what it says. */
          g_string_append (out, name);
          return;
        }
      (*at)++;
      {
        g_autoptr (GPtrArray) args = box_arguments (at);
        const char *a = args->len > 0 ? g_ptr_array_index (args, 0) : "";
        const char *b = args->len > 1 ? g_ptr_array_index (args, 1) : "";

        if (strcmp (name, "SuperscriptBox") == 0 && args->len >= 2)
          {
            append_piece (out, a);
            g_string_append_c (out, '^');
            append_piece (out, b);
            return;
          }
        if (strcmp (name, "FractionBox") == 0 && args->len >= 2)
          {
            append_piece (out, a);
            g_string_append_c (out, '/');
            append_piece (out, b);
            return;
          }
        if (strcmp (name, "SqrtBox") == 0 && args->len >= 1)
          {
            g_string_append_printf (out, "Sqrt[%s]", a);
            return;
          }
        if (strcmp (name, "RadicalBox") == 0 && args->len >= 2)
          {
            g_string_append_printf (out, "(%s)^(1/%s)", a, b);
            return;
          }
        if (strcmp (name, "SubscriptBox") == 0 && args->len >= 2)
          {
            /* x with a 1 under it is the name x1, which is a name
             * math42 can hold; there is no Subscript here. */
            g_string_append_printf (out, "%s%s", a, b);
            return;
          }
        /* RowBox, BoxData, StyleBox, TagBox and the rest: what they
         * hold, with anything after the first argument left alone --
         * a style or an option says how it looks, not what it is. */
        if (strcmp (name, "RowBox") == 0 || strcmp (name, "BoxData") == 0)
          {
            for (guint i = 0; i < args->len; i++)
              g_string_append (out, g_ptr_array_index (args, i));
            return;
          }
        g_string_append (out, a);
        return;
      }
    }

  /* Anything else -- an operator, a bracket the front end wrote as a
   * character of its own -- is passed through until the next thing
   * that matters. */
  while (**at != '\0' && **at != ',' && **at != ']' && **at != '}' && **at != '"')
    {
      if (g_ascii_isalpha (**at) || **at == '{')
        return;
      g_string_append_c (out, **at);
      (*at)++;
    }
}

static char *
notebook_read (const char *contents)
{
  GString *out = g_string_new (NULL);
  const char *at = contents;

  while ((at = strstr (at, "Cell[")) != NULL)
    {
      const char *cell = at + 5;
      const char *end = cell;
      int depth = 1;
      gboolean in_string = FALSE;
      g_autoptr (GPtrArray) args = NULL;
      const char *reading;

      /* Where this cell ends. */
      for (; *end != '\0' && depth > 0; end++)
        {
          if (in_string)
            {
              if (end[0] == '\\' && end[1] != '\0')
                end++;
              else if (end[0] == '"')
                in_string = FALSE;
              continue;
            }
          if (end[0] == '"')
            in_string = TRUE;
          else if (end[0] == '[')
            depth++;
          else if (end[0] == ']')
            depth--;
        }

      /* Mathematica puts an input and the output it gave inside one
       * Cell[CellGroupData[{...}]].  That is not a cell to read but a
       * box holding two, so the scan goes on inside it; otherwise the
       * whole group counts as one input and the output is read as part
       * of what was typed. */
      {
        const char *inside = cell;

        while (g_ascii_isspace (*inside))
          inside++;
        if (g_str_has_prefix (inside, "CellGroupData["))
          {
            at = cell;
            continue;
          }
      }
      at = end;

      /* Cell[content, "Style", options...]: only the ones that hold
       * input, and only their content. */
      reading = cell;
      args = box_arguments (&reading);
      if (args->len < 2 || strcmp (g_ptr_array_index (args, 1), "Input") != 0)
        continue;
      {
        char *text = g_ptr_array_index (args, 0);

        g_strstrip (text);
        if (*text != '\0')
          {
            g_string_append (out, text);
            g_string_append_c (out, '\n');
          }
      }
    }
  return g_string_free (out, FALSE);
}

/* One string as a Wolfram one, with the characters that cannot stand
 * for themselves spelt out. */
static void
append_quoted (GString *out, const char *text)
{
  g_string_append_c (out, '"');
  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == '\n')
        {
          g_string_append (out, "\\n");
          continue;
        }
      if (*p == '"' || *p == '\\')
        g_string_append_c (out, '\\');
      g_string_append_c (out, *p);
    }
  g_string_append_c (out, '"');
}

/* A notebook is a Wolfram expression: a list of cells, each of them an
 * input or the output that came of it.  The results are written down
 * as well as the inputs, so that somebody opening the file in
 * Mathematica sees what math42 answered; reading one back takes the
 * Input cells and passes over the rest, which is what keeps a round
 * trip unchanged. */
static char *
notebook_write (const char *inputs, const char *const *outputs)
{
  g_auto (GStrv) lines = g_strsplit (inputs, "\n", -1);
  GString *out = g_string_new (NULL);
  gboolean first = TRUE;
  guint n = 0;

  g_string_append (out, "(* Content-type: application/vnd.wolfram.mathematica *)\n");
  g_string_append (out, "(* written by math42 *)\n\n");
  g_string_append (out, "Notebook[{\n");
  for (guint i = 0; lines[i] != NULL; i++)
    {
      const char *line = lines[i];
      const char *result = outputs != NULL && outputs[i] != NULL ? outputs[i] : NULL;

      if (line[0] == '\0')
        continue;
      n++;
      if (!first)
        g_string_append (out, ",\n");
      first = FALSE;
      g_string_append (out, "Cell[BoxData[");
      append_quoted (out, line);
      g_string_append_printf (out, "], \"Input\", CellLabel->\"In[%u]:=\"]", n);
      if (result != NULL && *result != '\0')
        {
          g_string_append (out, ",\n");
          g_string_append (out, "Cell[BoxData[");
          append_quoted (out, result);
          g_string_append_printf (out, "], \"Output\", CellLabel->\"Out[%u]=\"]", n);
        }
    }
  g_string_append (out, "\n}]\n");
  return g_string_free (out, FALSE);
}

/* --- what the rest of the program asks for ------------------------------- */

char *
m42_format_read (const char *contents, M42Format format)
{
  if (contents == NULL)
    return g_strdup ("");
  /* A Wolfram script begins with a line telling the shell what to run
   * it with.  It is not something to evaluate, and math42 used to try:
   * #!/usr/bin/env wolframscript came out as an input of its own. */
  if (g_str_has_prefix (contents, "#!"))
    {
      const char *rest = strchr (contents, '\n');

      contents = rest != NULL ? rest + 1 : "";
    }
  switch (format)
    {
    case M42_FORMAT_MATLAB:   return matlab_read (contents);
    case M42_FORMAT_NOTEBOOK: return notebook_read (contents);
    default:                  return join_broken_lines (contents);
    }
}

char *
m42_format_write (const char *inputs, const char *const *outputs, M42Format format)
{
  if (inputs == NULL)
    return g_strdup ("");
  switch (format)
    {
    case M42_FORMAT_MATLAB:   return matlab_write (inputs);
    case M42_FORMAT_WOLFRAM:  return wolfram_write (inputs);
    case M42_FORMAT_NOTEBOOK: return notebook_write (inputs, outputs);
    default:                  return g_strdup (inputs);
    }
}
