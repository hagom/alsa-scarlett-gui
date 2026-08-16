// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

struct alsa_elem;

// Mark widget as the documented representative of elem, so its position
// and identity are recorded by export_widget_geometry_to_json().
void widget_doc_register(GtkWidget *widget, struct alsa_elem *elem);

// As above, for a documented widget that has no ALSA element of its own
// (e.g. the sample rate display); name identifies it in the export.
void widget_doc_register_named(GtkWidget *widget, const char *name);

// Write a JSON sidecar describing the geometry of documented regions
// within root, in the pixel coordinates of the PNG produced by
// screenshot_widget_to_png() for the same widget. image_name is
// recorded in the JSON so a consumer can pair it with its screenshot.
gboolean export_widget_geometry_to_json(
  GtkWidget  *root,
  const char *image_name,
  const char *filename
);
