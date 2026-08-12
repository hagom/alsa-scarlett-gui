// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "update-notice.h"

#ifdef HAVE_ONLINE_UPDATE

#include "device-update-firmware.h"
#include "online-check.h"

#define PROJECT_URL "https://github.com/geoffreybennett/alsa-scarlett-gui"

/* Every notice on screen, so that a fetch completing after the windows
 * were built can rewrite them all.
 *
 * Each notice is removed by a weak ref taken when it is added. A notice
 * lives and dies with the window holding it, which closing a card
 * destroys even when the card is immediately re-readied.
 */
static GSList *notices;

static gboolean subscribed;

/* The firmware finding for this card, naming the available and running
 * versions, or NULL when there is nothing to report. Free with g_free().
 */
static char *firmware_markup(struct alsa_card *card) {
  const struct card_update_status *status = online_check_get_status(card);

  if (!status || !status->available)
    return NULL;

  const struct card_download *download = online_check_get_download(card);

  if (download && download->state == DOWNLOAD_STATE_BLOCKED)
    return g_markup_printf_escaped(
      "Firmware %s is available for this device (running %s), "
      "but cannot be installed: %s.",
      status->offered_version, status->installed_version, download->error
    );

  return g_markup_printf_escaped(
    "Firmware %s is available for this device (running %s).",
    status->offered_version, status->installed_version
  );
}

/* The app-version finding, carrying the project URL because the app
 * cannot install itself, or NULL. Free with g_free().
 */
static char *client_markup(void) {
  const char *client = online_check_get_client_update();

  if (!client)
    return NULL;

  /* The URL on its own line: alongside the text it makes the notice
   * wider than the interface below it.
   */
  return g_markup_printf_escaped(
    "alsa-scarlett-gui %s is available:\n"
    "<a class=\"linktext\" href=\"%s\">%s</a>",
    client, PROJECT_URL, PROJECT_URL
  );
}

/* Whether the available firmware can be installed from here, which
 * needs it to be the local file rather than one only the index lists:
 * the update window installs from the firmware store and does not
 * download.
 */
static gboolean can_install_now(struct alsa_card *card) {
  const struct card_update_status *status = online_check_get_status(card);

  return status && status->available && status->have_local_file;
}

/* Whether to offer the button. Firmware that cannot be installed on this
 * machine gets none: the row already says why, and a button that only
 * repeated the reason would be a button that does nothing.
 */
static gboolean update_offered(struct alsa_card *card) {
  const struct card_update_status *status = online_check_get_status(card);

  if (!status || !status->available)
    return FALSE;

  const struct card_download *download = online_check_get_download(card);

  return !download || download->state != DOWNLOAD_STATE_BLOCKED;
}

static void update_clicked(GtkWidget *w, struct alsa_card *card) {
  if (can_install_now(card)) {
    create_update_firmware_window(w, card, NULL);
    return;
  }

  /* The file is not here yet, so the click asks for the transfer: either
   * the first attempt, or a retry of one that failed. A running download
   * has the bar in this button's place, so there is no click to handle.
   */
  online_check_start_download(card);
}

/* One finding: a label, and room after it for the action that applies
 * to that finding alone.
 */
static GtkWidget *create_row(void) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

  gtk_widget_set_halign(row, GTK_ALIGN_CENTER);

  GtkWidget *label = gtk_label_new(NULL);

  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);

  gtk_box_append(GTK_BOX(row), label);
  g_object_set_data(G_OBJECT(row), "label", label);

  return row;
}

/* Set a row's text, hiding the whole row when it has nothing to say.
 * Takes ownership of markup. Returns whether the row is showing.
 */
static gboolean refresh_row(GtkWidget *row, char *markup) {
  GtkWidget *label = g_object_get_data(G_OBJECT(row), "label");

  gtk_widget_set_visible(row, markup != NULL);

  if (!markup)
    return FALSE;

  gtk_label_set_markup(GTK_LABEL(label), markup);
  g_free(markup);

  return TRUE;
}

/* Show the download in the firmware row while one is running, so that a
 * user who clicks Update before the file has arrived can see what it is
 * waiting for rather than an unresponsive button.
 */
