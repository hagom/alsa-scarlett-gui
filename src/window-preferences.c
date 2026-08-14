// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HAVE_ONLINE_UPDATE

#include "app-settings.h"
#include "setting-row.h"
#include "usage-disclosure.h"
#include "window-preferences.h"

GtkWidget *window_preferences;

/* The survey is sent with the update check, so there is no survey
 * without it. Each switch moves the other to keep that true, so that
 * neither of them can be clicked and do nothing.
 */
struct online_switches {
  GtkWidget *updates;
  GtkWidget *usage;
};

static void on_check_online_updates_changed(
  GObject    *sw,
  GParamSpec *pspec,
  gpointer    data
) {
  struct online_switches *sws = data;
  gboolean active = gtk_switch_get_active(GTK_SWITCH(sw));

  app_settings_set_check_online_updates(active);

  // Turning update checks off clears the survey flag, so the switch
  // follows it
  gtk_switch_set_active(
    GTK_SWITCH(sws->usage), app_settings_get_send_usage_info()
  );
}

static void on_send_usage_info_changed(
  GObject    *sw,
  GParamSpec *pspec,
  gpointer    data
) {
  struct online_switches *sws = data;
  gboolean active = gtk_switch_get_active(GTK_SWITCH(sw));

  app_settings_set_send_usage_info(active);

  // Joining the survey turns on the check it is sent with, so the
  // switch follows it
  gtk_switch_set_active(
    GTK_SWITCH(sws->updates), app_settings_get_check_online_updates()
  );
}

static GtkWidget *make_online_settings(void) {
  GtkWidget *grid = make_settings_grid();
  int grid_y = 0;

  struct online_switches *sws = g_malloc0(sizeof(struct online_switches));

  sws->usage = gtk_switch_new();
  gtk_switch_set_active(
    GTK_SWITCH(sws->usage), app_settings_get_send_usage_info()
  );

  sws->updates = gtk_switch_new();
  gtk_switch_set_active(
    GTK_SWITCH(sws->updates), app_settings_get_check_online_updates()
  );

  g_signal_connect(
    sws->updates, "notify::active",
    G_CALLBACK(on_check_online_updates_changed), sws
  );
  add_setting_row(
    grid, &grid_y, "Check for Updates", sws->updates,
    "Automatically check once a day whether newer firmware for your "
    "interface, or a newer version of this app, has been released."
  );

  g_signal_connect(
    sws->usage, "notify::active",
    G_CALLBACK(on_send_usage_info_changed), sws
  );
  GtkWidget *usage_desc = add_setting_row(
    grid, &grid_y, "Join the Usage Survey", sws->usage,
    "Extend the update check to tell the author which device, "
    "kernel, and distro you're using. It helps him know what to focus "
    "testing on, and gives him numbers to show Focusrite how many "
    "people are using their interfaces on Linux."
  );

  g_object_weak_ref(G_OBJECT(grid), (GWeakNotify)g_free, sws);

  /* The same disclosure the consent dialog showed when it asked, so the
   * answer can be reconsidered against the same facts.
   */
  add_usage_disclosure_expanders(usage_desc);

  return grid;
}

static gboolean window_close_request(GtkWindow *w, gpointer data) {
  GtkApplication *app = data;

  g_action_group_activate_action(
    G_ACTION_GROUP(app), "preferences", NULL
  );
  return true;
}

static gboolean on_key_press(
  GtkEventControllerKey *controller,
  guint                  keyval,
  guint                  keycode,
  GdkModifierType        state,
  gpointer               user_data
) {
  GtkApplication *app = user_data;

  if (keyval == GDK_KEY_Escape) {
    g_action_group_activate_action(
      G_ACTION_GROUP(app), "preferences", NULL
    );
    return 1;
  }

  if (state & GDK_CONTROL_MASK) {
    const char *action = NULL;

    switch (keyval) {
      case GDK_KEY_q: action = "quit";        break;
      case GDK_KEY_h: action = "hardware";    break;
      case GDK_KEY_w:
      case GDK_KEY_p: action = "preferences"; break;
    }

    if (action) {
      g_action_group_activate_action(G_ACTION_GROUP(app), action, NULL);
      return 1;
    }
  }

  return 0;
}

/* These settings are app-wide, so this is one window shared by every
 * card rather than a per-card subwindow, and it is reachable with no
 * card attached.
 */
void create_preferences_window(GtkApplication *app) {
  window_preferences = gtk_window_new();
  gtk_window_set_resizable(GTK_WINDOW(window_preferences), FALSE);
  gtk_window_set_title(
    GTK_WINDOW(window_preferences),
    "ALSA Scarlett Control Panel - Preferences"
  );
  g_signal_connect(
    window_preferences, "close_request",
    G_CALLBACK(window_close_request), app
  );

  GtkEventController *key_controller = gtk_event_controller_key_new();
  gtk_widget_add_controller(window_preferences, key_controller);
  g_signal_connect(
    key_controller, "key-pressed", G_CALLBACK(on_key_press), app
  );

  GtkWidget *top = gtk_frame_new(NULL);
  gtk_widget_add_css_class(top, "window-frame");
  gtk_window_set_child(GTK_WINDOW(window_preferences), top);

  GtkWidget *content = make_online_settings();
  gtk_widget_add_css_class(content, "window-content");
  gtk_widget_add_css_class(content, "top-level-content");
  gtk_frame_set_child(GTK_FRAME(top), content);
}

#endif // HAVE_ONLINE_UPDATE
