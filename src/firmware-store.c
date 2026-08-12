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

enum firmware_generation card_firmware_generation(struct alsa_card *card) {
  return card->driver_type == DRIVER_TYPE_SOCKET ?
    FIRMWARE_GENERATION_SCARLETT4 : FIRMWARE_GENERATION_SCARLETT2;
}

const uint32_t *card_best_local_firmware(struct alsa_card *card) {
  return firmware_store_get_version(
    card_firmware_generation(card), card->pid
  );
}

int card_running_firmware_version(struct alsa_card *card, uint32_t *parts) {
  struct alsa_elem *elem = get_elem_by_name(card->elems, "Firmware Version");

  if (!elem)
    return 0;

  /* A scarlett2-era device reports one component; the store holds it in
   * element 0 with the remainder zero, so a single-component version
   * compares against a held one directly.
   */
  if (elem->count == 1) {
    memset(parts, 0, FIRMWARE_VERSION_PARTS * sizeof(*parts));
    parts[0] = alsa_get_elem_value(elem);
    return 1;
  }

  return firmware_version_from_elem(elem, parts);
}

gboolean card_firmware_update_available(struct alsa_card *card) {
  const uint32_t *best = card_best_local_firmware(card);

  if (!best)
    return FALSE;

  uint32_t running[FIRMWARE_VERSION_PARTS];

  if (!card_running_firmware_version(card, running))
    return FALSE;

  return firmware_version_cmp(best, running) > 0;
}

char *card_format_firmware_version(
  struct alsa_card *card,
  const uint32_t   *version
) {
  if (card_firmware_generation(card) == FIRMWARE_GENERATION_SCARLETT2)
    return g_strdup_printf("%u", version[0]);

  GString *s = g_string_new(NULL);

  for (int i = 0; i < FIRMWARE_VERSION_PARTS; i++)
    g_string_append_printf(s, i ? ".%u" : "%u", version[i]);

  return g_string_free(s, FALSE);
}
