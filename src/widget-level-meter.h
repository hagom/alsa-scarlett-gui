// SPDX-FileCopyrightText: 2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GTK_TYPE_LEVEL_METER (gtk_level_meter_get_type())

G_DECLARE_FINAL_TYPE(
  GtkLevelMeter,
  gtk_level_meter,
  GTK,
  LEVEL_METER,
  GtkWidget
)

// A vertical level meter: a coloured bar rising from the bottom of a
// trough, with a peak-hold marker and tick marks at the scale steps.
// The scale runs from -60 dB to 0 dB, linear down to -30 dB and
// compressed below it.
GtkWidget *gtk_level_meter_new(void);

// A dB scale to place beside a column of meters. It shares the meters'
// vertical layout, so its labels line up with their tick marks as long
// as both are given the same height.
GtkWidget *gtk_level_meter_scale_new(void);

// how long a peak is held for, in ms; zero disables the peak marker
void gtk_level_meter_set_peak_hold(GtkLevelMeter *meter, int peak_hold);

// A meter whose level can't be determined draws hatched instead of
// showing a bar; the levels window uses this for ports the hardware
// doesn't meter that aren't routed anywhere that is metered.
void gtk_level_meter_set_unknown(GtkLevelMeter *meter, gboolean unknown);

void gtk_level_meter_set_level(GtkLevelMeter *meter, double level_db);
double gtk_level_meter_get_level(GtkLevelMeter *meter);

G_END_DECLS
