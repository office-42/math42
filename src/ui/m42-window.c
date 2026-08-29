/* m42-window.c - see m42-window.h
 *
 * Copyright (C) 2026 The math42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m42-window.h"
#include "m42-format.h"
#include "m42-notebook.h"
#include "m42-eval.h"
#include "m42-help.h"

#include <string.h>

struct _M42Window {
  GtkApplicationWindow parent_instance;

  M42Session  *session;
  GtkWidget   *notebook;
  GtkWidget   *scroller;
  GtkWidget   *entry;
  GtkWidget   *status;
  GFile       *file;

  /* What has been typed, and where the up arrow has walked back to. */
  GPtrArray   *history;
  int          history_pos;
  char        *draft;
};

G_DEFINE_FINAL_TYPE (M42Window, m42_window, GTK_TYPE_APPLICATION_WINDOW)

static void
set_status (M42Window *self, const char *fmt, ...) G_GNUC_PRINTF (2, 3);

static void
set_status (M42Window *self, const char *fmt, ...)
{
  va_list ap;
  g_autofree char *text = NULL;

  va_start (ap, fmt);
  text = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  gtk_label_set_text (GTK_LABEL (self->status), text);
}

static void
update_title (M42Window *self)
{
  g_autofree char *name = self->file != NULL ? g_file_get_basename (self->file)
                                             : g_strdup ("Untitled");
  g_autofree char *title = g_strdup_printf ("%s - Math42", name);
  gtk_window_set_title (GTK_WINDOW (self), title);
}

static gboolean
scroll_to_end (gpointer data)
{
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (data));
  gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) -
                                 gtk_adjustment_get_page_size (adj));
  return G_SOURCE_REMOVE;
}

static void
evaluate_line (M42Window *self, const char *line)
{
  int n = m42_session_next_line (self->session);

  /* Nothing but a comment: it goes on the page as a line of text, with
   * no number and nothing worked out. */
  if (m42_source_is_blank (line))
    {
      m42_notebook_append (M42_NOTEBOOK (self->notebook), 0, line, NULL, NULL);
      g_idle_add (scroll_to_end, self->scroller);
      return;
    }

  g_autoptr (M42Value) result = m42_session_eval (self->session, line);
  g_autofree char *printed = m42_session_take_printed (self->session);

  m42_notebook_append (M42_NOTEBOOK (self->notebook), n, line, result, printed);
  if (result->kind == M42_VALUE_ERROR)
    set_status (self, "In[%d]: %s", n, result->u.error);
  else
    set_status (self, "Evaluated In[%d]", n);
  g_idle_add (scroll_to_end, self->scroller);
}

static void
action_evaluate (GSimpleAction *action, GVariant *param, gpointer data)
{
  M42Window *self = data;
  g_autofree char *text = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->entry)));

  g_strstrip (text);
  if (text[0] == '\0')
    return;
  evaluate_line (self, text);
  gtk_editable_set_text (GTK_EDITABLE (self->entry), "");
}

static void
zoom_by (M42Window *self, double factor, gboolean reset)
{
  M42Notebook *nb = M42_NOTEBOOK (self->notebook);
  double scale = reset ? 1.0 : m42_notebook_get_scale (nb) * factor;

  m42_notebook_set_scale (nb, scale);
  set_status (self, "Mathematics at %d%%", (int) (m42_notebook_get_scale (nb) * 100 + 0.5));
}

static void
action_zoom_in (GSimpleAction *action, GVariant *param, gpointer data)
{
  zoom_by (data, 1.15, FALSE);
}

static void
action_zoom_out (GSimpleAction *action, GVariant *param, gpointer data)
{
  zoom_by (data, 1 / 1.15, FALSE);
}

static void
action_zoom_reset (GSimpleAction *action, GVariant *param, gpointer data)
{
  zoom_by (data, 1, TRUE);
}

static void
action_clear_variables (GSimpleAction *action, GVariant *param, gpointer data)
{
  M42Window *self = data;
  m42_session_clear (self->session);
  set_status (self, "Variables cleared");
}

