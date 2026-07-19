// SPDX-FileCopyrightText: 2023-2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

#include "alsa.h"

// hw_link_elem (optional) is the device's link control: when linked,
// the device gangs the selection so both channels' buttons are shown
// selected. gui_link_elem (optional) is the GUI stereo link: when
// linked, the left button shows the pair range for the collapsed pair.
GtkWidget *make_input_select_alsa_elem(
  struct alsa_elem *alsa_elem,
  struct alsa_elem *hw_link_elem,
  struct alsa_elem *gui_link_elem,
  int               line_num
);
