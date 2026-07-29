// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

/* Stubs for what the code under test links against but does not
 * exercise.
 *
 * The alsa.c functions are the ones online-update.c calls to build the
 * census parameters; linking alsa.c itself would pull in the whole
 * GUI, and the cached-index paths under test never reach the census
 * builder, so "no cards" is the answer these need to give.
 *
 * The firmware store functions are what scarlett4-firmware.c needs.
 * That file is linked for real, because it defines
 * firmware_version_cmp() and the tests compare versions for real —
 * a second copy here is what this whole arrangement exists to avoid.
 */

#include <glib.h>

#include "../alsa.h"
#include "../firmware-store.h"

GArray *alsa_get_cards(void) {
  return NULL;
}

void firmware_store_add(
  enum firmware_generation  generation,
  uint32_t                  pid,
  const uint32_t           *version,
  char                     *fn
) {
  g_free(fn);
}

const uint32_t *firmware_store_get_version(
  enum firmware_generation generation,
  uint32_t                 pid
) {
  return NULL;
}

const char *firmware_store_get_filename(
  enum firmware_generation generation,
  uint32_t                 pid
) {
  return NULL;
}

long *alsa_get_elem_int_values(struct alsa_elem *elem) {
  return NULL;
}

char *card_firmware_version(struct alsa_card *card) {
  return NULL;
}