static void
action_clear_output (GSimpleAction *action, GVariant *param, gpointer data)
{
  M42Window *self = data;
  m42_notebook_clear (M42_NOTEBOOK (self->notebook));
  set_status (self, "Ready");
}

static void
action_new (GSimpleAction *action, GVariant *param, gpointer data)
{
  GtkWidget *window = m42_window_new (gtk_window_get_application (GTK_WINDOW (data)));
  gtk_window_present (GTK_WINDOW (window));
}

static void
action_close (GSimpleAction *action, GVariant *param, gpointer data)
{
  gtk_window_close (GTK_WINDOW (data));
}

static void
open_done (GObject *source, GAsyncResult *result, gpointer data)
{
  M42Window *self = data;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, NULL);

  if (file != NULL)
    m42_window_open_file (self, file);
}

/* The four kinds of file math42 reads and writes, offered in the
 * dialogs so that they can be found without knowing to type the
 * ending. */
static GListModel *
notebook_filters (void)
{
  static const struct { const char *name; const char *pattern; } KINDS[] = {
    { "math42 notebook (*.m42)",         "*.m42" },
    { "MATLAB script (*.m)",             "*.m" },
    { "Wolfram Language (*.wl, *.wls)",  "*.wl" },
    { "Mathematica notebook (*.nb)",     "*.nb" },
  };
  GListStore *all = g_list_store_new (GTK_TYPE_FILE_FILTER);
  GtkFileFilter *every = gtk_file_filter_new ();

  gtk_file_filter_set_name (every, "Everything math42 reads");
  for (guint i = 0; i < G_N_ELEMENTS (KINDS); i++)
    {
      GtkFileFilter *one = gtk_file_filter_new ();

      gtk_file_filter_set_name (one, KINDS[i].name);
      gtk_file_filter_add_pattern (one, KINDS[i].pattern);
      gtk_file_filter_add_pattern (every, KINDS[i].pattern);
      if (i == 2)
        {
          gtk_file_filter_add_pattern (one, "*.wls");
          gtk_file_filter_add_pattern (every, "*.wls");
        }
      g_list_store_append (all, one);
      g_object_unref (one);
    }
  g_list_store_insert (all, 0, every);
  g_object_unref (every);
  return G_LIST_MODEL (all);
}

static void
action_open (GSimpleAction *action, GVariant *param, gpointer data)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  g_autoptr (GListModel) filters = notebook_filters ();

  gtk_file_dialog_set_title (dialog, "Open Notebook");
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_open (dialog, GTK_WINDOW (data), NULL, open_done, data);
  g_object_unref (dialog);
}

static void
save_to (M42Window *self, GFile *file)
{
  g_autofree char *inputs = m42_notebook_get_inputs (M42_NOTEBOOK (self->notebook));
  g_auto (GStrv) outputs = m42_notebook_get_outputs (M42_NOTEBOOK (self->notebook));
  g_autofree char *path = g_file_get_path (file);
  M42Format format = m42_format_for_path (path);
  g_autofree char *text = m42_format_write (inputs, (const char *const *) outputs,
                                            format);
  g_autoptr (GError) error = NULL;

  if (!g_file_replace_contents (file, text, strlen (text), NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, &error))
    {
      set_status (self, "Could not save: %s", error->message);
      return;
    }
  g_set_object (&self->file, file);
  update_title (self);
  set_status (self, format == M42_FORMAT_M42 ? "Saved"
                                             : "Saved as a %s", m42_format_name (format));
}

static void
save_done (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, NULL);

  if (file != NULL)
    save_to (M42_WINDOW (data), file);
}

static void
action_save_as (GSimpleAction *action, GVariant *param, gpointer data)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  g_autoptr (GListModel) filters = notebook_filters ();

  gtk_file_dialog_set_title (dialog, "Save Notebook");
  gtk_file_dialog_set_initial_name (dialog, "Untitled.m42");
  /* The ending chooses the format: .m writes a MATLAB script, .nb a
   * Mathematica notebook, .wl a Wolfram one. */
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_save (dialog, GTK_WINDOW (data), NULL, save_done, data);
  g_object_unref (dialog);
}

