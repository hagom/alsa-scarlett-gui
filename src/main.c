// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "alsa.h"
#include "alsa-sim.h"
#include "app-settings.h"
#include "welcome.h"
#include "debug.h"
#include "firmware-dir.h"
#include "main.h"
#include "menu.h"
#include "window-consent.h"
#include "window-hardware.h"
#include "window-iface.h"
#include "window-preferences.h"

GtkApplication *app;

// CSS

static void load_css(void) {
  GtkCssProvider *css = gtk_css_provider_new();
  GdkDisplay *display = gdk_display_get_default();

  gtk_style_context_add_provider_for_display(
    display, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
  );
  gtk_css_provider_load_from_resource(
    css,
    "/vu/b4/alsa-scarlett-gui/alsa-scarlett-gui.css"
  );

  g_object_unref(css);
}

// gtk init

/* Files named on the command line, held until startup completes.
 * open_cb() runs after startup() returns, which with the consent dialog
 * up is before there is a lifecycle observer to present them.
 */
static GPtrArray *pending_files;
static gboolean startup_complete;

/* The rest of startup, deferred until the consent dialog has been
 * answered so that nothing of the app appears behind it. Registering
 * window-iface.c as the lifecycle observer is what allows windows at
 * all, so the cards found during startup are presented here.
 */
static void startup_continue(GtkApplication *app, gpointer user_data) {
  window_iface_init();

  GArray *cards = alsa_get_cards();

  for (guint i = 0; i < cards->len; i++) {
    struct alsa_card *card = g_array_index(cards, struct alsa_card *, i);

    if (card)
      alsa_card_ready(card);
  }

  create_no_card_window();
  create_hardware_window(app);
#ifdef HAVE_ONLINE_UPDATE
  create_preferences_window(app);
  if (app_settings_get_preferences_visible())
    g_action_group_activate_action(
      G_ACTION_GROUP(app), "preferences", NULL
    );
#endif

  startup_complete = TRUE;

  if (pending_files) {
    for (guint i = 0; i < pending_files->len; i++)
      create_sim_from_file(NULL, g_ptr_array_index(pending_files, i));
    g_ptr_array_free(pending_files, TRUE);
    pending_files = NULL;
  }

  show_welcome(app);
}

static void startup(GtkApplication *app, gpointer user_data) {
  debug_init();

  gtk_application_set_menubar(app, G_MENU_MODEL(create_app_menu(app)));

  load_css();

  /* Find the attached devices before asking, so that the consent dialog
   * can show the request line for this machine. Presentation waits:
   * without a lifecycle observer registered, discovering a card creates
   * no window.
   */
  firmware_enum_all();
  alsa_init();

#ifdef HAVE_ONLINE_UPDATE
  show_consent(app, startup_continue, NULL);
#else
  // Nothing to consent to without network support, so nothing is asked
  startup_continue(app, NULL);
#endif
}

// not called when any files are opened from the command-line so we do
// everything in startup() and startup_continue(), but GTK wants this
// signal handled regardless
static void activate(GtkApplication *app, gpointer user_data) {
}

static void open_cb(
  GtkApplication  *app,
  GFile          **files,
  gint             n_files,
  const gchar     *hint
) {
  for (int i = 0; i < n_files; i++) {
    char *fn = g_file_get_path(files[i]);

    if (startup_complete) {
      create_sim_from_file(NULL, fn);
      g_free(fn);
      continue;
    }

    if (!pending_files)
      pending_files = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(pending_files, fn);
  }
}

int main(int argc, char **argv) {
  app = gtk_application_new(
    "vu.b4.alsa-scarlett-gui", G_APPLICATION_HANDLES_OPEN
  );
  g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  g_signal_connect(app, "open", G_CALLBACK(open_cb), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  return status;
}
