// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "update-notice.h"

#ifdef HAVE_ONLINE_UPDATE

#include "online-check.h"

#define PROJECT_URL "https://github.com/geoffreybennett/alsa-scarlett-gui"

/* The label of every notice on screen, so that a fetch completing after
 * the windows were built can rewrite them all.
 *
 * Each label is removed by a weak ref taken when it is added. A notice
 * lives and dies with the window holding it, which closing a card
 * destroys even when the card is immediately re-readied.
 */
static GSList *notice_labels;

static gboolean subscribed;

/* What the check found for this card, as markup, or NULL when there is
 * nothing to report. Free with g_free().
 *
 * The firmware line gives the offered and installed versions; the app
 * line gives the project URL to download from.
 */
static char *notice_markup(struct alsa_card *card) {
  const struct card_update_status *status = online_check_get_status(card);
  const char *client = online_check_get_client_update();

  char *firmware_line = NULL;

  if (status && status->available)
    firmware_line = g_markup_printf_escaped(
      "Firmware %s is available for this device (running %s).",
      status->offered_version, status->installed_version
    );

  if (!client)
    return firmware_line;

  char *client_line = g_markup_printf_escaped(
    "alsa-scarlett-gui %s is available: "
    "<a class=\"linktext\" href=\"%s\">%s</a>",
    client, PROJECT_URL, PROJECT_URL
  );

  if (!firmware_line)
    return client_line;

  char *both = g_strconcat(firmware_line, "\n", client_line, NULL);

  g_free(firmware_line);
  g_free(client_line);

  return both;
}

// Update a label's text, hiding it when there is nothing to report
static void refresh_label(GtkWidget *label) {
  struct alsa_card *card = g_object_get_data(G_OBJECT(label), "card");
  char *markup = notice_markup(card);

  gtk_widget_set_visible(label, markup != NULL);

  if (!markup)
    return;

  gtk_label_set_markup(GTK_LABEL(label), markup);
  g_free(markup);
}

/* Refresh every notice, since each shows the app version alongside its
 * own card's firmware.
 */
static void status_changed(struct alsa_card *card) {
  for (GSList *l = notice_labels; l; l = l->next)
    refresh_label(l->data);
}

static void label_finalised(gpointer data, GObject *label) {
  notice_labels = g_slist_remove(notice_labels, label);
}

GtkWidget *create_update_notice(struct alsa_card *card) {
  if (!subscribed) {
    subscribed = TRUE;
    online_check_add_status_changed_callback(status_changed);
  }

  GtkWidget *label = gtk_label_new(NULL);

  gtk_widget_add_css_class(label, "update-notice");
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);

  // Sized to its text, so the notice reads as an aside
  gtk_widget_set_halign(label, GTK_ALIGN_CENTER);

  g_object_set_data(G_OBJECT(label), "card", card);

  notice_labels = g_slist_prepend(notice_labels, label);
  g_object_weak_ref(G_OBJECT(label), label_finalised, NULL);

  refresh_label(label);

  return label;
}

#else // HAVE_ONLINE_UPDATE

GtkWidget *create_update_notice(struct alsa_card *card) {
  return NULL;
}

#endif // HAVE_ONLINE_UPDATE