static void
action_save (GSimpleAction *action, GVariant *param, gpointer data)
{
  M42Window *self = data;

  if (self->file != NULL)
    save_to (self, self->file);
  else
    action_save_as (action, param, data);
}

/* What every printed page is headed with: the file's name, or a word
 * for a notebook that has not been saved anywhere yet. */
static char *
paper_title (M42Window *self)
{
  if (self->file != NULL)
    return g_file_get_basename (self->file);
  return g_strdup ("Untitled notebook");
}

static void
export_pdf_done (GObject *source, GAsyncResult *result, gpointer data)
{
  M42Window *self = data;
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, NULL);
  g_autoptr (GError) error = NULL;
  g_autofree char *path = NULL;

  if (file == NULL)
    return;
  path = g_file_get_path (file);
  if (path == NULL)
    {
      set_status (self, "That place cannot be written to directly");
      return;
    }
  {
    g_autofree char *title = paper_title (self);

    if (m42_notebook_export_pdf (M42_NOTEBOOK (self->notebook), path, title, &error))
      set_status (self, "Exported to %s", path);
    else
      set_status (self, "Could not export: %s", error->message);
  }
}

static void
action_print (GSimpleAction *action, GVariant *param, gpointer data)
{
  M42Window *self = data;
  g_autofree char *title = paper_title (self);
  g_autoptr (GError) error = NULL;

  if (!m42_notebook_print (M42_NOTEBOOK (self->notebook), title,
                           GTK_WINDOW (self), &error))
    set_status (self, "Could not print: %s",
                error != NULL ? error->message : "the printer said no");
}

static void
action_export_pdf (GSimpleAction *action, GVariant *param, gpointer data)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();

  gtk_file_dialog_set_title (dialog, "Export Notebook as PDF");
  gtk_file_dialog_set_initial_name (dialog, "notebook.pdf");
  gtk_file_dialog_save (dialog, GTK_WINDOW (data), NULL, export_pdf_done, data);
  g_object_unref (dialog);
}

/* The reference: every function math42 knows, in its sections, with a
 * box at the top that narrows the list as it is typed into. */
typedef struct {
  GtkWidget *label;
  GtkWidget *search;
} Reference;

static void
reference_fill (Reference *ref)
{
  const char *needle = gtk_editable_get_text (GTK_EDITABLE (ref->search));
  g_autofree char *folded = g_utf8_strdown (needle, -1);
  g_autoptr (GString) text = g_string_new (NULL);
  g_autoptr (GPtrArray) sections = g_ptr_array_new ();
  guint shown = 0;

  /* The sections in the order they first appear, each printed once
   * with everything of its kind under it.  The table is not sorted by
   * section -- a row is written next to the rows it belongs with,
   * which is not always the same thing -- and heading it every time
   * the section changed gave the reference "Lists" ten times over. */
  for (const M42Function *f = m42_functions (); f->name != NULL; f++)
    {
      gboolean already = FALSE;

      for (guint i = 0; i < sections->len; i++)
        if (g_strcmp0 (g_ptr_array_index (sections, i), f->section) == 0)
          already = TRUE;
      if (!already)
        g_ptr_array_add (sections, (gpointer) f->section);
    }

  for (guint i = 0; i < sections->len; i++)
    {
      const char *section = g_ptr_array_index (sections, i);
      gboolean headed = FALSE;

      for (const M42Function *f = m42_functions (); f->name != NULL; f++)
        {
          g_autofree char *joined = NULL;
          g_autofree char *hay = NULL;
          g_autofree char *usage = NULL;
          g_autofree char *summary = NULL;

          if (g_strcmp0 (f->section, section) != 0)
            continue;
          joined = g_strdup_printf ("%s %s %s %s", f->name,
                                    f->matlab != NULL ? f->matlab : "",
                                    f->usage, f->summary);
          hay = g_utf8_strdown (joined, -1);
          if (*folded != 0 && strstr (hay, folded) == NULL)
            continue;
          usage = g_markup_escape_text (f->usage, -1);
          summary = g_markup_escape_text (f->summary, -1);
          if (!headed)
            {
              headed = TRUE;
              g_string_append_printf (text, "%s<b>%s</b>\n", shown > 0 ? "\n" : "",
                                      section);
            }
          g_string_append_printf (text, "  <tt>%s</tt>", usage);
          if (f->matlab != NULL)
            g_string_append_printf (text, "   <i>%s</i>", f->matlab);
          g_string_append_printf (text, "\n      %s\n", summary);
          shown++;
        }
    }
  if (shown == 0)
    g_string_append (text, "Nothing here goes by that name.");

  gtk_label_set_markup (GTK_LABEL (ref->label), text->str);
}