// Returns whether the download is running, and so the bar is showing
static gboolean refresh_progress(GtkWidget *progress, struct alsa_card *card) {
  const struct card_download *download = online_check_get_download(card);
  gboolean running =
    download && download->state == DOWNLOAD_STATE_RUNNING;

  gtk_widget_set_visible(progress, running);

  if (!running)
    return FALSE;

  /* Before the first chunk, and if a server sends no length, there is no
   * fraction to show; pulse rather than sit at zero.
   */
  if (!download->total) {
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(progress));
    return TRUE;
  }

  gtk_progress_bar_set_fraction(
    GTK_PROGRESS_BAR(progress),
    (double)download->received / download->total
  );

  return TRUE;
}

/* Update both rows, hiding the notice when neither has anything to
 * report.
 */
static void refresh_notice(GtkWidget *notice) {
  struct alsa_card *card = g_object_get_data(G_OBJECT(notice), "card");
  GtkWidget *firmware_row = g_object_get_data(G_OBJECT(notice), "firmware-row");
  GtkWidget *client_row = g_object_get_data(G_OBJECT(notice), "client-row");
  GtkWidget *button = g_object_get_data(G_OBJECT(notice), "button");
  GtkWidget *progress = g_object_get_data(G_OBJECT(notice), "progress");

  gboolean any = refresh_row(firmware_row, firmware_markup(card));

  /* The bar takes the button's place while the download runs, rather
   * than sitting beside it: the button does nothing until the file has
   * arrived, and one that cannot be clicked should not be there to
   * click. Both belong to the firmware row, so both go when it goes.
   */
  gboolean downloading = refresh_progress(progress, card);

  gtk_widget_set_visible(button, !downloading && update_offered(card));

  if (refresh_row(client_row, client_markup()))
    any = TRUE;

  gtk_widget_set_visible(notice, any);
}

/* Refresh every notice, since each shows the app version alongside its
 * own card's firmware.
 */
static void status_changed(struct alsa_card *card) {
  for (GSList *l = notices; l; l = l->next)
    refresh_notice(l->data);
}

static void notice_finalised(gpointer data, GObject *notice) {
  notices = g_slist_remove(notices, notice);
}

GtkWidget *create_update_notice(struct alsa_card *card) {
  if (!subscribed) {
    subscribed = TRUE;
    online_check_add_status_changed_callback(status_changed);
  }

  GtkWidget *notice = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  gtk_widget_add_css_class(notice, "update-notice");

  // Sized to its contents, so the notice reads as an aside
  gtk_widget_set_halign(notice, GTK_ALIGN_CENTER);

  /* One row per finding, each carrying its own action, so that the
   * firmware row's button cannot look as though it applies to the app
   * version as well.
   */
  GtkWidget *firmware_row = create_row();
  GtkWidget *client_row = create_row();

  gtk_box_append(GTK_BOX(notice), firmware_row);
  gtk_box_append(GTK_BOX(notice), client_row);

  /* Present only while the available firmware is held locally; the app
   * version has no button, since the app cannot install itself, and its
   * row carries a link instead.
   */
  GtkWidget *button = gtk_button_new_with_label("Update");
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  g_signal_connect(button, "clicked", G_CALLBACK(update_clicked), card);
  gtk_box_append(GTK_BOX(firmware_row), button);

  /* Occupies the button's place while the download runs, so the CSS
   * gives it the button's width: the row is then the same size either
   * way, and neither sets the window's width.
   */
  GtkWidget *progress = gtk_progress_bar_new();
  gtk_widget_set_valign(progress, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(progress, "update-progress");
  gtk_box_append(GTK_BOX(firmware_row), progress);

  g_object_set_data(G_OBJECT(notice), "card", card);
  g_object_set_data(G_OBJECT(notice), "firmware-row", firmware_row);
  g_object_set_data(G_OBJECT(notice), "client-row", client_row);
  g_object_set_data(G_OBJECT(notice), "button", button);
  g_object_set_data(G_OBJECT(notice), "progress", progress);

  notices = g_slist_prepend(notices, notice);
  g_object_weak_ref(G_OBJECT(notice), notice_finalised, NULL);

  refresh_notice(notice);

  return notice;
}

#else // HAVE_ONLINE_UPDATE

GtkWidget *create_update_notice(struct alsa_card *card) {
  return NULL;
}

#endif // HAVE_ONLINE_UPDATE
