// SPDX-FileCopyrightText: 2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Peak tracking over a sliding time window, shared by the widgets that
// display signal levels.

#define PEAK_HISTORY_COUNT 50

struct peak_tracker {
  double    values[PEAK_HISTORY_COUNT];
  long long times[PEAK_HISTORY_COUNT];
  double    peak;

  // how long a peak is held for, in ms; zero disables tracking
  int       hold_ms;

  int       head, tail, count;
};

void peak_tracker_init(struct peak_tracker *pt);

// sample the clock that peak_tracker_add() timestamps values with; call
// once per round of level updates
void peak_tracker_tick(void);

void peak_tracker_add(struct peak_tracker *pt, double value);