static void
reference_changed (GtkEditable *editable, gpointer data)
{
  reference_fill (data);
}

static void
reference_closed (GtkWidget *window, gpointer data)
{
  g_free (data);
}

static void
action_reference (GSimpleAction *action, GVariant *param, gpointer data)
{
  Reference *ref = g_new0 (Reference, 1);
  GtkWidget *window = gtk_window_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *scroller = gtk_scrolled_window_new ();

  ref->search = gtk_search_entry_new ();
  ref->label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (ref->label), 0.0);
  gtk_label_set_selectable (GTK_LABEL (ref->label), TRUE);
  gtk_widget_set_margin_start (ref->label, 8);
  gtk_widget_set_margin_end (ref->label, 8);
  gtk_widget_set_margin_bottom (ref->label, 8);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), ref->label);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_widget_set_margin_start (box, 8);
  gtk_widget_set_margin_end (box, 8);
  gtk_widget_set_margin_top (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  gtk_box_append (GTK_BOX (box), ref->search);
  gtk_box_append (GTK_BOX (box), scroller);

  gtk_window_set_title (GTK_WINDOW (window), "Math42 Function Reference");
  gtk_window_set_default_size (GTK_WINDOW (window), 640, 660);
  gtk_window_set_transient_for (GTK_WINDOW (window), GTK_WINDOW (data));
  gtk_window_set_child (GTK_WINDOW (window), box);

  g_signal_connect (ref->search, "changed", G_CALLBACK (reference_changed), ref);
  g_signal_connect (window, "destroy", G_CALLBACK (reference_closed), ref);
  reference_fill (ref);
  gtk_window_present (GTK_WINDOW (window));
}

static void
action_about (GSimpleAction *action, GVariant *param, gpointer data)
{
  const char *authors[] = { "The math42 authors", NULL };

  gtk_show_about_dialog (GTK_WINDOW (data),
                         "program-name", "Math42",
                         "version", M42_VERSION,
                         "comments", "A mathematics notebook in the shape of Mathematica and MATLAB",
                         "website", "https://github.com/office-42/math42",
                         "license-type", GTK_LICENSE_GPL_3_0,
                         "authors", authors,
                         "logo-icon-name", "net.office42.math42",
                         NULL);
}

static const GActionEntry WIN_ACTIONS[] = {
  { "new",             action_new,             NULL, NULL, NULL, { 0 } },
  { "open",            action_open,            NULL, NULL, NULL, { 0 } },
  { "save",            action_save,            NULL, NULL, NULL, { 0 } },
  { "save-as",         action_save_as,         NULL, NULL, NULL, { 0 } },
  { "close",           action_close,           NULL, NULL, NULL, { 0 } },
  { "evaluate",        action_evaluate,        NULL, NULL, NULL, { 0 } },
  { "clear-variables", action_clear_variables, NULL, NULL, NULL, { 0 } },
  { "clear-output",    action_clear_output,    NULL, NULL, NULL, { 0 } },
  { "zoom-in",         action_zoom_in,         NULL, NULL, NULL, { 0 } },
  { "zoom-out",        action_zoom_out,        NULL, NULL, NULL, { 0 } },
  { "zoom-reset",      action_zoom_reset,      NULL, NULL, NULL, { 0 } },
  { "export-pdf",      action_export_pdf,      NULL, NULL, NULL, { 0 } },
  { "print",           action_print,           NULL, NULL, NULL, { 0 } },
  { "reference",       action_reference,       NULL, NULL, NULL, { 0 } },
  { "about",           action_about,           NULL, NULL, NULL, { 0 } },
};

