// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

struct alsa_elem;

/* Mark widget as part of the documented control elem, so its position
 * and identity are recorded by export_widget_geometry_to_json(). Does
 * nothing when elem is NULL, so a caller can pass whatever element it
 * looked for without testing first.
 *
 * A control registered on several widgets takes the union of their
 * positions, for one drawn as pieces that sit apart: a routing port's
 * socket and its label. A widget may be part of several controls, as a
 * talkback button is both the switch and where its mix's name is
 * written.
 */
void widget_doc_register(GtkWidget *widget, struct alsa_elem *elem);

// As above, for a control with no ALSA element of its own (e.g. the
// sample rate display), identified in the export by name.
void widget_doc_register_named(GtkWidget *widget, const char *name);

/* Mark widget as part of a documented section: the region its crop
 * takes, belonging to documentation group group and headed by title.
 * instance distinguishes sections of the same group within one window
 * (the DSP sections, one set per channel; a mixer row per mix), and is
 * NULL for a group that appears once; it goes into the id that names
 * the crop.
 *
 * A section's region is the union of the positions of every widget
 * registered to it, so a mixer row is built by registering its label
 * and each of its gains. A widget may be in several sections, as a
 * mixer gain is in both a row and a column.
 */
void widget_doc_register_group(
  GtkWidget  *widget,
  const char *group,
  const char *instance,
  const char *title
);

// Write a JSON sidecar describing the geometry of documented regions
// within root, in the pixel coordinates of the PNG produced by
// screenshot_widget_to_png() for the same widget. image_name is
// recorded in the JSON so a consumer can pair it with its screenshot.
gboolean export_widget_geometry_to_json(
  GtkWidget  *root,
  const char *image_name,
  const char *filename
);
