// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

// Render a mapped widget to a PNG file; returns FALSE if the widget
// cannot be rendered or the file cannot be written
gboolean screenshot_widget_to_png(
  GtkWidget  *widget,
  const char *filename
);