/* Clicking a cell puts what was typed there back on the input line,
 * which is the quickest way to run a line again with one thing
 * changed. */
static void
on_notebook_clicked (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  M42Window *self = data;
  const char *input = m42_notebook_input_at (M42_NOTEBOOK (self->notebook), x, y);

  if (input == NULL)
    return;
  gtk_editable_set_text (GTK_EDITABLE (self->entry), input);
  gtk_editable_set_position (GTK_EDITABLE (self->entry), -1);
  gtk_widget_grab_focus (self->entry);
  set_status (self, "Ready to run again");
}

static void
on_entry_activate (GtkEntry *entry, gpointer data)
{
  g_action_group_activate_action (G_ACTION_GROUP (data), "evaluate", NULL);
}

/* The up arrow walks back through what has been typed and the down
 * arrow forward again, with whatever was half-written kept at the end
 * of the walk. */
static gboolean
on_entry_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
              GdkModifierType state, gpointer data)
{
  M42Window *self = data;
  const char *text;

  if (keyval != GDK_KEY_Up && keyval != GDK_KEY_Down)
    return GDK_EVENT_PROPAGATE;
  if (self->history->len == 0)
    return GDK_EVENT_PROPAGATE;

  if (self->history_pos == (int) self->history->len)
    {
      g_free (self->draft);
      self->draft = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->entry)));
    }

  if (keyval == GDK_KEY_Up)
    self->history_pos = MAX (self->history_pos - 1, 0);
  else
    self->history_pos = MIN (self->history_pos + 1, (int) self->history->len);

  text = self->history_pos == (int) self->history->len
    ? (self->draft != NULL ? self->draft : "")
    : g_ptr_array_index (self->history, self->history_pos);
  gtk_editable_set_text (GTK_EDITABLE (self->entry), text);
  gtk_editable_set_position (GTK_EDITABLE (self->entry), -1);
  return GDK_EVENT_STOP;
}

static void
m42_window_dispose (GObject *object)
{
  M42Window *self = M42_WINDOW (object);

  g_clear_pointer (&self->session, m42_session_free);
  g_clear_pointer (&self->history, g_ptr_array_unref);
  g_clear_pointer (&self->draft, g_free);
  g_clear_object (&self->file);
  G_OBJECT_CLASS (m42_window_parent_class)->dispose (object);
}

static void
m42_window_class_init (M42WindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = m42_window_dispose;
}

