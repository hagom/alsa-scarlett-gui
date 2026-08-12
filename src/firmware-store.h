// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>

#include "firmware-probe.h"
#include "scarlett4-firmware.h"

/* Best available firmware file per (generation, USB product id).
 *
 * Only the file name and version are kept; the parsed header is not
 * retained, because callers wanting the firmware data re-read the
 * file. Versions are held as FIRMWARE_VERSION_PARTS components for
 * both generations: scarlett2 has a single-component version, stored
 * in element 0 with the remainder zero.
 */

/* Record fn as the best firmware for this generation and pid if no
 * better version is already known. Takes ownership of fn.
 */
void firmware_store_add(
  enum firmware_generation  generation,
  uint32_t                  pid,
  const uint32_t           *version,
  char                     *fn
);

/* Discard every entry, so that firmware_enum_all() can be run again to
 * pick up a newly downloaded file.
 *
 * Invalidates every pointer previously returned by this store. Callers
 * holding one must re-ask after the re-enumeration that follows.
 */
void firmware_store_reset(void);

/* The best known version, or NULL if none.
 *
 * The returned pointer is owned by the store and is valid only while
 * the store holds that entry. A better version arriving for the same
 * generation and pid updates the values in place rather than moving
 * them, so the pointer stays usable but what it points at may change.
 */
const uint32_t *firmware_store_get_version(
  enum firmware_generation generation,
  uint32_t                 pid
);

// The file holding the best known firmware, or NULL if none
const char *firmware_store_get_filename(
  enum firmware_generation generation,
  uint32_t                 pid
);

/* Which generation's firmware files belong to a card. Socket cards are
 * scarlett4; every other card is scarlett2.
 */
enum firmware_generation card_firmware_generation(struct alsa_card *card);

/* The best firmware version held for a card, from any of the
 * directories walked, or NULL if none. Ownership and lifetime are those
 * of firmware_store_get_version().
 */
const uint32_t *card_best_local_firmware(struct alsa_card *card);

/* The firmware version a card reports running, written to parts as
 * FIRMWARE_VERSION_PARTS components so that it compares against a held
 * version. Reads the "Firmware Version" control, which carries a single
 * component for the scarlett2 era and four for scarlett4.
 *
 * Returns 0, leaving parts untouched, when the card reports no version.
 */
int card_running_firmware_version(struct alsa_card *card, uint32_t *parts);

/* Whether a card has firmware held locally that is newer than the
 * version it is running, which is what makes an update installable
 * without downloading anything.
 */
gboolean card_firmware_update_available(struct alsa_card *card);

/* A held firmware version as text, formatted the way a device of that
 * card's generation reports its own: a single number for the scarlett2
 * era, four dotted components for scarlett4. Free with g_free().
 */
char *card_format_firmware_version(
  struct alsa_card *card,
  const uint32_t   *version
);
