// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <string.h>

#include "firmware-probe.h"
#include "scarlett2-firmware.h"
#include "scarlett4-firmware.h"

#define MAGIC_SIZE 8

enum firmware_generation firmware_probe_generation(const char *fn) {
  FILE *file = fopen(fn, "rb");

  if (!file) {
    perror("fopen firmware");
    fprintf(stderr, "Unable to open %s\n", fn);
    return FIRMWARE_GENERATION_UNKNOWN;
  }

  char   magic[MAGIC_SIZE];
  size_t read_count = fread(magic, MAGIC_SIZE, 1, file);
  int    read_error = ferror(file);

  fclose(file);

  if (read_count != 1) {
    if (read_error)
      fprintf(stderr, "Error reading magic from %s\n", fn);
    else
      fprintf(stderr, "%s is too short to be a firmware file\n", fn);
    return FIRMWARE_GENERATION_UNKNOWN;
  }

  if (memcmp(magic, MAGIC_STRING, MAGIC_SIZE) == 0)
    return FIRMWARE_GENERATION_SCARLETT2;

  for (int i = 0; i < SCARLETT4_FIRMWARE_TYPE_COUNT; i++)
    if (memcmp(magic, scarlett4_firmware_type_magic[i], MAGIC_SIZE) == 0)
      return FIRMWARE_GENERATION_SCARLETT4;

  fprintf(stderr, "%s is not a Scarlett firmware file\n", fn);
  return FIRMWARE_GENERATION_UNKNOWN;
}
