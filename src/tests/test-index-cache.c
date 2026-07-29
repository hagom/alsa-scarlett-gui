// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

/* Exercises the cached index against a temporary XDG_CACHE_HOME:
 *
 *   make test-index-cache && ./test-index-cache
 *
 * Covers loading a good index, the once-a-day gate including a
 * future mtime, and rejection of a corrupt or wrong-schema cache.
 */

#include <glib/gstdio.h>
#include <stdio.h>
#include <utime.h>

#include "../online-update.h"

static int failures;

static void check(const char *what, gboolean ok) {
  printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    failures++;
}

static char *cache_fn(void) {
  return g_build_filename(
    g_get_user_cache_dir(), "alsa-scarlett-gui", "index.json", NULL
  );
}

static void write_cache(const char *json) {
  char *fn = cache_fn();
  char *dir = g_path_get_dirname(fn);

  g_mkdir_with_parents(dir, 0755);

  GError *gerror = NULL;
  if (!g_file_set_contents(fn, json, -1, &gerror)) {
    fprintf(stderr, "cannot write %s: %s\n", fn, gerror->message);
    g_clear_error(&gerror);
    failures++;
  }

  g_free(dir);
  g_free(fn);
}

// Backdate the cache file by the given number of seconds
static void age_cache(gint64 seconds) {
  char *fn = cache_fn();
  GStatBuf st;

  if (g_stat(fn, &st) == 0) {
    struct utimbuf times = {
      .actime = st.st_atime, .modtime = st.st_mtime - seconds
    };
    if (utime(fn, &times) != 0)
      failures++;
  } else {
    failures++;
  }

  g_free(fn);
}

#define GOOD_JSON \
  "{\"schema\":1," \
  " \"clients\":{\"alsa-scarlett-gui\":{\"version\":\"9.9.9\"}}," \
  " \"firmware\":{\"1235:8218\":{" \
  "   \"version\":\"2417\"," \
  "   \"url\":\"https://example.invalid/scarlett2-1235-8218-2417.bin\"," \
  "   \"sha256\":" \
  "\"ab12345678901234567890123456789012345678901234567890123456789012\"," \
  "   \"size\":245760}}}"

static void test_no_cache(void) {
  check("no cache: load returns NULL", !online_update_load_cached_index());
  check("no cache: check is due", online_update_check_is_due());
}

static void test_good_cache(void) {
  write_cache(GOOD_JSON);

  struct online_update_index *index = online_update_load_cached_index();

  check("good cache: loads", index != NULL);

  if (index) {
    struct online_firmware *fw =
      g_hash_table_lookup(index->firmware, GINT_TO_POINTER(0x8218));

    check("good cache: firmware entry found", fw != NULL);
    check(
      "good cache: version parsed",
      fw && !strcmp(fw->version, "2417") && fw->version_parts[0] == 2417
    );
    check(
      "good cache: basename from url",
      fw && !strcmp(fw->basename, "scarlett2-1235-8218-2417.bin")
    );
    check("good cache: client listed", g_list_length(index->clients) == 1);

    online_update_index_free(index);
  }

  check("fresh cache: check not due", !online_update_check_is_due());

  age_cache(23 * 60 * 60);
  check("23h old cache: check not due", !online_update_check_is_due());

  age_cache(2 * 60 * 60);
  check("25h old cache: check is due", online_update_check_is_due());
}

// A clock change or a copied cache can leave the mtime ahead of now
static void test_future_mtime(void) {
  write_cache(GOOD_JSON);
  age_cache(-(48 * 60 * 60));

  check("future mtime: check is due", online_update_check_is_due());
}

static void test_bad_cache(void) {
  write_cache("{ this is not json");
  check("corrupt cache: load returns NULL",
        !online_update_load_cached_index());

  write_cache("{\"schema\":99}");
  check("wrong schema: load returns NULL",
        !online_update_load_cached_index());

  /* An index that parses but lists nothing is still a valid index:
   * it is what the server serves when no firmware is published.
   */
  write_cache("{\"schema\":1}");
  struct online_update_index *index = online_update_load_cached_index();
  check("empty index: loads", index != NULL);
  online_update_index_free(index);
}

int main(void) {
  char *tmp_dir = g_dir_make_tmp("asg-index-cache-XXXXXX", NULL);

  if (!tmp_dir) {
    fprintf(stderr, "cannot create a temporary directory\n");
    return 1;
  }

  g_setenv("XDG_CACHE_HOME", tmp_dir, TRUE);

  test_no_cache();
  test_good_cache();
  test_future_mtime();
  test_bad_cache();

  printf("\n%s\n", failures ? "FAILURES" : "all passed");
  printf("temporary directory left behind: %s\n", tmp_dir);

  g_free(tmp_dir);

  return failures ? 1 : 0;
}
