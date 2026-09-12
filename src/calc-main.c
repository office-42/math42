/* calc-main.c - math42 from a terminal: the engine without the window
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reads one expression per line from standard input and prints
 * "Out[n] = value" for each.  Blank lines are skipped.  With a terminal
 * on stdin it prompts as Mathematica does; with a pipe it stays quiet,
 * so its output can be checked by a script.
 *
 * Given a file to run instead, it reads that -- a .m42 notebook, a
 * MATLAB .m script, a Wolfram .wl one or a Mathematica .nb notebook,
 * whichever the name says.
 */

#include "m42-eval.h"
#include "m42-format.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#ifdef G_OS_UNIX
#include <unistd.h>
#define INTERACTIVE() isatty (0)
#else
#include <io.h>
#define INTERACTIVE() _isatty (0)
#endif

/* One line worked out and its answer printed, as both ways in do it. */
static void
run_line (M42Session *session, const char *line, int n)
{
  g_autoptr (M42Value) result = m42_session_eval (session, line);
  g_autofree char *text = NULL;
  g_autofree char *printed = m42_session_take_printed (session);

  /* Whatever Print wrote comes first, as it does in a notebook. */
  if (printed != NULL)
    fputs (printed, stdout);
  if (result->kind == M42_VALUE_NULL)
    return;
  text = m42_value_to_string (result);
  printf ("Out[%d]= %s\n", n, text);
  fflush (stdout);
}

/* A line from stdin however long it is, or NULL at the end.  A fixed
 * buffer cut a long list into two lines, each a syntax error. */
static char *
read_line (FILE *in)
{
  GString *line = g_string_new (NULL);
  char chunk[4096];

  while (fgets (chunk, sizeof chunk, in) != NULL)
    {
      g_string_append (line, chunk);
      if (line->str[line->len - 1] == '\n')
        break;
    }
  if (line->len == 0)
    {
      g_string_free (line, TRUE);
      return NULL;
    }
  return g_string_free (line, FALSE);
}

int
main (int argc, char *argv[])
{
  g_autoptr (M42Session) session = m42_session_new ();
  gboolean interactive = INTERACTIVE ();

  if (argc > 1 && (strcmp (argv[1], "--version") == 0 || strcmp (argv[1], "-v") == 0))
    {
      printf ("math42-calc %s\n", M42_VERSION);
      return 0;
    }

  /* A file to run, in whatever it was written in. */
  if (argc > 1 && argv[1][0] != '-')
    {
      g_autofree char *contents = NULL;
      g_autoptr (GError) error = NULL;
      g_autofree char *plain = NULL;
      g_auto (GStrv) lines = NULL;

      if (!g_file_get_contents (argv[1], &contents, NULL, &error))
        {
          g_printerr ("math42-calc: %s\n", error->message);
          return 1;
        }
      plain = m42_format_read (contents, m42_format_for_path (argv[1]));
      lines = g_strsplit (plain, "\n", -1);
      for (guint i = 0; lines[i] != NULL; i++)
        {
          g_strstrip (lines[i]);
          if (lines[i][0] != '\0')
            run_line (session, lines[i], m42_session_next_line (session));
        }
      return 0;
    }

  if (interactive)
    printf ("Math42 %s -- type an expression, Ctrl-D to leave\n", M42_VERSION);

  for (;;)
    {
      int n = m42_session_next_line (session);
      g_autofree char *line = NULL;

      if (interactive)
        {
          printf ("In[%d]:= ", n);
          fflush (stdout);
        }
      line = read_line (stdin);
      if (line == NULL)
        break;
      g_strstrip (line);
      if (line[0] == '\0')
        continue;

      run_line (session, line, n);
    }

  if (interactive)
    printf ("\n");
  return 0;
}
