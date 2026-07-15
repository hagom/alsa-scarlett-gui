// SPDX-FileCopyrightText: 2023-2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

#include "alsa.h"

// link_elem (optional) is the pair's stereo link control: when linked,
// both channels' buttons are shown selected
GtkWidget *make_input_select_alsa_elem(
  struct alsa_elem *alsa_elem,
  struct alsa_elem *link_elem,
  int               line_num
);
