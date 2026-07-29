// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <glib.h>
#include <stdint.h>

#ifdef HAVE_ONLINE_UPDATE

#include "scarlett4-firmware.h"

// One firmware entry from the online update index
struct online_firmware {
  uint16_t  usb_vid;
  uint16_t  usb_pid;
  char     *version;           // e.g. "2417" or "3.0.2778.0"
  uint32_t  version_parts[FIRMWARE_VERSION_PARTS];
  char     *url;
  char     *basename;          // download filename, validated from url
  char     *sha256;            // lowercase hex
  gsize     size;
  char    **min_kernel;        // NULL-terminated, or NULL
  char     *min_fcp_server;    // or NULL
};

// One client (program) entry from the online update index
struct online_client {
  char *name;
  char *version;
  char *url;
};

struct online_update_index {
  GHashTable *firmware;  // GINT_TO_POINTER(pid) -> struct online_firmware
  GList      *clients;   // struct online_client
};

// Called with the parsed index (to be freed with
// online_update_index_free()), or NULL and an error message
typedef void (*online_update_index_cb)(
  struct online_update_index *index,
  const char                 *error,
  gpointer                    user_data
);

// Called with the path of the downloaded and verified firmware
// file, or NULL and an error message
typedef void (*online_update_download_cb)(
  const char *fn,
  const char *error,
  gpointer    user_data
);

/* Fetch and parse the update index, caching it on success.
 *
 * The callback is invoked exactly once: from the main loop once the
 * request completes, or synchronously before this function returns if
 * the URL cannot be used at all. There is no way to cancel a request,
 * so user_data must remain valid until the callback runs; pass state
 * that outlives any window involved (and have the callback check
 * whether that window is still open).
 */
void online_update_fetch_index(
  online_update_index_cb callback,
  gpointer               user_data
);

/* The last successfully fetched index, or NULL if none is cached or it
 * cannot be read or parsed. Free with online_update_index_free().
 *
 * The cache is what makes an update notice survive between checks:
 * without it, a launch that the once-a-day gate stops from fetching
 * would have nothing to report and the notice would come and go with
 * the day boundary rather than with the availability of an update.
 */
struct online_update_index *online_update_load_cached_index(void);

/* Whether enough time has passed since the cached index was written to
 * check again. True when nothing is cached. A user-initiated check
 * should fetch without consulting this.
 */
gboolean online_update_check_is_due(void);

/* Download a firmware file into the cache directory, verifying its
 * size and SHA256 against the index entry before putting it in place.
 * The same callback contract as online_update_fetch_index() applies.
 */
void online_update_download(
  const struct online_firmware *firmware,
  online_update_download_cb     callback,
  gpointer                      user_data
);

void online_update_index_free(struct online_update_index *index);

/* Parse a version string's leading dotted numeric components (e.g.
 * "2403.3.g4a26.dirty" -> 2403.3) into a FIRMWARE_VERSION_PARTS
 * array; missing components are zero and components above UINT32_MAX
 * are clamped to it. Returns the number of components parsed (0 if
 * none).
 *
 * Zero-filling is what lets versions with fewer components than that
 * — a three-part kernel version, a single-part scarlett2 one — be
 * compared with firmware_version_cmp() like any other.
 */
int online_parse_version(const char *version, uint32_t *parts);

/* Returns NULL if the running kernel and fcp-server meet the
 * firmware's requirements, otherwise a newly allocated message
 * explaining the unmet requirement. fcp_server_version may be NULL
 * (meaning unknown/old, which fails any min_fcp_server requirement).
 */
char *online_firmware_requirement_error(
  const struct online_firmware *firmware,
  const char                   *fcp_server_version
);

#endif // HAVE_ONLINE_UPDATE
