// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

#ifdef HAVE_ONLINE_UPDATE

/* Holds only the online settings, so it does not exist in a build
 * without them.
 */

extern GtkWidget *window_preferences;

void create_preferences_window(GtkApplication *app);

#endif
