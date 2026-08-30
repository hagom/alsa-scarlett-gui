// SPDX-FileCopyrightText: 2022-2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

#include "alsa.h"

// Create an empty context-menu popover parented to widget and pointing at
// (x, y) in that widget's coordinates
GtkWidget *routing_menu_popover_new(GtkWidget *widget, double x, double y);

// Port name qualified by its group title where the bare name would be
// ambiguous on its own.
// Returns a newly allocated string that must be freed.
char *get_src_qualified_name(struct routing_src *src);
char *get_snk_qualified_name(struct routing_snk *snk);

// Build a menu of checkboxes for the ports of one routing group.
// Returns an unparented popover for the caller to own, or NULL if the
// group has no ports.
GtkWidget *routing_port_menu_group_popover(
  struct alsa_card *card,
  int               is_src,
  int               port_category
);
