// SPDX-FileCopyrightText: 2024-2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <gtk/gtk.h>

#include "alsa.h"
#include "device-update-firmware.h"
#include "firmware-store.h"
#include "gtkhelper.h"
#include "iface-update.h"
#include "online-check.h"
#include "scarlett2-firmware.h"
#include "scarlett4-firmware.h"

#define SCARLETT2_FIRMWARE_URL \
  "https://github.com/geoffreybennett/scarlett2-firmware"
#define SCARLETT4_FIRMWARE_URL \
  "https://github.com/geoffreybennett/scarlett4-firmware"

struct auto_start_data {
  struct alsa_card *card;
  GtkWidget        *label;
};

// Auto-start update callback (called via g_idle_add)
static gboolean auto_start_update(gpointer user_data) {
  struct auto_start_data *data = user_data;

  create_update_firmware_window(NULL, data->card, data->label);

  g_free(data);
  return G_SOURCE_REMOVE;
}

// Button callback wrapper (passes NULL for parent_label)
static void update_button_clicked(GtkWidget *w, struct alsa_card *card) {
  create_update_firmware_window(w, card, NULL);
}

/* Whether the firmware to install is held locally. Only a local file can
 * be installed: the update window installs from the firmware store and
 * does not download.
 */
static gboolean have_firmware(struct alsa_card *card) {
  return card_best_local_firmware(card) != NULL;
}

/* Where a user is sent to fetch the firmware by hand, which is the way
 * forward whenever the app has not got it.
 *
 * reason names a failed attempt, so that "obtain it yourself" does not
 * read as the app having made none; NULL when there was no attempt to
 * report. Free with g_free().
 */
static char *obtain_markup(struct alsa_card *card, const char *reason) {
  const char *url = card->driver_type == DRIVER_TYPE_SOCKET
    ? SCARLETT4_FIRMWARE_URL
    : SCARLETT2_FIRMWARE_URL;

  char *failed = reason
    ? g_markup_printf_escaped("It could not be downloaded:\n%s.\n", reason)
    : g_strdup("");
  char *escaped_url = g_markup_escape_text(url, -1);

  /* Assembled rather than printf-escaped as a whole: failed is markup
   * already, and escaping it again would show its own escapes.
   */
  char *markup = g_strdup_printf(
    "A firmware update is required for this device in order to\n"
    "access all of its features. %sPlease obtain the firmware from\n"
    "<a class=\"linktext\" href=\"%s\">%s</a>,\n"
    "and restart this application.",
    failed, escaped_url, escaped_url
  );

  g_free(failed);
  g_free(escaped_url);

  return markup;
}

#ifdef HAVE_ONLINE_UPDATE

/* Every update interface on screen, so that firmware arriving after one
 * was built can rewrite it.
 *
 * This screen has no working device behind it, so it is the one a user
 * cannot navigate away from: leaving it saying "obtain the firmware and
 * restart this application" while the file is downloading would make a
 * restart the only visible way forward.
 *
 * Each entry is removed by a weak ref, the interface living and dying
 * with the window holding it. The status callback cannot be
 * unregistered, so it must not hold a card pointer of its own.
 */
static GSList *ifaces;

static gboolean subscribed;

// What this screen is waiting for. Free with g_free().
static char *waiting_markup(struct alsa_card *card) {
  const struct card_download *download = online_check_get_download(card);

  /* A download in hand replaces the instruction to go and fetch the
   * firmware by hand, which is otherwise the only way forward offered.
   */
  if (download && download->state == DOWNLOAD_STATE_RUNNING)
    return g_markup_escape_text(
      "A firmware update is required for this device in order to\n"
      "access all of its features. Downloading the firmware...",
      -1
    );

  gboolean failed =
    download &&
    (download->state == DOWNLOAD_STATE_FAILED ||
     download->state == DOWNLOAD_STATE_BLOCKED);

  return obtain_markup(card, failed ? download->error : NULL);
}

/* Show the transfer, so that a user on a screen with no other way
 * forward can see that there is one in progress.
 */
static void refresh_progress(GtkWidget *progress, struct alsa_card *card) {
  const struct card_download *download = online_check_get_download(card);
  gboolean running =
    download && download->state == DOWNLOAD_STATE_RUNNING;

  gtk_widget_set_visible(progress, running);

  if (!running)
    return;

  // Before the first chunk, and if a server sends no length, pulse
  if (!download->total) {
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(progress));
    return;
  }

  gtk_progress_bar_set_fraction(
    GTK_PROGRESS_BAR(progress),
    (double)download->received / download->total
  );
}

#else // HAVE_ONLINE_UPDATE

/* Offline there is no download to report, so the message is the plain
 * "obtain it yourself" and there is never a bar to show.
 */
static char *waiting_markup(struct alsa_card *card) {
  return obtain_markup(card, NULL);
}

static void refresh_progress(GtkWidget *progress, struct alsa_card *card) {
  gtk_widget_set_visible(progress, FALSE);
}

#endif // HAVE_ONLINE_UPDATE

