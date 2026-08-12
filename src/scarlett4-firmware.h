// SPDX-FileCopyrightText: 2023-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>

#include "alsa.h"

// Firmware directory for big Gen 4 devices
#define SCARLETT4_FIRMWARE_DIR "/usr/lib/firmware/scarlett4"

/* Number of components in a firmware version.
 *
 * Scarlett4 versions have four (e.g. 3.0.2778.0). Single-component
 * versions from earlier generations are held in the same form, in
 * element 0 with the remainder zero, so that one comparison serves
 * both.
 */
#define FIRMWARE_VERSION_PARTS 4

// Compare two firmware versions; returns -1, 0, or 1
int firmware_version_cmp(const uint32_t *v1, const uint32_t *v2);

/* Read a firmware version from an ALSA element into parts. Returns 0 if
 * the element is missing or does not hold one.
 */
int firmware_version_from_elem(struct alsa_elem *elem, uint32_t *parts);

enum scarlett4_firmware_type {
  SCARLETT4_FIRMWARE_CONTAINER,
  SCARLETT4_FIRMWARE_APP,
  SCARLETT4_FIRMWARE_ESP,
  SCARLETT4_FIRMWARE_LEAPFROG,
  SCARLETT4_FIRMWARE_TYPE_COUNT
};

extern const char *scarlett4_firmware_type_magic[SCARLETT4_FIRMWARE_TYPE_COUNT];

/* On-disk format of the firmware container header and the firmware
 * header. These are preceded by the 8-byte magic string identifying
 * the type of the following header.
 */
// All fields big-endian
struct scarlett4_firmware_container_header_disk {
  uint16_t usb_vid;
  uint16_t usb_pid;
  uint32_t firmware_version[FIRMWARE_VERSION_PARTS];
  uint32_t num_sections;
} __attribute__((packed));

// All fields big-endian
struct scarlett4_firmware_header_disk {
  uint16_t usb_vid;
  uint16_t usb_pid;
  uint32_t firmware_version[FIRMWARE_VERSION_PARTS];
  uint32_t firmware_length;
  uint8_t  sha256[32];
} __attribute__((packed));

/* In-memory representation of the firmware */
struct scarlett4_firmware {
  enum scarlett4_firmware_type type;
  uint16_t  usb_vid;
  uint16_t  usb_pid;
  uint32_t  firmware_version[FIRMWARE_VERSION_PARTS];
  uint32_t  firmware_length;
  uint8_t   sha256[32];
  uint8_t   md5[16];
  uint8_t  *firmware_data;
};

/* In-memory representation of the firmware container */
struct scarlett4_firmware_container {
  uint16_t                   usb_vid;
  uint16_t                   usb_pid;
  uint32_t                   firmware_version[FIRMWARE_VERSION_PARTS];
  uint32_t                   num_sections;
  struct scarlett4_firmware **sections;
};

/* Read just the firmware container header from a file */
struct scarlett4_firmware_container *scarlett4_read_firmware_header(const char *fn);

/* Read all sections of a firmware container from a file */
struct scarlett4_firmware_container *scarlett4_read_firmware_file(const char *fn);

void scarlett4_free_firmware_container(struct scarlett4_firmware_container *container);

const char *scarlett4_firmware_type_to_string(enum scarlett4_firmware_type type);

/* Record a firmware file in the firmware store; takes ownership of fn */
void scarlett4_store_firmware(char *fn);

/* Get the best firmware container for a device, or NULL if none */
struct scarlett4_firmware_container *scarlett4_get_best_firmware(uint32_t pid);

/* Check if device is in mid-upgrade state (leapfrog loaded, ESP needs update)
 * Returns 1 if mid-upgrade, 0 otherwise */
int scarlett4_is_mid_upgrade(struct alsa_card *card);
