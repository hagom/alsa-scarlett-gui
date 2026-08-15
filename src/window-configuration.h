// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "alsa.h"

// Key for storing page ID on notebook pages (shared by config-*.c)
#define PAGE_ID_KEY "page-id"

// Wrap tab content in a scrolled window (shared by config-*.c)
GtkWidget *wrap_tab_content_scrolled(GtkWidget *content);

// Setup notebook tab persistence (shared by config-*.c)
void setup_notebook_tab_persistence(
  GtkNotebook      *notebook,
  struct alsa_card *card,
  const char       *key
);

GtkWidget *create_configuration_controls(struct alsa_card *card);

// Select a top-level configuration tab by its page ID; returns FALSE if
// the configuration window or the requested page does not exist
gboolean configuration_select_tab(struct alsa_card *card, const char *page_id);

// Floating "as" GVariant of the configuration tab ids in display order
GVariant *configuration_tab_ids(struct alsa_card *card);
