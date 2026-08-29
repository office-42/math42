/* m42-application.c - see m42-application.h
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m42-application.h"
#include "m42-window.h"

#include <stdio.h>

struct _M42Application {
  GtkApplication parent_instance;

  char *screenshot;      /* --screenshot FILE: render the window and exit */
  char *activate;        /* --activate ACTION: fire a window action first */
  char *export_pdf;      /* --export-pdf FILE: write the notebook out and exit */
  char *convert;         /* --convert FILE: write it in that file's format and exit */
  int   width, height;   /* --size WxH: how big the window opens */
};

G_DEFINE_FINAL_TYPE (M42Application, m42_application, GTK_TYPE_APPLICATION)

static const struct {
  const char *action;
  const char *accels[3];
} ACCELS[] = {
  { "win.new",             { "<Control>n", NULL } },
  { "win.open",            { "<Control>o", NULL } },
  { "win.save",            { "<Control>s", NULL } },
  { "win.save-as",         { "<Control><Shift>s", NULL } },
  { "win.close",           { "<Control>w", NULL } },
  { "win.evaluate",        { "<Shift>Return", "<Control>Return", NULL } },
  { "win.clear-output",    { "<Control>l", NULL } },
  { "win.zoom-in",         { "<Control>plus", "<Control>equal", NULL } },
  { "win.zoom-out",        { "<Control>minus", NULL } },
  { "win.zoom-reset",      { "<Control>0", NULL } },
  { "win.reference",       { "F1", NULL } },
  { "app.quit",            { "<Control>q", NULL } },
};

static void
action_quit (GSimpleAction *action, GVariant *param, gpointer data)
{
  GList *windows = g_list_copy (gtk_application_get_windows (GTK_APPLICATION (data)));

  for (GList *l = windows; l != NULL; l = l->next)
    gtk_window_close (GTK_WINDOW (l->data));
  g_list_free (windows);
}

static const GActionEntry APP_ACTIONS[] = {
  { "quit", action_quit, NULL, NULL, NULL, { 0 } },
};

static void
load_css (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();
  GdkDisplay *display = gdk_display_get_default ();

  gtk_css_provider_load_from_resource (provider, "/net/office42/math42/style.css");
  if (display != NULL)
    gtk_style_context_add_provider_for_display (display,
                                                GTK_STYLE_PROVIDER (provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);
}

static void
m42_application_startup (GApplication *app)
{
  G_APPLICATION_CLASS (m42_application_parent_class)->startup (app);

  load_css ();
  for (guint i = 0; i < G_N_ELEMENTS (ACCELS); i++)
    gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                           ACCELS[i].action, ACCELS[i].accels);
}

/* --screenshot renders the window's widget tree through GSK into a PNG
 * and quits.  It goes through the render nodes rather than the screen,
 * so it works without a compositor and in CI; it is how a change to the
 * notebook can be looked at without a person at the keyboard. */
