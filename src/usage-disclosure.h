// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <gtk/gtk.h>

#ifdef HAVE_ONLINE_UPDATE

/* What the usage survey sends and what becomes of it, shown the same way
 * wherever it is disclosed: the consent dialog that asks, and the
 * preferences that let the answer be changed.
 *
 * There is nothing to disclose in a build that cannot check online, so
 * none of this exists there.
 */

/* A collapsed detail section appended to content. Monospace suits text
 * meant to be read literally, such as the request line. footnote may be
 * NULL.
 */
void add_detail_expander(
  GtkWidget  *content,
  const char *label,
  const char *text,
  gboolean    monospace,
  const char *footnote
);

/* Both expanders: the literal request line this machine would send, and
 * how it is handled once it arrives.
 */
void add_usage_disclosure_expanders(GtkWidget *content);

#endif
