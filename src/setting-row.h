// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

// A grid to hold setting rows added by add_setting_row()
GtkWidget *make_settings_grid(void);

/* Appends one setting to a grid made by make_settings_grid(): heading
 * and control in the left column, description in the right, separated
 * from the setting above it. grid_y tracks the next free row and starts
 * at zero.
 *
 * Returns the box holding the description, for a caller with more to
 * say about this setting than the description itself.
 */
GtkWidget *add_setting_row(
  GtkWidget  *grid,
  int        *grid_y,
  const char *label,
  GtkWidget  *control,
  const char *description
);