static gboolean
take_screenshot (gpointer data)
{
  M42Application *self = data;
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));
  GtkWidget *window;
  GdkPaintable *paintable;
  GtkSnapshot *snapshot;
  GskRenderNode *node;
  cairo_surface_t *surface;
  cairo_t *cr;
  int width, height;

  if (windows == NULL)
    {
      g_application_quit (G_APPLICATION (self));
      return G_SOURCE_REMOVE;
    }

  /* Every toplevel that is up, stacked one above the other: the window
   * itself, and whatever dialog --activate opened over it. */
  {
    GListModel *all = gtk_window_get_toplevels ();
    guint n = g_list_model_get_n_items (all);
    g_autoptr (GPtrArray) shots = g_ptr_array_new ();
    g_autoptr (GArray) sizes = g_array_new (FALSE, FALSE, sizeof (int) * 2);
    int y = 0;

    width = 1;
    height = 0;
    for (guint i = 0; i < n; i++)
      {
        g_autoptr (GtkWindow) top = g_list_model_get_item (all, i);
        int size[2];

        if (!gtk_widget_get_visible (GTK_WIDGET (top)))
          continue;
        window = GTK_WIDGET (top);
        size[0] = MAX (gtk_widget_get_width (window), 1);
        size[1] = MAX (gtk_widget_get_height (window), 1);

        paintable = gtk_widget_paintable_new (window);
        snapshot = gtk_snapshot_new ();
        gdk_paintable_snapshot (paintable, snapshot, size[0], size[1]);
        node = gtk_snapshot_free_to_node (snapshot);
        g_object_unref (paintable);

        g_ptr_array_add (shots, node);
        g_array_append_val (sizes, size);
        width = MAX (width, size[0]);
        height += size[1] + 8;
      }

    surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, MAX (height, 1));
    cr = cairo_create (surface);
    cairo_set_source_rgb (cr, 0.85, 0.85, 0.85);
    cairo_paint (cr);
    for (guint i = 0; i < shots->len; i++)
      {
        GskRenderNode *one = g_ptr_array_index (shots, i);
        int *size = &g_array_index (sizes, int, i * 2);

        cairo_save (cr);
        cairo_translate (cr, 0, y);
        cairo_set_source_rgb (cr, 1, 1, 1);
        cairo_rectangle (cr, 0, 0, size[0], size[1]);
        cairo_fill (cr);
        if (one != NULL)
          {
            gsk_render_node_draw (one, cr);
            gsk_render_node_unref (one);
          }
        cairo_restore (cr);
        y += size[1] + 8;
      }
    cairo_destroy (cr);
  }

  if (cairo_surface_write_to_png (surface, self->screenshot) != CAIRO_STATUS_SUCCESS)
    g_printerr ("math42: could not write %s\n", self->screenshot);
  cairo_surface_destroy (surface);

  g_application_quit (G_APPLICATION (self));
  return G_SOURCE_REMOVE;
}

/* --activate fires a window action before the picture is taken, which
 * is how a dialog gets one of its own. */
static gboolean
fire_activate (gpointer data)
{
  M42Application *self = data;
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));

  if (windows != NULL && self->activate != NULL)
    g_action_group_activate_action (G_ACTION_GROUP (windows->data), self->activate, NULL);
  return G_SOURCE_REMOVE;
}

/* --export-pdf writes the notebook out and quits, without a soul at
 * the keyboard. */
static gboolean
export_and_quit (gpointer data)
{
  M42Application *self = data;
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));
  g_autoptr (GError) error = NULL;

  if (windows != NULL &&
      !m42_window_export_pdf (M42_WINDOW (windows->data), self->export_pdf, &error))
    g_printerr ("math42: could not write %s: %s\n", self->export_pdf, error->message);
  g_application_quit (G_APPLICATION (self));
  return G_SOURCE_REMOVE;
}

/* --convert writes the notebook out in whatever format the name given
 * asks for, and quits.  It is how a MATLAB script becomes a notebook,
 * or a notebook a Mathematica one, without opening anything. */
static gboolean
convert_and_quit (gpointer data)
{
  M42Application *self = data;
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));
  g_autoptr (GError) error = NULL;

  if (windows != NULL &&
      !m42_window_save_as (M42_WINDOW (windows->data), self->convert, &error))
    g_printerr ("math42: could not write %s: %s\n", self->convert, error->message);
  g_application_quit (G_APPLICATION (self));
  return G_SOURCE_REMOVE;
}

static void
arm_options (M42Application *self)
{
  if (self->convert != NULL)
    g_timeout_add (900, convert_and_quit, self);
  if (self->activate != NULL)
    g_timeout_add (500, fire_activate, self);
  if (self->export_pdf != NULL)
    g_timeout_add (900, export_and_quit, self);
  if (self->screenshot != NULL)
    g_timeout_add (1200, take_screenshot, self);
}

static void
size_window (M42Application *self, GtkWidget *window)
{
  if (self->width > 0 && self->height > 0)
    gtk_window_set_default_size (GTK_WINDOW (window), self->width, self->height);
}

