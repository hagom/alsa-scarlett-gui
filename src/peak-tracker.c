// SPDX-FileCopyrightText: 2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <math.h>
#include <time.h>

#include "peak-tracker.h"

static long long current_time = 0;

void peak_tracker_init(struct peak_tracker *pt) {
  pt->peak = -INFINITY;
  pt->hold_ms = 0;
  pt->head = 0;
  pt->tail = 0;
  pt->count = 0;
}

void peak_tracker_tick(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_BOOTTIME, &ts) < 0)
    return;

  current_time = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void peak_tracker_add(struct peak_tracker *pt, double value) {
  int need_peak_update = 0;

  // remove the oldest value(s) if they are too old or if the history
  // is full
  while (pt->count > 0 &&
         (pt->times[pt->head] < current_time - pt->hold_ms ||
          pt->count == PEAK_HISTORY_COUNT)) {

    // check if the value removed is the current peak
    if (pt->values[pt->head] >= pt->peak)
      need_peak_update = 1;

    // move the head forward
    pt->head = (pt->head + 1) % PEAK_HISTORY_COUNT;
    pt->count--;
  }

  // recalculate the peak if needed
  if (need_peak_update) {
    pt->peak = -INFINITY;
    for (int i = pt->head; i != pt->tail; i = (i + 1) % PEAK_HISTORY_COUNT)
      if (pt->values[i] > pt->peak)
        pt->peak = pt->values[i];
  }

  // add the new value
  pt->values[pt->tail] = value;
  pt->times[pt->tail] = current_time;
  pt->tail = (pt->tail + 1) % PEAK_HISTORY_COUNT;
  pt->count++;

  // update the peak if needed
  if (value > pt->peak)
    pt->peak = value;
}
