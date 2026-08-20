// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "about.h"
#include "alsa.h"
#include "hardware.h"

static GdkTexture *logo = NULL;

// One "Label: value" line, dropped when the value isn't known
static void append_field(
  GString    *s,
  const char *label,
  const char *value
) {
  if (!value || !*value)
    return;

  g_string_append_printf(s, "  %s: %s\n", label, value);
}

// What the About dialog reports about one card. The series and model
// come from the USB product ID, so a device the GUI doesn't recognise
// is listed by its ALSA name alone.
static void append_card_information(GString *s, struct alsa_card *card) {
  struct scarlett2_device *device = get_device_for_pid(card->pid);
  int simulated = card->num == SIMULATED_CARD_NUM;

  if (s->len)
    g_string_append_c(s, '\n');

  const char *name = card->name ? card->name : "Unknown Device";

  if (simulated)
    g_string_append_printf(s, "%s (simulated)\n", name);
  else
    g_string_append_printf(s, "%s (card %d)\n", name, card->num);

  append_field(s, "Series", device ? device->series : NULL);
  append_field(s, "Model", device ? device->model : NULL);

  if (card->pid) {
    char *usb_id = g_strdup_printf("%04x:%04x", card->vid, card->pid);

    append_field(s, "USB ID", usb_id);
    g_free(usb_id);
  }

  // a simulated card's serial number is made up from the file name
  if (!simulated)
    append_field(s, "Serial Number", card->serial);

  char *firmware_version = card_firmware_version(card);

  append_field(s, "Firmware Version", firmware_version);
  g_free(firmware_version);
}

// The connected devices, for the About dialog's System page. Free with
// g_free().
static char *get_device_information(void) {
  GArray *cards = alsa_get_cards();
  GString *s = g_string_new(NULL);

  for (guint i = 0; cards && i < cards->len; i++) {
    struct alsa_card *card = g_array_index(cards, struct alsa_card *, i);

    if (card)
      append_card_information(s, card);
  }

  if (!s->len)
    g_string_append(s, "No devices found.\n");

  return g_string_free(s, FALSE);
}

void activate_about(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  GtkWindow *w = GTK_WINDOW(data);

  const char *authors[] = {
    "Geoffrey D. Bennett <g@b4.vu>",
    NULL
  };

  if (!logo)
    logo = gdk_texture_new_from_resource(
      "/vu/b4/alsa-scarlett-gui/icons/vu.b4.alsa-scarlett-gui.png"
    );

  char *device_information = get_device_information();

  gtk_show_about_dialog(
    w,
    "program-name", "ALSA Scarlett Control Panel",
    "version", "Version " VERSION,
    "comments",
      "Gtk4 GUI for the ALSA controls presented by the\n"
      "Linux kernel Focusrite USB drivers",
    "website", "https://github.com/geoffreybennett/alsa-scarlett-gui",
    "copyright", "Copyright 2022-2025 Geoffrey D. Bennett",
    "license-type", GTK_LICENSE_GPL_3_0,
    "logo", logo,
    "title", "About ALSA Scarlett Mixer Interface",
    "authors", authors,
    "system-information", device_information,
    NULL
  );

  g_free(device_information);
}
