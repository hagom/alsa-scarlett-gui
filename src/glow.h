// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cairo.h>
#include "alsa.h"

// glow effect configuration
#define GLOW_LAYERS 4
#define GLOW_MAX_WIDTH 16.0
#define GLOW_MIN_DB -60.0
#define GLOW_MAX_DB 0.0

// calculate glow intensity (0 to 1) from dB level, with curve applied
double get_glow_intensity(double level_db);

// calculate glow layer width and alpha for a given layer and intensity
void get_glow_layer_params(
  int     layer,
  double  intensity,
  double *width,
  double *alpha
);

// convert dB level to RGB colour (green → yellow → red)
void level_to_colour(double db, double *r, double *g, double *b);

// Level meter colour bands, as used by the dials' level display and by
// the vertical level meters. Each colour is exact at its own
// breakpoint and fades into the next one.
#define LEVEL_BAND_COUNT 7

extern const int level_band_breakpoints[LEVEL_BAND_COUNT];
extern const double level_band_colours[LEVEL_BAND_COUNT * 3];

// interpolate a colour from a breakpoint/colour table: each colour is
// exact at its own breakpoint and fades linearly to the next one
void interpolate_band_colour(
  const int    *breakpoints,
  const double *colours,
  int           count,
  double        db,
  double       *r,
  double       *g,
  double       *b
);

// the colour of a level, fading between the band colours
void level_band_colour(double db, double *r, double *g, double *b);

// the level shown for a signal that is off, or whose level is unknown
#define LEVEL_DB_MIN -80.0

// compute and cache level meter indices for all routing srcs/snks
void init_routing_level_indices(struct alsa_card *card);

// get the cached level meter index for a routing sink
int get_routing_snk_level_index(struct routing_snk *r_snk);

// The level of a routing endpoint the hardware may not meter itself,
// taken from the endpoint it is routed to when it doesn't. *known is
// set to 0 when nothing carrying the signal is metered; pass NULL if
// the distinction doesn't matter.
double get_routing_src_level(
  struct alsa_card   *card,
  struct routing_src *r_src,
  int                *known
);

double get_routing_snk_level(
  struct alsa_card   *card,
  struct routing_snk *r_snk,
  int                *known
);

// get level in dB for a routing source (-80 if no level data)
double get_routing_src_level_db(
  struct alsa_card   *card,
  struct routing_src *r_src
);
