// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "alsa.h"

/* The autogain window reports the progress and outcome of each
 * channel's autogain run. It is created hidden and presents itself
 * when a run starts or finishes, so nothing has to poll for it.
 *
 * Does nothing if the card has no autogain controls.
 */
void create_autogain_window(struct alsa_card *card);

// Destroy the window and deregister its ALSA callbacks.
void destroy_autogain_window(struct alsa_card *card);
