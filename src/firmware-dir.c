// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "firmware-dir.h"
#include "firmware-probe.h"
#include "scarlett2-firmware.h"
#include "scarlett4-firmware.h"

#define BIN_SUFFIX ".bin"

static int has_bin_suffix(const char *name) {
  size_t len = strlen(name);
  size_t suffix_len = strlen(BIN_SUFFIX);

  return len > suffix_len &&
         strcmp(name + len - suffix_len, BIN_SUFFIX) == 0;
}

static int is_regular_file(const struct dirent *entry, const char *full_fn) {
  if (entry->d_type == DT_REG)
    return 1;

  if (entry->d_type != DT_UNKNOWN)
    return 0;

  struct stat st;

  if (stat(full_fn, &st) < 0) {
    perror("stat");
    return 0;
  }

  return S_ISREG(st.st_mode);
}

// Hand the file to the generation its magic identifies
static void store_firmware(char *full_fn) {
  switch (firmware_probe_generation(full_fn)) {
    case FIRMWARE_GENERATION_SCARLETT2:
      scarlett2_store_firmware(full_fn);
      return;
    case FIRMWARE_GENERATION_SCARLETT4:
      scarlett4_store_firmware(full_fn);
      return;
    case FIRMWARE_GENERATION_UNKNOWN:
      g_free(full_fn);
      return;
  }
}

static void enum_dir(const char *dir_name) {
  DIR *dir = opendir(dir_name);

  if (!dir) {
    if (errno != ENOENT)
      fprintf(
        stderr, "Error opening directory %s: %s\n", dir_name, strerror(errno)
      );
    return;
  }

  struct dirent *entry;

  while ((entry = readdir(dir))) {
    if (!has_bin_suffix(entry->d_name))
      continue;

    char *full_fn = g_build_filename(dir_name, entry->d_name, NULL);

    if (!is_regular_file(entry, full_fn)) {
      g_free(full_fn);
      continue;
    }

    store_firmware(full_fn);
  }

  closedir(dir);
}

// The system firmware directory for a generation, overridable for testing
static const char *sys_dir(const char *env_name, const char *default_dir) {
  const char *dir = getenv(env_name);

  return dir ? dir : default_dir;
}

// The directory downloaded firmware is written to
static char *cache_dir(void) {
  return g_build_filename(
    g_get_user_cache_dir(), "alsa-scarlett-gui", "firmware", NULL
  );
}

void firmware_enum_all(void) {
  enum_dir(sys_dir("SCARLETT2_FIRMWARE_DIR", SCARLETT2_FIRMWARE_DIR));
  enum_dir(sys_dir("SCARLETT4_FIRMWARE_DIR", SCARLETT4_FIRMWARE_DIR));

  char *cache = cache_dir();

  enum_dir(cache);
  g_free(cache);
}
