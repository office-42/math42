/* m42-eval.h - the evaluator and its session of variables
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

#include "m42-value.h"

G_BEGIN_DECLS

/* A session: the variables and functions the user has defined, the
 * last result (%), and the running count of In[n]/Out[n]. */
typedef struct _M42Session M42Session;

M42Session *m42_session_new (void);
void        m42_session_free (M42Session *s);

/* Parses and evaluates one line.  Never returns NULL: a parse or
 * evaluation failure comes back as an M42_VALUE_ERROR, and a line
 * ending in ';' as M42_VALUE_NULL. */
M42Value *m42_session_eval (M42Session *s, const char *src);

/* What Print has written since this was last called, or NULL.  The
 * caller frees it. */
char *m42_session_take_printed (M42Session *s);

/* Forgets every user-defined variable and function. */
void m42_session_clear (M42Session *s);

/* The number of the next In[n], starting at 1. */
int m42_session_next_line (M42Session *s);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (M42Session, m42_session_free)

G_END_DECLS
