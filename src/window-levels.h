// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "alsa.h"

GtkWidget *create_levels_controls(struct alsa_card *card);

void restart_levels_timer(struct alsa_card *card);

// Re-apply the levels window layout when the set of available ports
// changes
void update_levels_availability(struct alsa_card *card);
