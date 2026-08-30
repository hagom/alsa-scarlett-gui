// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>
#include "alsa.h"

void draw_routing_lines(
  GtkDrawingArea *drawing_area,
  cairo_t        *cr,
  int             width,
  int             height,
  void           *user_data
);

void draw_drag_line(
  GtkDrawingArea *drawing_area,
  cairo_t        *cr,
  int             width,
  int             height,
  void           *user_data
);

// Direction the hidden-connection arrow indicator points, away from the
// port and towards the middle of the routing window
enum {
  ARROW_DIR_RIGHT = 0,
  ARROW_DIR_LEFT  = 1,
  ARROW_DIR_UP    = 2,
  ARROW_DIR_DOWN  = 3
};

int routing_src_arrow_dir(struct routing_src *r_src);
int routing_snk_arrow_dir(struct routing_snk *r_snk);

// level indication for routing lines
void routing_levels_init(struct alsa_card *card);
void routing_levels_cleanup(struct alsa_card *card);