static void
m42_application_activate (GApplication *app)
{
  M42Application *self = M42_APPLICATION (app);
  GtkWidget *window = m42_window_new (GTK_APPLICATION (app));

  size_window (self, window);
  gtk_window_present (GTK_WINDOW (window));
  arm_options (self);
}

static void
m42_application_open (GApplication *app, GFile **files, int n_files,
                      const char *hint)
{
  M42Application *self = M42_APPLICATION (app);

  for (int i = 0; i < n_files; i++)
    {
      GtkWidget *window = m42_window_new (GTK_APPLICATION (app));

      size_window (self, window);
      m42_window_open_file (M42_WINDOW (window), files[i]);
      gtk_window_present (GTK_WINDOW (window));
    }
  arm_options (self);
}

static int
m42_application_handle_local_options (GApplication *app, GVariantDict *options)
{
  M42Application *self = M42_APPLICATION (app);
  const char *path = NULL;

  if (g_variant_dict_lookup (options, "screenshot", "^&ay", &path))
    {
      g_free (self->screenshot);
      self->screenshot = g_strdup (path);
      g_application_set_flags (app, g_application_get_flags (app) |
                                    G_APPLICATION_NON_UNIQUE);
    }

  if (g_variant_dict_lookup (options, "activate", "&s", &path))
    {
      g_free (self->activate);
      self->activate = g_strdup (path);
    }

  if (g_variant_dict_lookup (options, "convert", "^&ay", &path))
    {
      g_free (self->convert);
      self->convert = g_strdup (path);
      g_application_set_flags (app, g_application_get_flags (app) |
                                    G_APPLICATION_NON_UNIQUE);
    }

  if (g_variant_dict_lookup (options, "export-pdf", "^&ay", &path))
    {
      g_free (self->export_pdf);
      self->export_pdf = g_strdup (path);
      g_application_set_flags (app, g_application_get_flags (app) |
                                    G_APPLICATION_NON_UNIQUE);
    }

  if (g_variant_dict_lookup (options, "size", "&s", &path))
    {
      int w, h;
      if (sscanf (path, "%dx%d", &w, &h) == 2 && w > 100 && h > 100)
        {
          self->width = w;
          self->height = h;
        }
      else
        g_printerr ("math42: --size wants something like 900x1100\n");
    }
  return -1;
}

static void
m42_application_finalize (GObject *object)
{
  g_free (M42_APPLICATION (object)->screenshot);
  g_free (M42_APPLICATION (object)->activate);
  g_free (M42_APPLICATION (object)->export_pdf);
  G_OBJECT_CLASS (m42_application_parent_class)->finalize (object);
}

static void
m42_application_class_init (M42ApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = m42_application_finalize;

  app_class->startup = m42_application_startup;
  app_class->activate = m42_application_activate;
  app_class->open = m42_application_open;
  app_class->handle_local_options = m42_application_handle_local_options;
}

static void
m42_application_init (M42Application *self)
{
  g_action_map_add_action_entries (G_ACTION_MAP (self), APP_ACTIONS,
                                   G_N_ELEMENTS (APP_ACTIONS), self);

  g_application_add_main_option (G_APPLICATION (self), "screenshot", 0,
                                 G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
                                 "Render the window to a PNG and exit", "FILE");
  g_application_add_main_option (G_APPLICATION (self), "activate", 0,
                                 G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
                                 "Fire a window action, such as reference, first", "ACTION");
  g_application_add_main_option (G_APPLICATION (self), "export-pdf", 0,
                                 G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
                                 "Write the notebook to a PDF and exit", "FILE");
  g_application_add_main_option (G_APPLICATION (self), "convert", 0,
                                 G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
                                 "Write the notebook out in that name's format and exit",
                                 "FILE");
  g_application_add_main_option (G_APPLICATION (self), "size", 0,
                                 G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
                                 "Open the window at this size, as 900x1100", "WxH");
}

M42Application *
m42_application_new (void)
{
  return g_object_new (M42_TYPE_APPLICATION,
                       "application-id", "net.office42.math42",
                       "flags", G_APPLICATION_HANDLES_OPEN,
                       NULL);
}
