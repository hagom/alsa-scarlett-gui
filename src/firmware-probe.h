// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/* Firmware files of both generations begin with an 8-byte magic
 * string, and the two generations' magics are disjoint. The
 * generation owning a file is determined from that magic alone; file
 * names carry no meaning here.
 */
enum firmware_generation {
  FIRMWARE_GENERATION_UNKNOWN,
  FIRMWARE_GENERATION_SCARLETT2,
  FIRMWARE_GENERATION_SCARLETT4
};

/* Identify which generation's reader can handle fn.
 *
 * Returns FIRMWARE_GENERATION_UNKNOWN for a file that is unreadable,
 * shorter than the magic, or carries a magic belonging to neither
 * generation, reporting the reason to stderr.
 */
enum firmware_generation firmware_probe_generation(const char *fn);
