// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

#include "alsa.h"

enum {
  WIDGET_GAIN_TAPER_LINEAR,
  WIDGET_GAIN_TAPER_LOG,
  WIDGET_GAIN_TAPER_GEN4_VOLUME
};

GtkWidget *make_gain_alsa_elem(
  struct alsa_elem *elem,
  int               zero_is_off,
  int               taper_type,
  int               can_control,
  gboolean          show_level
);

// Create a gain widget restricted to a table of values the device
// accepts (sorted ascending). Used for controls the device quantises
// to its own step table: the dial snaps to table entries and each
// keyboard/scroll step moves to the next real device value.
GtkWidget *make_mapped_gain_alsa_elem(
  struct alsa_elem *elem,
  int               zero_is_off,
  int               can_control,
  gboolean          show_level,
  const int        *value_map,
  int               value_map_len
);

// Give a gain widget a second channel to display. The widget shows
// the higher of the two values, which is what a stereo pair whose
// channels have been trimmed apart actually reaches. The peer is only
// read; the widget still writes to its own element.
void set_gain_display_peer(GtkWidget *gain_widget, struct alsa_elem *peer);

// Gang two gain widgets: dragging either moves both by the same
// amount, preserving the offset between them. Holding Ctrl moves only
// the widget being dragged, which is how the offset is set.
void gang_gain_widgets(GtkWidget *a, GtkWidget *b);

// Get the dial widget from a gain widget container (for level updates)
GtkWidget *get_gain_dial(GtkWidget *gain_widget);

// Clean up a gain widget (remove callbacks) before destroying it
// Must be called before unreffing/destroying the widget
void cleanup_gain_widget(GtkWidget *gain_widget);

// Maximum number of elements that can be controlled by a stereo gain widget
#define MAX_STEREO_GAIN_ELEMS 4

// Create a stereo gain widget controlling multiple elements in sync
// All elements must have compatible value ranges
GtkWidget *make_stereo_gain_alsa_elem(
  struct alsa_elem **elems,
  int               elem_count,
  int               zero_is_off,
  int               taper_type,
  int               can_control,
  gboolean          show_level
);
