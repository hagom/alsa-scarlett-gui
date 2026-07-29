// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "alsa.h"

// Get the window title for a card (with custom name if set)
// Returns newly allocated string that must be freed
char *get_card_window_title(struct alsa_card *card);

// register this file as the observer of alsa.c card lifecycle events
void window_iface_init(void);

void create_no_card_window(void);
void check_modal_window_closed(void);
