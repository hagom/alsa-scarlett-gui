// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HAVE_ONLINE_UPDATE

#include "window-consent.h"

#include "app-settings.h"
#include "gtkhelper.h"
#include "usage-disclosure.h"

#define PREFERENCES_NOTE "(change any time in View \342\206\222 Preferences)"

struct consent_data {
  GtkApplication  *app;
  GtkWidget       *window;
  gboolean         check_online_updates;
  gboolean         send_usage_info;
  consent_done_cb  done_cb;
  gpointer         user_data;
};

/* Write both flags, then mark the choice as made, and hand control to
 * the caller's continuation. Every way out of either window arrives
 * here, so the choice is recorded exactly once and the continuation runs
 * exactly once.
 *
 * The order matters: an interruption between the two leaves the dialog
 * to reappear next run rather than a recorded opt-in nobody agreed to.
 */
static void finish(struct consent_data *data) {
  app_settings_set_check_online_updates(data->check_online_updates);
  app_settings_set_send_usage_info(data->send_usage_info);
  app_settings_set_online_choice_made();

  consent_done_cb done_cb = data->done_cb;
  GtkApplication *app = data->app;
  gpointer user_data = data->user_data;

  gtk_window_destroy(GTK_WINDOW(data->window));

  done_cb(app, user_data);
}

static gboolean on_key_pressed(
  GtkEventControllerKey *controller,
  guint                  keyval,
  guint                  keycode,
  GdkModifierType        state,
  struct consent_data   *data
) {
  if (keyval == GDK_KEY_Escape) {
    gtk_window_close(GTK_WINDOW(data->window));
    return TRUE;
  }
  return FALSE;
}

/* The common shell of both questions: a titled, non-resizable
 * application window with a heading, Escape wired to close it, and the
 * content box for the caller to fill above the buttons it adds with
 * add_answer_buttons().
 */
static GtkWidget *make_question_window(
  struct consent_data  *data,
  const char           *title,
  const char           *heading_text
) {
  GtkWidget *window = gtk_window_new();
  data->window = window;
  gtk_window_set_title(GTK_WINDOW(window), title);
  gtk_window_set_application(GTK_WINDOW(window), data->app);
  gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
  gtk_widget_add_css_class(window, "window-frame");
  gtk_widget_add_css_class(window, "modal");

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
  gtk_window_set_child(GTK_WINDOW(window), content);
  gtk_widget_add_css_class(content, "window-content");
  gtk_widget_add_css_class(content, "top-level-content");
  gtk_widget_set_margin(content, 30);

  GtkWidget *heading = gtk_label_new(NULL);
  char *markup = g_strdup_printf(
    "<span size='large' weight='bold'>%s</span>", heading_text
  );
  gtk_label_set_markup(GTK_LABEL(heading), markup);
  g_free(markup);
  gtk_box_append(GTK_BOX(content), heading);

  GtkEventController *key_controller = gtk_event_controller_key_new();
  g_signal_connect(
    key_controller, "key-pressed", G_CALLBACK(on_key_pressed), data
  );
  gtk_widget_add_controller(window, key_controller);

  g_signal_connect_swapped(window, "destroy", G_CALLBACK(g_free), data);

  return content;
}

static void add_answer_buttons(
  GtkWidget *content,
  GCallback  on_no,
  GCallback  on_yes,
  gpointer   data
) {
  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
  gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(content), button_box);

  GtkWidget *no_button = gtk_button_new_with_label("No");
  g_signal_connect(no_button, "clicked", on_no, data);
  gtk_box_append(GTK_BOX(button_box), no_button);

  GtkWidget *yes_button = gtk_button_new_with_label("Yes");
  g_signal_connect(yes_button, "clicked", on_yes, data);
  gtk_box_append(GTK_BOX(button_box), yes_button);
}

static void on_usage_no(GtkWidget *button, struct consent_data *data) {
  finish(data);
}

