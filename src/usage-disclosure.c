// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HAVE_ONLINE_UPDATE

#include "alsa.h"
#include "online-update.h"
#include "usage-disclosure.h"

/* Every request line on display. The dev parameters depend on which
 * devices are attached, so they are all rewritten when that changes.
 */
static GSList *request_lines;

GtkWidget *add_detail_expander(
  GtkWidget  *content,
  const char *label,
  const char *text,
  gboolean    monospace,
  const char *footnote
) {
  GtkWidget *expander = gtk_expander_new(NULL);
  GtkWidget *expander_label = gtk_label_new(label);

  gtk_widget_add_css_class(expander_label, "consent-detail-label");
  gtk_expander_set_label_widget(GTK_EXPANDER(expander), expander_label);
  gtk_box_append(GTK_BOX(content), expander);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_expander_set_child(GTK_EXPANDER(expander), box);

  GtkWidget *detail = gtk_label_new(text);

  if (monospace)
    gtk_widget_add_css_class(detail, "monospace");

  /* GTK indents what an expander reveals under the disclosure triangle,
   * so it is left aligned rather than centred in the window.
   */
  gtk_label_set_xalign(GTK_LABEL(detail), 0.0);
  gtk_label_set_selectable(GTK_LABEL(detail), TRUE);
  gtk_box_append(GTK_BOX(box), detail);

  if (!footnote)
    return detail;

  GtkWidget *note = gtk_label_new(footnote);

  gtk_widget_add_css_class(note, "consent-aside");
  gtk_label_set_xalign(GTK_LABEL(note), 0.0);
  gtk_box_append(GTK_BOX(box), note);

  return detail;
}

/* The request line, one query parameter per line so that a reader can
 * check them off individually.
 */
static char *request_line_text(void) {
  char *url = online_update_index_url(TRUE);
  GString *text = g_string_new("GET ");

  for (const char *p = url; *p; p++) {
    if (*p == '?' || *p == '&')
      g_string_append(text, "\n    ");
    g_string_append_c(text, *p);
  }

  g_free(url);

  /* With no device attached there is no dev parameter to show, so show
   * the shape of one instead.
   */
  if (!online_update_reports_a_device())
    g_string_append(
      text,
      "\n    (with a device attached:"
      "\n    &" ONLINE_UPDATE_SAMPLE_DEV ")"
    );

  return g_string_free(text, FALSE);
}

static void forget_request_line(gpointer data, GObject *label) {
  request_lines = g_slist_remove(request_lines, label);
}

static void devices_changed(void) {
  if (!request_lines)
    return;

  char *text = request_line_text();

  for (GSList *l = request_lines; l; l = l->next)
    gtk_label_set_text(GTK_LABEL(l->data), text);

  g_free(text);
}

void add_usage_disclosure_expanders(GtkWidget *content) {

  /* Plain text, not markup: the URL's "&" separators would be read as
   * the start of a Pango entity.
   */
  char *request_line = request_line_text();

  GtkWidget *detail = add_detail_expander(
    content, "Show what is sent", request_line, TRUE,
    "dev is your device's USB product id, its firmware version, and the\n"
    "first 16 characters of the SHA256 of its serial number (enough to\n"
    "de-dupe)."
  );
  g_free(request_line);

  /* The request line names the attached devices, so it is rewritten
   * whenever they change. Registered on first use rather than from an
   * init function: the consent dialog shows a request line before the
   * rest of the app is set up, and it has to track devices too.
   */
  static gboolean registered;

  if (!registered) {
    alsa_add_cards_changed_callback(devices_changed);
    registered = TRUE;
  }

  request_lines = g_slist_prepend(request_lines, detail);
  g_object_weak_ref(G_OBJECT(detail), forget_request_line, NULL);

  add_detail_expander(
    content, "Show how it is handled",
    "The request lands in the author's web server log. A script adds it\n"
    "up into counts of devices, firmware versions, kernels, and distros,\n"
    "using the hashed serial to tell one device from another.\n\n"
    "The log holds your IP address, as any web request does, and is\n"
    "rotated away after about a month.",
    FALSE, NULL
  );
}

#endif // HAVE_ONLINE_UPDATE