static void
m42_window_init (M42Window *self)
{
  GtkBuilder *builder;
  GMenuModel *menu;
  GtkWidget *box, *menubar, *prompt, *row;

  self->session = m42_session_new ();
  self->history = g_ptr_array_new_with_free_func (g_free);

  g_action_map_add_action_entries (G_ACTION_MAP (self), WIN_ACTIONS,
                                   G_N_ELEMENTS (WIN_ACTIONS), self);

  gtk_widget_add_css_class (GTK_WIDGET (self), "m42");
  gtk_window_set_default_size (GTK_WINDOW (self), 760, 560);
  gtk_window_set_icon_name (GTK_WINDOW (self), "net.office42.math42");

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  builder = gtk_builder_new_from_resource ("/net/office42/math42/menus.ui");
  menu = G_MENU_MODEL (gtk_builder_get_object (builder, "menubar"));
  menubar = gtk_popover_menu_bar_new_from_model (menu);
  gtk_box_append (GTK_BOX (box), menubar);
  g_object_unref (builder);

  self->notebook = m42_notebook_new ();
  self->scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroller), self->notebook);
  {
    GtkGesture *click = gtk_gesture_click_new ();
    g_signal_connect (click, "pressed", G_CALLBACK (on_notebook_clicked), self);
    gtk_widget_add_controller (self->notebook, GTK_EVENT_CONTROLLER (click));
  }
  gtk_widget_set_vexpand (self->scroller, TRUE);
  gtk_box_append (GTK_BOX (box), self->scroller);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start (row, 8);
  gtk_widget_set_margin_end (row, 8);
  gtk_widget_set_margin_top (row, 6);
  gtk_widget_set_margin_bottom (row, 6);
  prompt = gtk_label_new ("In:=");
  gtk_widget_add_css_class (prompt, "dim-label");
  self->entry = gtk_entry_new ();
  gtk_widget_add_css_class (self->entry, "m42-input");
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->entry), "Enter an expression");
  gtk_widget_set_hexpand (self->entry, TRUE);
  g_signal_connect (self->entry, "activate", G_CALLBACK (on_entry_activate), self);
  {
    GtkEventController *keys = gtk_event_controller_key_new ();
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_entry_key), self);
    gtk_widget_add_controller (self->entry, keys);
  }
  gtk_box_append (GTK_BOX (row), prompt);
  gtk_box_append (GTK_BOX (row), self->entry);
  gtk_box_append (GTK_BOX (box), row);

  self->status = gtk_label_new ("Ready");
  gtk_label_set_xalign (GTK_LABEL (self->status), 0.0);
  gtk_widget_add_css_class (self->status, "m42-statusbar");
  gtk_box_append (GTK_BOX (box), self->status);

  gtk_window_set_child (GTK_WINDOW (self), box);
  gtk_widget_grab_focus (self->entry);
  update_title (self);
}

GtkWidget *
m42_window_new (GtkApplication *app)
{
  return g_object_new (M42_TYPE_WINDOW, "application", app, NULL);
}

gboolean
m42_window_print_to_file (M42Window *self, const char *path, GError **error)
{
  g_autofree char *title = paper_title (self);

  return m42_notebook_print_to_file (M42_NOTEBOOK (self->notebook), title, path, error);
}

gboolean
m42_window_export_pdf (M42Window *self, const char *path, GError **error)
{
  {
    g_autofree char *title = paper_title (self);

    return m42_notebook_export_pdf (M42_NOTEBOOK (self->notebook), path, title, error);
  }
}

gboolean
m42_window_save_as (M42Window *self, const char *path, GError **error)
{
  g_autofree char *inputs = m42_notebook_get_inputs (M42_NOTEBOOK (self->notebook));
  g_auto (GStrv) outputs = m42_notebook_get_outputs (M42_NOTEBOOK (self->notebook));
  M42Format format = m42_format_for_path (path);
  g_autofree char *text = m42_format_write (inputs, (const char *const *) outputs,
                                            format);

  return g_file_set_contents (path, text, -1, error);
}

gboolean
m42_window_open_file (M42Window *self, GFile *file)
{
  g_autofree char *contents = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) lines = NULL;

  if (!g_file_load_contents (file, NULL, &contents, NULL, NULL, &error))
    {
      set_status (self, "Could not open: %s", error->message);
      return FALSE;
    }

  m42_notebook_clear (M42_NOTEBOOK (self->notebook));
  m42_session_clear (self->session);
  {
    /* A MATLAB script, a Wolfram one or a Mathematica notebook becomes
     * a list of inputs first; a .m42 file already is one. */
    g_autofree char *path = g_file_get_path (file);
    M42Format format = m42_format_for_path (path);
    g_autofree char *plain = m42_format_read (contents, format);

    lines = g_strsplit (plain, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++)
      {
        g_strstrip (lines[i]);
        if (lines[i][0] != '\0')
          evaluate_line (self, lines[i]);
      }
    if (format != M42_FORMAT_M42)
      set_status (self, "Read a %s", m42_format_name (format));
  }

  g_set_object (&self->file, file);
  update_title (self);
  return TRUE;
}
