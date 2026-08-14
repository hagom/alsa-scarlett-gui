// SPDX-FileCopyrightText: 2024-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "alsa.h"

/* The firmware update screen. mid_upgrade says the device is part-way
 * through an upgrade and the remaining stage starts by itself; the
 * caller decides it, this being what routed the card here.
 */
GtkWidget *create_iface_update_main(struct alsa_card *card, int mid_upgrade);
