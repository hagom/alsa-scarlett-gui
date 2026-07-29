// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <glib.h>
#include <string.h>

#include "firmware-store.h"

struct firmware_entry {
  enum firmware_generation generation;
  uint32_t                 pid;
  uint32_t                 version[FIRMWARE_VERSION_PARTS];
  char                    *fn;
};

/* Best firmware per (generation, pid); a couple of dozen entries at
 * most. Entries are individually allocated so that version pointers
 * handed to callers survive the addition of further entries.
 */
static GPtrArray *entries = NULL;

static struct firmware_entry *find_entry(
  enum firmware_generation generation,
  uint32_t                 pid
) {
  if (!entries)
    return NULL;

  for (guint i = 0; i < entries->len; i++) {
    struct firmware_entry *entry = g_ptr_array_index(entries, i);

    if (entry->generation == generation && entry->pid == pid)
      return entry;
  }

  return NULL;
}

void firmware_store_add(
  enum firmware_generation  generation,
  uint32_t                  pid,
  const uint32_t           *version,
  char                     *fn
) {
  struct firmware_entry *entry = find_entry(generation, pid);

  // Already have firmware for this device?
  if (entry) {

    // Lower or equal version, ignore
    if (firmware_version_cmp(version, entry->version) <= 0) {
      g_free(fn);
      return;
    }

    // Higher version, replace
    g_free(entry->fn);
    entry->fn = fn;
    memcpy(entry->version, version, sizeof(entry->version));
    return;
  }

  if (!entries)
    entries = g_ptr_array_new();

  entry = g_malloc0(sizeof(struct firmware_entry));

  entry->generation = generation;
  entry->pid        = pid;
  entry->fn         = fn;
  memcpy(entry->version, version, sizeof(entry->version));

  g_ptr_array_add(entries, entry);
}

const uint32_t *firmware_store_get_version(
  enum firmware_generation generation,
  uint32_t                 pid
) {
  struct firmware_entry *entry = find_entry(generation, pid);

  return entry ? entry->version : NULL;
}

const char *firmware_store_get_filename(
  enum firmware_generation generation,
  uint32_t                 pid
) {
  struct firmware_entry *entry = find_entry(generation, pid);

  return entry ? entry->fn : NULL;
}
