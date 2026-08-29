/* m42-format.h - reading and writing the files other programs write
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  M42_FORMAT_M42,       /* .m42  one input to a line, as math42 writes it */
  M42_FORMAT_MATLAB,    /* .m    a MATLAB script */
  M42_FORMAT_WOLFRAM,   /* .wl, .wls, .nb-less Wolfram Language */
  M42_FORMAT_NOTEBOOK,  /* .nb   a Mathematica notebook */
} M42Format;

/* Which of them a file name says it is; .m42 for anything unfamiliar. */
M42Format m42_format_for_path (const char *path);

/* What to call the format, for a message or a file filter. */
const char *m42_format_name (M42Format format);

/* The inputs a file holds, one to a line, whatever it was written in.
 * An expression spread over several lines is put back together, so
 * that the notebook sees one input where the file had four. */
char *m42_format_read (const char *contents, M42Format format);

/* Those inputs written out again in that format.  inputs is one to a
 * line, as m42_notebook_get_inputs gives them. */
/* The notebook as a file of that kind.  outputs, when it is given, is
 * one line per input -- what the answer was -- and is written down by
 * the formats that have a place for it; NULL asks for the inputs
 * alone.  Reading such a file back gives the inputs again. */
char *m42_format_write (const char *inputs, const char *const *outputs,
                        M42Format format);

G_END_DECLS
