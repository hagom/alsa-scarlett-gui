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
