// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

#include "alsa.h"

gboolean window_configuration_close_request(GtkWindow *w, gpointer data);

/* Tag a widget with the device family, for styling that follows the
 * hardware rather than the control: the Vocaster's autogain indication
 * is orange where the Scarletts' is blue.
 *
 * Goes on the "window-content" widget, not the "window-frame" one, so
 * that a ".window-frame .<family>" selector has two widgets to match.
 */
void add_card_style_class(GtkWidget *widget, struct alsa_card *card);

GtkWidget *create_subwindow(
  struct alsa_card *card,
  const char       *name,
  GCallback         close_callback
);
