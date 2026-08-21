// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <math.h>
#include <string.h>

#include "glow.h"

// calculate glow intensity (0 to 1) from dB level, with curve applied
double get_glow_intensity(double level_db) {
  if (level_db < GLOW_MIN_DB)
    return 0.0;

  double intensity = (level_db - GLOW_MIN_DB) / (GLOW_MAX_DB - GLOW_MIN_DB);
  if (intensity > 1.0)
    intensity = 1.0;

  // apply curve so glow ramps up more gradually
  return intensity * intensity;
}

// calculate glow layer width and alpha for a given layer and intensity
void get_glow_layer_params(
  int     layer,
  double  intensity,
  double *width,
  double *alpha
) {
  double layer_frac = (double)layer / (GLOW_LAYERS - 1);

  // width: minimum of 4 pixels, scaling up to GLOW_MAX_WIDTH
  // (routing line is 2 pixels, so glow must be > 2 to be visible)
  *width = 4.0 + (GLOW_MAX_WIDTH - 4.0) * intensity * (0.3 + 0.7 * layer_frac);

  // alpha: minimum of 0.08, scaling up with intensity
  *alpha = 0.08 + intensity * 0.32 * (1.0 - 0.7 * layer_frac);
}

const int level_band_breakpoints[LEVEL_BAND_COUNT] = {
  -80, -60, -18, -12, -6, -3, -1
};

const double level_band_colours[LEVEL_BAND_COUNT * 3] = {
  0.00, 1.00, 0.00, // -80
  0.00, 1.00, 0.00, // -60
  0.75, 1.00, 0.00, // -18
  1.00, 1.00, 0.00, // -12
  1.00, 0.75, 0.00, //  -6
  1.00, 0.50, 0.00, //  -3
  1.00, 0.00, 0.00  //  -1
};

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
) {
  int i;

  for (i = 0; i < count - 1; i++)
    if (db < breakpoints[i + 1])
      break;

  const double *low = &colours[i * 3];

  // at or above the last breakpoint: nothing to fade towards
  if (i == count - 1 || db <= breakpoints[i]) {
    *r = low[0];
    *g = low[1];
    *b = low[2];
    return;
  }

  const double *high = &colours[(i + 1) * 3];
  double frac =
    (db - breakpoints[i]) / (double)(breakpoints[i + 1] - breakpoints[i]);

  *r = low[0] + (high[0] - low[0]) * frac;
  *g = low[1] + (high[1] - low[1]) * frac;
  *b = low[2] + (high[2] - low[2]) * frac;
}

// the colour of a level, fading between the band colours
void level_band_colour(double db, double *r, double *g, double *b) {
  interpolate_band_colour(
    level_band_breakpoints, level_band_colours, LEVEL_BAND_COUNT,
    db, r, g, b
  );
}

// convert dB level to RGB colour (green → yellow → red)
void level_to_colour(double db, double *r, double *g, double *b) {
  if (db < -18.0) {
    // green
    *r = 0.0;
    *g = 1.0;
    *b = 0.0;
  } else if (db < -12.0) {
    // green → yellow-green
    double t = (db + 18.0) / 6.0;
    *r = 0.5 * t;
    *g = 1.0;
    *b = 0.0;
  } else if (db < -6.0) {
    // yellow-green → yellow
    double t = (db + 12.0) / 6.0;
    *r = 0.5 + 0.5 * t;
    *g = 1.0;
    *b = 0.0;
  } else if (db < -3.0) {
    // yellow → orange
    double t = (db + 6.0) / 3.0;
    *r = 1.0;
    *g = 1.0 - 0.25 * t;
    *b = 0.0;
  } else {
    // orange → red
    double t = fmin(1.0, (db + 3.0) / 3.0);
    *r = 1.0;
    *g = 0.75 - 0.75 * t;
    *b = 0.0;
  }
}

