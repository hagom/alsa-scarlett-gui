// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

#include "alsa.h"

// Create a boolean switch button paired with a popup dial controlling
// an associated integer level element (e.g. Console/Console Level,
// Drive/Drive Level).
GtkWidget *make_switch_dial_alsa_elem(
  struct alsa_elem *switch_elem,
  struct alsa_elem *level_elem,
  const char       *label
);
