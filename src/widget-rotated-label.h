// SPDX-FileCopyrightText: 2022-2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

// The angle at which rotated text rises to the right, shared with the
// mixer window's input labels so that the two read as the same thing.
// Labels in adjacent columns are parallel, so they stay clear of each
// other only while the column pitch times the sine of the angle
// exceeds the text height.
#define ROTATED_LABEL_ANGLE (G_PI / 6.0)
#define ROTATED_LABEL_SIN   0.5
#define ROTATED_LABEL_COS   0.86602540378443864676

// A label drawn at an angle, for a name that has to sit under a column
// too narrow to hold it. The text rises to the right, past the width of
// the widget, so the column keeps its width and only the row gets
// taller.
//
// The widget itself only reserves the space and marks the position: a
// widget clips its own drawing, so the text is drawn on a surrounding
// widget by rotated_label_draw_on().
GtkWidget *rotated_label_new(void);

// Draw the label's text in the coordinates of a widget it sits within
void rotated_label_draw_on(
  GtkWidget *widget,
  GtkWidget *target,
  cairo_t   *cr
);

// Append the outline of the text, rotated with it and padded, to the
// current path, for a highlight drawn behind the label
void rotated_label_append_outline(
  GtkWidget *widget,
  GtkWidget *target,
  cairo_t   *cr,
  double     pad
);

// Is this widget a rotated label?
int is_rotated_label(GtkWidget *widget);

// Set the text, as Pango markup
void rotated_label_set_markup(GtkWidget *widget, const char *markup);

// Is the point, in the coordinates of a widget the label sits within,
// on the text?
int rotated_label_contains_point(
  GtkWidget *widget,
  GtkWidget *target,
  double     x,
  double     y,
  double     pad
);

// How far the text reaches to the left of the widget it belongs to
int rotated_label_get_overhang(GtkWidget *widget);
