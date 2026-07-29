// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "alsa.h"

GtkWidget *create_startup_controls(struct alsa_card *card);

// Adds the Startup tab to the configuration notebook; adds nothing when
// the device has no startup controls or reset actions
void add_startup_tab(GtkWidget *notebook, struct alsa_card *card);
