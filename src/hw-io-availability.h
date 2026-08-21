// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "alsa.h"
#include "asound-stream-parser.h"

// Update the HW I/O limits stored in the card struct
void update_hw_io_limits(struct alsa_card *card);

// Whether a routing endpoint's port exists with the current Digital
// I/O mode and sample rate
int is_routing_src_available(struct routing_src *r_src);
int is_routing_snk_available(struct routing_snk *r_snk);
