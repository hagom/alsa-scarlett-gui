// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

#include "alsa.h"

/* A line in the main window reporting what the online check found:
 * newer firmware for this card, or a newer release of this app. The
 * widget hides itself while there is nothing to report, and updates as
 * the check finds things.
 *
 * Returns NULL in a build without online update support, so the caller
 * appends nothing.
 */
GtkWidget *create_update_notice(struct alsa_card *card);