static void on_usage_yes(GtkWidget *button, struct consent_data *data) {
  data->send_usage_info = TRUE;
  finish(data);
}

/* Closing the second window declines the census and keeps the yes to
 * update checks: send_usage_info is still false at this point, so
 * finishing records exactly that.
 *
 * finish() destroys the window, and returning TRUE is what stops GTK
 * from following that with a destroy of its own.
 */
static gboolean on_usage_close_request(
  GtkWindow           *window,
  struct consent_data *data
) {
  finish(data);
  return TRUE;
}

// Sits directly above the buttons, where it is read before answering
static void add_preferences_note(GtkWidget *content) {
  GtkWidget *note = gtk_label_new(PREFERENCES_NOTE);

  gtk_widget_add_css_class(note, "consent-aside");
  gtk_widget_set_halign(note, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(content), note);
}

static void show_usage_question(struct consent_data *data) {
  GtkWidget *content = make_question_window(
    data, "Usage Survey", "Join the usage survey?"
  );

  GtkWidget *message = gtk_label_new(
    "A quick request from Geoffrey Bennett, the author of this app\n"
    "(alsa-scarlett-gui) and the Linux driver for Focusrite interfaces:\n\n"
    "If you don't mind, the update check can also let me know which\n"
    "device, kernel, and distro you're using. It helps me know what to\n"
    "focus testing on, and it gives me numbers to show Focusrite how\n"
    "many people are using their interfaces on Linux."
  );
  gtk_label_set_justify(GTK_LABEL(message), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(content), message);

  add_usage_disclosure_expanders(content);

  add_preferences_note(content);

  add_answer_buttons(
    content, G_CALLBACK(on_usage_no), G_CALLBACK(on_usage_yes), data
  );

  g_signal_connect(
    data->window, "close-request", G_CALLBACK(on_usage_close_request), data
  );

  gtk_window_present(GTK_WINDOW(data->window));
}

static void on_updates_no(GtkWidget *button, struct consent_data *data) {
  finish(data);
}

/* Answering yes replaces this window with the census question, which
 * reuses the same data and so reaches finish() with both answers.
 */
static void on_updates_yes(GtkWidget *button, struct consent_data *data) {
  data->check_online_updates = TRUE;

  GtkWidget *old_window = data->window;

  /* Detach the data from the window being destroyed: the second window
   * takes over freeing it.
   */
  g_signal_handlers_disconnect_by_func(old_window, G_CALLBACK(g_free), data);

  show_usage_question(data);

  gtk_window_destroy(GTK_WINDOW(old_window));
}

/* Closing the first window declines both questions, which is what the
 * flags already say, and skips the second: a window that refuses to
 * close reads as a hang.
 */
static gboolean on_updates_close_request(
  GtkWindow           *window,
  struct consent_data *data
) {
  finish(data);
  return TRUE;
}

void show_consent(
  GtkApplication  *app,
  consent_done_cb  done_cb,
  gpointer         user_data
) {
  if (app_settings_get_online_choice_made()) {
    done_cb(app, user_data);
    return;
  }

  struct consent_data *data = g_new0(struct consent_data, 1);
  data->app = app;
  data->done_cb = done_cb;
  data->user_data = user_data;

  GtkWidget *content = make_question_window(
    data,
    "Online Update Checks",
    "Check online for firmware and software updates?"
  );

  GtkWidget *message = gtk_label_new(
    "New firmware for your device, and new versions of this\n"
    "application."
  );
  gtk_label_set_justify(GTK_LABEL(message), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(content), message);

  add_preferences_note(content);

  add_answer_buttons(
    content, G_CALLBACK(on_updates_no), G_CALLBACK(on_updates_yes), data
  );

  g_signal_connect(
    data->window, "close-request",
    G_CALLBACK(on_updates_close_request), data
  );

  gtk_window_present(GTK_WINDOW(data->window));
}

#endif // HAVE_ONLINE_UPDATE
