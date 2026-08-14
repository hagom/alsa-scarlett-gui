// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

/* Exercises the already-downloaded check:
 *
 *   make test-downloaded && ./test-downloaded
 *
 * It decides whether a cached firmware file is the one the index
 * promised, and deletes it when it is not, so that a file which can
 * never be installed does not sit in the cache being skipped forever.
 */

#include <stdio.h>
#include <string.h>

#include <glib/gstdio.h>

#include "../online-update.h"

static int failures;
static char *cache_dir;

static void check(const char *desc, gboolean got, gboolean want) {
  gboolean ok = !got == !want;

  printf("%-46s %s\n", desc, ok ? "ok" : "FAIL");

  if (!ok)
    failures++;
}

// Put contents in the cache at the firmware's download path
static void write_cached(
  const struct online_firmware *firmware,
  const char                   *contents
) {
  char *fn = online_update_download_path(firmware);
  char *dir = g_path_get_dirname(fn);

  g_mkdir_with_parents(dir, 0755);
  g_file_set_contents(fn, contents, -1, NULL);

  g_free(dir);
  g_free(fn);
}

static gboolean cached_exists(const struct online_firmware *firmware) {
  char *fn = online_update_download_path(firmware);
  gboolean exists = g_file_test(fn, G_FILE_TEST_EXISTS);

  g_free(fn);

  return exists;
}

int main(void) {
  cache_dir = g_dir_make_tmp("asg-downloaded-XXXXXX", NULL);
  if (!cache_dir) {
    fprintf(stderr, "cannot create a temporary directory\n");
    return 2;
  }
  g_setenv("XDG_CACHE_HOME", cache_dir, TRUE);

  const char *good = "firmware contents";
  char *good_sha = g_compute_checksum_for_string(
    G_CHECKSUM_SHA256, good, -1
  );

  struct online_firmware firmware;

  memset(&firmware, 0, sizeof(firmware));
  firmware.basename = "test-firmware.bin";
  firmware.sha256 = good_sha;
  firmware.size = strlen(good);

  // Nothing cached
  check("absent", online_update_already_downloaded(&firmware), FALSE);

  // The file the index describes
  write_cached(&firmware, good);
  check("matching file", online_update_already_downloaded(&firmware), TRUE);
  check("matching file kept", cached_exists(&firmware), TRUE);

  /* Right size, wrong contents: the reader would reject it, so keeping
   * it would leave a download the status wants and this call refuses
   */
  write_cached(&firmware, "firmware contentX");
  check("same size, wrong contents",
        online_update_already_downloaded(&firmware), FALSE);
  check("wrong contents deleted", cached_exists(&firmware), FALSE);

  // Truncated: rejected on size before it is hashed
  write_cached(&firmware, "firmware");
  check("wrong size", online_update_already_downloaded(&firmware), FALSE);
  check("wrong size deleted", cached_exists(&firmware), FALSE);

  // A directory in the file's place is not a download, and not this
  // function's to remove
  char *fn = online_update_download_path(&firmware);
  g_mkdir(fn, 0755);
  check("directory in place of the file",
        online_update_already_downloaded(&firmware), FALSE);
  check("directory left alone", cached_exists(&firmware), TRUE);
  g_rmdir(fn);
  g_free(fn);

  g_free(good_sha);

  printf("\n%s\n", failures ? "FAILURES" : "all passed");
  printf("temporary directory left behind: %s\n", cache_dir);

  return failures ? 1 : 0;
}
