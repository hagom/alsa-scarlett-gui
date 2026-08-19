// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

#include "alsa.h"

/* The channel's autogain switch, or NULL if the GUI has nothing to do.
 *
 * The scarlett2 driver marks a channel's device controls read-only
 * while autogain runs, which the widgets pick up from the element's
 * writability. The FCP server's controls keep the access they were
 * created with, so on those devices the GUI holds them itself.
 */
struct alsa_elem *get_autogain_hold_elem(struct alsa_card *card, int ch);

/* Grey out a device control belonging to the channel pair starting at
 * ch, while autogain runs on either of its channels. The device
 * ignores writes to these for the duration.
 *
 * elem is the control's own element, so the hold is re-asserted when
 * the widget sets its own sensitivity from writability. Does nothing
 * on devices whose driver holds the control itself.
 */
void autogain_hold_pair_control(
  GtkWidget        *widget,
  struct alsa_card *card,
  int               ch,
  struct alsa_elem *elem
);

/* Grey out a control of the GUI's own while autogain runs on any
 * channel. No driver marks these read-only, so every device needs the
 * hold, and every channel counts: stereo pairing follows routing
 * connections, so pairing anything can repair a channel that autogain
 * is running on.
 */
void autogain_hold_gui_control(
  GtkWidget        *widget,
  struct alsa_card *card,
  struct alsa_elem *elem
);
