/* m42-application.h - the GtkApplication for math42
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define M42_TYPE_APPLICATION (m42_application_get_type ())
G_DECLARE_FINAL_TYPE (M42Application, m42_application, M42, APPLICATION, GtkApplication)

M42Application *m42_application_new (void);

G_END_DECLS