/* Swap between waiting for the firmware and offering to install it.
 *
 * A mid-upgrade interface is never registered for refreshing: it has
 * handed the card to the update window and shows what that reports.
 */
static void refresh_iface(GtkWidget *top) {
  struct alsa_card *card = g_object_get_data(G_OBJECT(top), "card");
  GtkWidget *title = g_object_get_data(G_OBJECT(top), "title");
  GtkWidget *waiting = g_object_get_data(G_OBJECT(top), "waiting");
  GtkWidget *ready = g_object_get_data(G_OBJECT(top), "ready");
  GtkWidget *button = g_object_get_data(G_OBJECT(top), "button");
  GtkWidget *progress = g_object_get_data(G_OBJECT(top), "progress");

  gboolean installable = have_firmware(card);

  gtk_label_set_text(
    GTK_LABEL(title),
    installable ? "Firmware Update Available" : "Firmware Update Required"
  );

  if (!installable) {
    char *markup = waiting_markup(card);

    gtk_label_set_markup(GTK_LABEL(waiting), markup);
    g_free(markup);
  }

  gtk_widget_set_visible(waiting, !installable);
  gtk_widget_set_visible(ready, installable);
  gtk_widget_set_visible(button, installable);

  refresh_progress(progress, card);
}

#ifdef HAVE_ONLINE_UPDATE

static void status_changed(struct alsa_card *card) {
  for (GSList *l = ifaces; l; l = l->next)
    refresh_iface(l->data);
}

static void iface_finalised(gpointer data, GObject *top) {
  ifaces = g_slist_remove(ifaces, top);
}

/* Track this interface and subscribe, so that firmware arriving rewrites
 * it in place rather than leaving it telling the user to restart the
 * application.
 */
static void watch_iface(GtkWidget *top) {
  if (!subscribed) {
    subscribed = TRUE;
    online_check_add_status_changed_callback(status_changed);
  }

  ifaces = g_slist_prepend(ifaces, top);
  g_object_weak_ref(G_OBJECT(top), iface_finalised, NULL);
}

#else // HAVE_ONLINE_UPDATE

// Offline the firmware cannot arrive while this screen is up
static void watch_iface(GtkWidget *top) {
}

#endif // HAVE_ONLINE_UPDATE

GtkWidget *create_iface_update_main(struct alsa_card *card, int mid_upgrade) {
  GtkWidget *top = gtk_frame_new(NULL);
  gtk_widget_add_css_class(top, "window-frame");

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 30);
  gtk_widget_add_css_class(content, "window-content");
  gtk_widget_add_css_class(content, "top-level-content");
  gtk_widget_add_css_class(content, "big-padding");
  gtk_frame_set_child(GTK_FRAME(top), content);

  /* Mid-upgrade hands the card straight to the update window and shows
   * whatever that reports through the label it is given, so it is built
   * complete and gets none of the swapping below.
   */
  if (mid_upgrade) {
    GtkWidget *w = gtk_label_new("Firmware Update In Progress");
    gtk_widget_add_css_class(w, "window-title");
    gtk_box_append(GTK_BOX(content), w);

    w = gtk_label_new(
      "Firmware update in progress...\n"
      "Please do not disconnect the device."
    );
    gtk_box_append(GTK_BOX(content), w);

    struct auto_start_data *data = g_new0(struct auto_start_data, 1);
    data->card = card;
    data->label = w;
    g_idle_add(auto_start_update, data);

    return top;
  }

  /* The title follows the firmware along with everything else, so it is
   * left empty here and set by the refresh below.
   */
  GtkWidget *title_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(title_label, "window-title");
  gtk_box_append(GTK_BOX(content), title_label);

  /* Both messages are built, and which one shows follows the firmware:
   * it can arrive while this screen is up, and this screen is the one
   * with no working device behind it to fall back on.
   */
  GtkWidget *waiting = gtk_label_new(NULL);
  gtk_label_set_justify(GTK_LABEL(waiting), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(content), waiting);

  GtkWidget *ready = gtk_label_new(
    "A firmware update is available for this device.\n"
    "This process may take a few minutes.\n"
    "Please do not disconnect the device during the update."
  );
  gtk_label_set_justify(GTK_LABEL(ready), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(content), ready);

  GtkWidget *progress = gtk_progress_bar_new();
  gtk_widget_add_css_class(progress, "update-progress");
  gtk_box_append(GTK_BOX(content), progress);

  GtkWidget *button = gtk_button_new_with_label("Update");
  g_signal_connect(
    GTK_BUTTON(button), "clicked", G_CALLBACK(update_button_clicked), card
  );
  gtk_box_append(GTK_BOX(content), button);

  g_object_set_data(G_OBJECT(top), "card", card);
  g_object_set_data(G_OBJECT(top), "title", title_label);
  g_object_set_data(G_OBJECT(top), "waiting", waiting);
  g_object_set_data(G_OBJECT(top), "ready", ready);
  g_object_set_data(G_OBJECT(top), "button", button);
  g_object_set_data(G_OBJECT(top), "progress", progress);

  refresh_iface(top);
  watch_iface(top);

  return top;
}