// compute the level meter index for a routing sink
static int compute_snk_level_index(
  struct alsa_card   *card,
  struct routing_snk *r_snk
) {
  if (!r_snk->elem)
    return -1;

  int cat = r_snk->elem->port_category;
  if (cat == PC_OFF)
    return -1;

  // if meter labels are available, search for matching "Sink" label
  if (card->level_meter_elem && card->level_meter_elem->meter_labels) {
    for (int i = 0; i < card->routing_levels_count; i++) {
      const char *label = card->level_meter_elem->meter_labels[i];
      if (!label)
        continue;

      if (strncmp(label, "Sink ", 5) != 0)
        continue;

      // label+5 is like "Analogue 1"; elem->name is like
      // "Analogue 1 Playback Enum"; match the label as a
      // complete word prefix (followed by space)
      const char *sink_name = label + 5;
      int sink_name_len = strlen(sink_name);
      if (strncmp(r_snk->elem->name, sink_name, sink_name_len) == 0 &&
          r_snk->elem->name[sink_name_len] == ' ')
        return i;
    }

    return -1;
  }

  // without labels, level meters are ordered:
  // HW Outputs, Mixer Inputs, DSP Inputs, PCM Inputs
  int index = 0;
  for (int c = PC_HW; c < cat; c++)
    index += card->routing_out_count[c];
  index += r_snk->elem->port_num;

  if (index >= card->routing_levels_count)
    return -1;

  return index;
}

// compute the level meter index for a routing source
static int compute_src_level_index(
  struct alsa_card   *card,
  struct routing_src *r_src
) {
  if (!card->level_meter_elem || !card->routing_levels ||
      r_src->port_category == PC_OFF)
    return -1;

  // if meter labels are available, search for matching "Source" label
  if (card->level_meter_elem->meter_labels) {
    for (int i = 0; i < card->routing_levels_count; i++) {
      const char *label = card->level_meter_elem->meter_labels[i];
      if (!label)
        continue;

      if (strncmp(label, "Source ", 7) != 0)
        continue;

      if (strcmp(label + 7, r_src->name) == 0)
        return i;
    }

    return -1;
  }

  // without labels there are no source meters; the level of a source
  // is derived from a sink it is routed to
  return -1;
}

// compute and cache level meter indices for all routing srcs/snks
void init_routing_level_indices(struct alsa_card *card) {

  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *r_snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );
    r_snk->level_index = compute_snk_level_index(card, r_snk);
  }

  for (int i = 0; i < card->routing_srcs->len; i++) {
    struct routing_src *r_src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );
    r_src->level_index = compute_src_level_index(card, r_src);
  }
}

// get the cached level meter index for a routing sink
int get_routing_snk_level_index(struct routing_snk *r_snk) {
  return r_snk->level_index;
}

// the routing source a sink is fed from, or NULL if it is off or the
// source no longer exists
static struct routing_src *get_snk_source(
  struct alsa_card   *card,
  struct routing_snk *r_snk
) {
  int src_id = r_snk->effective_source_idx;

  if (src_id <= 0)
    return NULL;

  for (int i = 0; i < card->routing_srcs->len; i++) {
    struct routing_src *r_src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );

    if (r_src->id == src_id)
      return r_src;
  }

  return NULL;
}

// The level of a routing source. A source the hardware doesn't meter
// takes the level of a sink it is routed to; if it isn't routed
// anywhere that is metered, the level is unknown and *known is set to
// 0 (pass NULL if the distinction doesn't matter).
double get_routing_src_level(
  struct alsa_card   *card,
  struct routing_src *r_src,
  int                *known
) {
  if (known)
    *known = 1;

  if (r_src->port_category == PC_OFF)
    return LEVEL_DB_MIN;

  if (r_src->level_index >= 0)
    return card->routing_levels[r_src->level_index];

  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *r_snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );

    if (r_snk->effective_source_idx == r_src->id && r_snk->level_index >= 0)
      return card->routing_levels[r_snk->level_index];
  }

  if (known)
    *known = 0;

  return LEVEL_DB_MIN;
}

// The level of a routing sink. A sink the hardware doesn't meter takes
// the level of the source routed to it, which is silent if that is
// off; if the source isn't metered either, the level is unknown and
// *known is set to 0 (pass NULL if the distinction doesn't matter).
double get_routing_snk_level(
  struct alsa_card   *card,
  struct routing_snk *r_snk,
  int                *known
) {
  if (known)
    *known = 1;

  if (r_snk->level_index >= 0)
    return card->routing_levels[r_snk->level_index];

  struct routing_src *r_src = get_snk_source(card, r_snk);

  if (!r_src)
    return LEVEL_DB_MIN;

  if (r_src->level_index >= 0)
    return card->routing_levels[r_src->level_index];

  if (known)
    *known = 0;

  return LEVEL_DB_MIN;
}

// get level in dB for a routing source (LEVEL_DB_MIN if no level data)
double get_routing_src_level_db(
  struct alsa_card   *card,
  struct routing_src *r_src
) {
  if (!card->routing_levels)
    return LEVEL_DB_MIN;

  return get_routing_src_level(card, r_src, NULL);
}
