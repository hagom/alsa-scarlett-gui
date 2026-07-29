// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

#ifdef HAVE_ONLINE_UPDATE

typedef void (*consent_done_cb)(GtkApplication *app, gpointer user_data);

/* Ask, on first run, whether to check online for updates and whether to
 * send the usage census, then run done_cb.
 *
 * Nothing else of the app may be visible while this is up, so the
 * caller's remaining startup belongs in done_cb. It runs exactly once:
 * immediately when a choice has already been recorded, otherwise once
 * the last window is answered or closed.
 *
 * There is nothing to ask about in a build that cannot check online, so
 * the caller runs its own continuation directly there.
 */
void show_consent(
  GtkApplication  *app,
  consent_done_cb  done_cb,
  gpointer         user_data
);

#endif
