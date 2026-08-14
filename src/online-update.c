// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HAVE_ONLINE_UPDATE

// for strverscmp(), which orders "beta2" before "beta10"
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "alsa.h"
#include "online-update.h"

#define INDEX_URL "https://fr.b4.vu/scarlett/v1/index.json"
#define TIMEOUT_SECONDS 10

// Leading hex characters of SHA256(serial) sent as the census's
// device identifier
#define SERIAL_HASH_CHARS 16

/* How stale the cached index must be before checking again. This is
 * also the census ping's frequency, the ping riding on the fetch.
 */
#define CHECK_INTERVAL_SECONDS (24 * 60 * 60)

// Response bodies are read into memory, so refuse anything larger
// than the biggest plausible firmware file
#define MAX_INDEX_SIZE     (1 * 1024 * 1024)
#define MAX_FIRMWARE_SIZE (32 * 1024 * 1024)

static SoupSession *get_session(void) {
  static SoupSession *session = NULL;

  if (!session)
    session = soup_session_new_with_options(
      "user-agent", "alsa-scarlett-gui/" VERSION " ",
      "timeout",    TIMEOUT_SECONDS,
      NULL
    );

  return session;
}

static const char *get_index_url(void) {
  const char *url = g_getenv("SCARLETT_UPDATE_INDEX_URL");

  return url ? url : INDEX_URL;
}

static void free_online_firmware(gpointer data) {
  struct online_firmware *firmware = data;

  g_free(firmware->version);
  g_free(firmware->url);
  g_free(firmware->basename);
  g_free(firmware->sha256);
  g_strfreev(firmware->min_kernel);
  g_free(firmware->min_fcp_server);
  g_free(firmware->min_gui);
  g_free(firmware);
}

static void free_online_client(gpointer data) {
  struct online_client *client = data;

  g_free(client->name);
  g_free(client->version);
  g_free(client->url);
  g_free(client);
}

void online_update_index_free(struct online_update_index *index) {
  if (!index)
    return;

  g_hash_table_destroy(index->firmware);
  g_list_free_full(index->clients, free_online_client);
  g_free(index);
}

/* Parse the leading dotted decimal components, stopping at the first
 * character that cannot continue a component. end_out, if given,
 * receives the first unconsumed character, so callers that require
 * the whole string to be a version can check for the terminator.
 */
static int parse_version_parts(
  const char  *version,
  uint32_t    *parts,
  int          count,
  const char **end_out
) {
  int i = 0;

  memset(parts, 0, count * sizeof(*parts));

  while (i < count) {
    // strtoul() accepts leading whitespace and a sign, neither of
    // which belongs in a version component
    if (!g_ascii_isdigit(*version))
      break;

    char *end;
    guint64 value = g_ascii_strtoull(version, &end, 10);

    parts[i++] = value > UINT32_MAX ? UINT32_MAX : value;

    version = end;

    if (*version != '.')
      break;

    // A trailing dot is not the start of another component
    if (!g_ascii_isdigit(version[1]))
      break;

    version++;
  }

  if (end_out)
    *end_out = version;

  return i;
}

int online_parse_version(const char *version, uint32_t *parts) {
  if (!version) {
    memset(parts, 0, FIRMWARE_VERSION_PARTS * sizeof(*parts));
    return 0;
  }

  return parse_version_parts(
    version, parts, FIRMWARE_VERSION_PARTS, NULL
  );
}

// Step over any further dotted numeric components
static const char *skip_version_parts(const char *p) {
  while (g_ascii_isdigit(*p)) {
    while (g_ascii_isdigit(*p))
      p++;

    if (*p != '.' || !g_ascii_isdigit(p[1]))
      break;

    p++;
  }

  return p;
}

/* Split what follows the numeric base into the pre-release tag and the
 * git-describe commit count. They rank in opposite directions: "beta9"
 * is older than the release it precedes, while "-55-g695e6c6" is 55
 * commits newer than the version it describes.
 *
 * Returns the tag, with commits_out receiving the count (0 when there
 * is none). Free the tag with g_free().
 */
static char *split_version_suffix(const char *suffix, guint64 *commits_out) {
  *commits_out = 0;

  /* git describe appends "-<count>-g<hash>". The last dash starts the
   * hash, so the count runs back from the dash before it, and whatever
   * precedes that is the tag.
   */
  const char *hash = strrchr(suffix, '-');

  if (!hash || hash[1] != 'g' || hash == suffix)
    return g_strdup(suffix);

  const char *count = hash - 1;

  while (count > suffix && g_ascii_isdigit(count[-1]))
    count--;

  if (count == hash || count[-1] != '-')
    return g_strdup(suffix);

  *commits_out = g_ascii_strtoull(count, NULL, 10);

  return g_strndup(suffix, count - 1 - suffix);
}

int online_client_version_cmp(const char *a, const char *b) {
  uint32_t a_parts[FIRMWARE_VERSION_PARTS];
  uint32_t b_parts[FIRMWARE_VERSION_PARTS];
  const char *a_rest, *b_rest;

  parse_version_parts(a, a_parts, FIRMWARE_VERSION_PARTS, &a_rest);
  parse_version_parts(b, b_parts, FIRMWARE_VERSION_PARTS, &b_rest);

  /* Numeric components past the fourth belong to the base. Left in
   * place they would look like a tag, making a longer version compare
   * as a pre-release of the prefix just compared. The parse stops
   * having consumed the separating dot, so the first of them is reached
   * directly.
   */
  a_rest = skip_version_parts(a_rest);
  b_rest = skip_version_parts(b_rest);

  int cmp = firmware_version_cmp(a_parts, b_parts);

  if (cmp)
    return cmp;

  guint64 a_commits, b_commits;
  char *a_tag = split_version_suffix(a_rest, &a_commits);
  char *b_tag = split_version_suffix(b_rest, &b_commits);

  // No tag means a release, which is newer than a pre-release of it
  if (!*a_tag != !*b_tag)
    cmp = *a_tag ? -1 : 1;
  else
    cmp = strverscmp(a_tag, b_tag);

  g_free(a_tag);
  g_free(b_tag);

  if (cmp)
    return cmp;

  // Same tag, so compare the commits since it
  if (a_commits != b_commits)
    return a_commits < b_commits ? -1 : 1;

  return 0;
}

/* Parse a version that must be nothing but dotted decimals, such as
 * "2417" or "3.0.2778.0". Returns 0 if it has trailing junk, more
 * components than fit, or no components at all.
 */
static int parse_strict_version(const char *version, uint32_t *parts) {
  const char *end;
  int count = parse_version_parts(
    version, parts, FIRMWARE_VERSION_PARTS, &end
  );

  if (!count || *end)
    return 0;

  return count;
}

/* Check the running kernel against the firmware's min_kernel list:
 * a branch (major.minor) listed there requires at least that entry,
 * and unlisted branches require at least the last (highest) entry.
 */
static char *check_min_kernel(char **min_kernel) {
  struct utsname uts;
  uint32_t kernel[FIRMWARE_VERSION_PARTS];

  if (uname(&uts) < 0 ||
      online_parse_version(uts.release, kernel) < 2)
    return g_strdup("Cannot determine the running kernel version");

  const char *last = NULL;
  for (char **entry = min_kernel; *entry; entry++) {
    uint32_t min[FIRMWARE_VERSION_PARTS];
    if (online_parse_version(*entry, min) < 2)
      continue;

    last = *entry;

    // Same branch?
    if (kernel[0] == min[0] && kernel[1] == min[1]) {
      if (firmware_version_cmp(kernel, min) >= 0)
        return NULL;
      return g_strdup_printf(
        "This firmware requires kernel %s or later", *entry
      );
    }
  }

  if (!last)
    return NULL;

  uint32_t min[FIRMWARE_VERSION_PARTS];
  online_parse_version(last, min);

  if (firmware_version_cmp(kernel, min) >= 0)
    return NULL;

  return g_strdup_printf(
    "This firmware requires kernel %s or later", last
  );
}

char *online_firmware_requirement_error(
  const struct online_firmware *firmware,
  const char                   *fcp_server_version
) {
  if (firmware->min_kernel && *firmware->min_kernel) {
    char *error = check_min_kernel(firmware->min_kernel);
    if (error)
      return error;
  }

  if (firmware->min_fcp_server) {
    uint32_t min[FIRMWARE_VERSION_PARTS];

    /* An unparseable requirement cannot be checked against anything,
     * so treat it as no requirement rather than blocking every
     * device on a malformed index entry
     */
    if (!online_parse_version(firmware->min_fcp_server, min))
      return NULL;

    /* Compared the way client versions are, so that a pre-release of
     * the required version does not satisfy it. A card reporting no
     * version is served by a server predating the control, which is
     * older than anything a requirement can name.
     */
    if (!fcp_server_version ||
        online_client_version_cmp(
          fcp_server_version, firmware->min_fcp_server
        ) < 0)
      return g_strdup_printf(
        "This firmware requires fcp-server %s or later",
        firmware->min_fcp_server
      );
  }

  /* No firmware has required a newer version of this app, but the check
   * has to be shipped before the firmware that needs it: an index field
   * a released client does not read cannot be applied to it later.
   */
  if (firmware->min_gui) {
    uint32_t min[FIRMWARE_VERSION_PARTS];

    // As above, a requirement that cannot be parsed is not one
    if (!online_parse_version(firmware->min_gui, min))
      return NULL;

    if (online_client_version_cmp(VERSION, firmware->min_gui) < 0)
      return g_strdup_printf(
        "This firmware requires alsa-scarlett-gui %s or later",
        firmware->min_gui
      );
  }

  return NULL;
}

/* Typed member accessors: json-glib's own get_*_member() log a
 * critical and return a default when the member is absent or holds
 * another type, which untrusted input must not be able to provoke.
 */
static JsonNode *get_member(
  JsonObject *obj,
  const char *name,
  GType       value_type
) {
  JsonNode *node = json_object_get_member(obj, name);

  if (!node ||
      !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != value_type)
    return NULL;

  return node;
}

static const char *get_string_member(JsonObject *obj, const char *name) {
  JsonNode *node = get_member(obj, name, G_TYPE_STRING);

  return node ? json_node_get_string(node) : NULL;
}

// Returns fallback if the member is missing or not an integer
static gint64 get_int_member(
  JsonObject *obj,
  const char *name,
  gint64      fallback
) {
  JsonNode *node = get_member(obj, name, G_TYPE_INT64);

  return node ? json_node_get_int(node) : fallback;
}

static JsonObject *get_object_member(JsonObject *obj, const char *name) {
  JsonNode *node = json_object_get_member(obj, name);

  if (!node || !JSON_NODE_HOLDS_OBJECT(node))
    return NULL;

  return json_node_get_object(node);
}

static JsonArray *get_array_member(JsonObject *obj, const char *name) {
  JsonNode *node = json_object_get_member(obj, name);

  if (!node || !JSON_NODE_HOLDS_ARRAY(node))
    return NULL;

  return json_node_get_array(node);
}

// Collect the string elements of an array, skipping any that aren't
// strings, so a bad element cannot truncate the list
static char **parse_min_kernel(JsonObject *obj) {
  JsonArray *array = get_array_member(obj, "min_kernel");

  if (!array)
    return NULL;

  guint length = json_array_get_length(array);
  char **min_kernel = g_new0(char *, length + 1);
  guint n = 0;

  for (guint i = 0; i < length; i++) {
    JsonNode *node = json_array_get_element(array, i);

    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
      continue;

    min_kernel[n++] = g_strdup(json_node_get_string(node));
  }

  if (!n) {
    g_strfreev(min_kernel);
    return NULL;
  }

  return min_kernel;
}

// Reject anything that is not plain lowercase hex of the right length
static gboolean is_valid_sha256(const char *sha256) {
  if (strlen(sha256) != 64)
    return FALSE;

  for (const char *p = sha256; *p; p++)
    if (!g_ascii_isdigit(*p) && !(*p >= 'a' && *p <= 'f'))
      return FALSE;

  return TRUE;
}

/* The filename is taken from the URL in a downloaded index, so it
 * must not be able to escape the download directory. Use the last
 * segment of the parsed (and therefore normalised and percent-decoded)
 * path, and reject anything that is not a plain filename.
 */
static char *get_download_basename(const char *url) {
  GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);

  if (!uri)
    return NULL;

  const char *path = g_uri_get_path(uri);
  const char *slash = strrchr(path, '/');
  char *basename = g_strdup(slash ? slash + 1 : path);

  g_uri_unref(uri);

  if (!*basename ||
      !strcmp(basename, ".") ||
      !strcmp(basename, "..") ||
      strchr(basename, '/') ||
      g_path_is_absolute(basename)) {
    g_free(basename);
    return NULL;
  }

  return basename;
}

static struct online_firmware *parse_firmware_entry(
  const char *key,
  JsonObject *obj
) {
  unsigned int vid, pid;
  char extra;
  if (sscanf(key, "%4x:%4x%c", &vid, &pid, &extra) != 2 ||
      vid > UINT16_MAX || pid > UINT16_MAX) {
    fprintf(stderr, "Update index: invalid device key %s\n", key);
    return NULL;
  }

  const char *version = get_string_member(obj, "version");
  const char *url = get_string_member(obj, "url");
  const char *sha256 = get_string_member(obj, "sha256");
  gint64 size = get_int_member(obj, "size", 0);

  if (!version || !url || !sha256 ||
      size <= 0 || size > MAX_FIRMWARE_SIZE) {
    fprintf(stderr, "Update index: incomplete entry for %s\n", key);
    return NULL;
  }

  if (!is_valid_sha256(sha256)) {
    fprintf(stderr, "Update index: invalid sha256 for %s\n", key);
    return NULL;
  }

  uint32_t parts[FIRMWARE_VERSION_PARTS];
  if (!parse_strict_version(version, parts)) {
    fprintf(
      stderr, "Update index: invalid version %s for %s\n", version, key
    );
    return NULL;
  }

  /* Must be the last check: this allocates, and ownership only passes
   * to the entry below, so an earlier return added after it would leak
   */
  char *basename = get_download_basename(url);
  if (!basename) {
    fprintf(stderr, "Update index: unusable url %s for %s\n", url, key);
    return NULL;
  }

  struct online_firmware *firmware = g_new0(struct online_firmware, 1);

  firmware->usb_vid = vid;
  firmware->usb_pid = pid;
  firmware->version = g_strdup(version);
  memcpy(firmware->version_parts, parts, sizeof(parts));
  firmware->url = g_strdup(url);
  firmware->basename = basename;
  firmware->sha256 = g_strdup(sha256);
  firmware->size = size;
  firmware->min_kernel = parse_min_kernel(obj);
  firmware->min_fcp_server = g_strdup(
    get_string_member(obj, "min_fcp_server")
  );
  firmware->min_gui = g_strdup(get_string_member(obj, "min_gui"));

  return firmware;
}

static void parse_firmware(
  struct online_update_index *index,
  JsonObject                 *root_obj
) {
  JsonObject *firmware_obj = get_object_member(root_obj, "firmware");

  if (!firmware_obj)
    return;

  JsonObjectIter iter;
  const char *key;
  JsonNode *node;

  json_object_iter_init(&iter, firmware_obj);
  while (json_object_iter_next(&iter, &key, &node)) {
    if (!JSON_NODE_HOLDS_OBJECT(node))
      continue;

    struct online_firmware *firmware =
      parse_firmware_entry(key, json_node_get_object(node));
    if (!firmware)
      continue;

    // A duplicate product ID replaces the earlier entry
    g_hash_table_insert(
      index->firmware, GINT_TO_POINTER(firmware->usb_pid), firmware
    );
  }
}

static void parse_clients(
  struct online_update_index *index,
  JsonObject                 *root_obj
) {
  JsonObject *clients_obj = get_object_member(root_obj, "clients");

  if (!clients_obj)
    return;

  JsonObjectIter iter;
  const char *key;
  JsonNode *node;

  json_object_iter_init(&iter, clients_obj);
  while (json_object_iter_next(&iter, &key, &node)) {
    if (!JSON_NODE_HOLDS_OBJECT(node))
      continue;

    JsonObject *obj = json_node_get_object(node);
    const char *version = get_string_member(obj, "version");

    if (!version) {
      fprintf(stderr, "Update index: no version for client %s\n", key);
      continue;
    }

    struct online_client *client = g_new0(struct online_client, 1);

    client->name = g_strdup(key);
    client->version = g_strdup(version);
    client->url = g_strdup(get_string_member(obj, "url"));
    index->clients = g_list_append(index->clients, client);
  }
}

static struct online_update_index *parse_index(
  GBytes      *bytes,
  const char **error
) {
  JsonParser *parser = json_parser_new();
  struct online_update_index *index = NULL;

  gsize length;
  const char *data = g_bytes_get_data(bytes, &length);

  if (!data || !json_parser_load_from_data(parser, data, length, NULL)) {
    *error = "Cannot parse update index";
    goto done;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    *error = "Update index is not a JSON object";
    goto done;
  }
  JsonObject *root_obj = json_node_get_object(root);

  if (get_int_member(root_obj, "schema", 0) != 1) {
    *error = "Unsupported update index schema";
    goto done;
  }

  index = g_new0(struct online_update_index, 1);
  index->firmware = g_hash_table_new_full(
    g_direct_hash, g_direct_equal, NULL, free_online_firmware
  );

  parse_firmware(index, root_obj);
  parse_clients(index, root_obj);

done:
  g_object_unref(parser);
  return index;
}

/* The fetched index is cached so that a launch which the once-a-day
 * gate stops from fetching still knows what the last check found, and
 * so the file's mtime can serve as the record of when that was. A
 * separate timestamp file could say "checked" with no index beside it.
 */
static char *cached_index_fn(void) {
  return g_build_filename(
    g_get_user_cache_dir(), "alsa-scarlett-gui", "index.json", NULL
  );
}

/* Written only after the response parses, so a server returning
 * something unusable leaves the previous index in place instead of
 * replacing it with something that yields no update notice.
 */
static void save_cached_index(GBytes *bytes) {
  char *fn = cached_index_fn();
  char *dir = g_path_get_dirname(fn);
  GError *gerror = NULL;

  if (g_mkdir_with_parents(dir, 0755) != 0) {
    fprintf(stderr, "Cannot create %s: %s\n", dir, strerror(errno));
    goto done;
  }

  gsize length;
  const char *data = g_bytes_get_data(bytes, &length);

  if (!g_file_set_contents(fn, data, length, &gerror)) {
    fprintf(stderr, "Cannot write %s: %s\n", fn, gerror->message);
    g_clear_error(&gerror);
  }

done:
  g_free(dir);
  g_free(fn);
}

struct online_update_index *online_update_load_cached_index(void) {
  char *fn = cached_index_fn();
  char *data = NULL;
  gsize length;

  if (!g_file_get_contents(fn, &data, &length, NULL)) {
    g_free(fn);
    return NULL;
  }

  GBytes *bytes = g_bytes_new_take(data, length);
  const char *error = NULL;
  struct online_update_index *index = parse_index(bytes, &error);

  g_bytes_unref(bytes);

  if (index && error) {
    online_update_index_free(index);
    index = NULL;
  }

  if (!index)
    fprintf(
      stderr, "Ignoring cached update index %s: %s\n",
      fn, error ? error : "cannot parse"
    );

  g_free(fn);

  return index;
}

gboolean online_update_check_is_due(void) {
  char *fn = cached_index_fn();
  GStatBuf st;
  gboolean due = TRUE;

  if (g_stat(fn, &st) == 0) {
    gint64 age = g_get_real_time() / G_USEC_PER_SEC - st.st_mtime;

    /* A negative age means the mtime is in the future, which a clock
     * change or a copied cache can produce; treat it as due rather
     * than as a check that never expires.
     */
    due = age < 0 || age >= CHECK_INTERVAL_SECONDS;
  }

  g_free(fn);

  return due;
}

struct request;

/* Called with the response body, or with NULL and an error message
 * derived from the transfer, the HTTP status, or the size limit
 */
typedef void (*request_done_cb)(
  struct request *req,
  GBytes         *bytes,
  const char     *error
);

/* Reports bytes transferred so far. total is the expected size, or 0
 * when the server did not say.
 */
typedef void (*request_progress_cb)(
  gsize    received,
  gsize    total,
  gpointer user_data
);

// How much of the body to ask for at a time when streaming
#define READ_CHUNK_SIZE (64 * 1024)

/* A GET request whose body is read into memory. done() is called
 * exactly once; either way the request is freed afterwards.
 *
 * The body is read either in one call, or — when progress is wanted — a
 * chunk at a time into buffer, which is why the size cap is enforced as
 * it grows as well as from the headers.
 */
struct request {
  SoupMessage         *msg;
  GCancellable        *cancellable;
  gsize                max_size;
  gboolean             too_large;
  request_done_cb      done;
  gpointer             callback;
  gpointer             user_data;

  // streaming reads only
  GInputStream        *stream;
  GByteArray          *buffer;
  gsize                expected_size;
  request_progress_cb  progress;
};

static void request_free(struct request *req) {
  if (req->stream)
    g_object_unref(req->stream);
  if (req->buffer)
    g_byte_array_unref(req->buffer);

  g_object_unref(req->cancellable);
  g_object_unref(req->msg);
  g_free(req);
}

/* Hand the body to done() if the transfer and the HTTP status allow it,
 * then free the request. bytes is NULL when the transfer itself failed.
 */
static void request_complete(
  struct request *req,
  GBytes         *bytes,
  GError         *gerror
) {
  if (!bytes) {
    const char *error = "Request failed";

    if (req->too_large)
      error = "Response is too large";
    else if (gerror)
      error = gerror->message;

    req->done(req, NULL, error);
  } else {
    SoupStatus status = soup_message_get_status(req->msg);

    if (status != SOUP_STATUS_OK) {
      char *error = g_strdup_printf(
        "Server returned %u %s", status, soup_status_get_phrase(status)
      );
      req->done(req, NULL, error);
      g_free(error);
    } else if (g_bytes_get_size(bytes) > req->max_size) {
      req->done(req, NULL, "Response is too large");
    } else {
      req->done(req, bytes, NULL);
    }
  }

  request_free(req);
}

static void request_finished(
  GObject      *source,
  GAsyncResult *result,
  gpointer      user_data
) {
  struct request *req = user_data;
  GError *gerror = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(
    SOUP_SESSION(source), result, &gerror
  );

  request_complete(req, bytes, gerror);

  g_clear_error(&gerror);

  if (bytes)
    g_bytes_unref(bytes);
}

static void request_read_chunk(struct request *req);

static void request_chunk_read(
  GObject      *source,
  GAsyncResult *result,
  gpointer      user_data
) {
  struct request *req = user_data;
  GError *gerror = NULL;

  GBytes *chunk = g_input_stream_read_bytes_finish(
    G_INPUT_STREAM(source), result, &gerror
  );

  if (!chunk) {
    request_complete(req, NULL, gerror);
    g_clear_error(&gerror);
    return;
  }

  gsize length = g_bytes_get_size(chunk);

  // A zero-length read is the end of the body
  if (!length) {
    g_bytes_unref(chunk);

    GBytes *bytes = g_bytes_new(req->buffer->data, req->buffer->len);

    request_complete(req, bytes, NULL);
    g_bytes_unref(bytes);
    return;
  }

  /* Enforce the cap as the body grows: a server that understates or
   * omits Content-Length must not be able to make this read for ever.
   */
  if (req->buffer->len + length > req->max_size) {
    g_bytes_unref(chunk);
    req->too_large = TRUE;
    request_complete(req, NULL, NULL);
    return;
  }

  g_byte_array_append(
    req->buffer, g_bytes_get_data(chunk, NULL), length
  );
  g_bytes_unref(chunk);

  if (req->progress)
    req->progress(req->buffer->len, req->expected_size, req->user_data);

  request_read_chunk(req);
}

static void request_read_chunk(struct request *req) {
  g_input_stream_read_bytes_async(
    req->stream, READ_CHUNK_SIZE, G_PRIORITY_DEFAULT, req->cancellable,
    request_chunk_read, req
  );
}

// The response headers have arrived; read the body a chunk at a time
static void request_sent(
  GObject      *source,
  GAsyncResult *result,
  gpointer      user_data
) {
  struct request *req = user_data;
  GError *gerror = NULL;

  req->stream = soup_session_send_finish(
    SOUP_SESSION(source), result, &gerror
  );

  if (!req->stream) {
    request_complete(req, NULL, gerror);
    g_clear_error(&gerror);
    return;
  }

  /* A non-OK status has no body worth reading, and request_complete()
   * turns it into the message to present.
   */
  if (soup_message_get_status(req->msg) != SOUP_STATUS_OK) {
    GBytes *empty = g_bytes_new(NULL, 0);

    request_complete(req, empty, NULL);
    g_bytes_unref(empty);
    return;
  }

  goffset length = soup_message_headers_get_content_length(
    soup_message_get_response_headers(req->msg)
  );

  req->expected_size = length > 0 ? (gsize)length : 0;
  req->buffer = g_byte_array_new();

  if (req->progress)
    req->progress(0, req->expected_size, req->user_data);

  request_read_chunk(req);
}

/* Refuse an oversized body before reading it into memory. The length
 * check in request_finished() catches a server that lies about or
 * omits Content-Length.
 */
static void request_got_headers(SoupMessage *msg, gpointer user_data) {
  struct request *req = user_data;
  goffset length = soup_message_headers_get_content_length(
    soup_message_get_response_headers(msg)
  );

  if (length > 0 && (gsize)length > req->max_size) {
    req->too_large = TRUE;
    g_cancellable_cancel(req->cancellable);
  }
}

/* Returns FALSE without calling done() if the URL is unusable.
 *
 * With a progress callback the body is read a chunk at a time so that
 * it can be reported; without one it is read in a single call.
 */
static gboolean request_start(
  const char          *url,
  gsize                max_size,
  request_done_cb      done,
  gpointer             callback,
  gpointer             user_data,
  request_progress_cb  progress
) {
  SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url);

  if (!msg)
    return FALSE;

  struct request *req = g_new0(struct request, 1);

  req->msg = msg;
  req->cancellable = g_cancellable_new();
  req->max_size = max_size;
  req->done = done;
  req->callback = callback;
  req->user_data = user_data;
  req->progress = progress;

  g_signal_connect(
    msg, "got-headers", G_CALLBACK(request_got_headers), req
  );

  if (progress)
    soup_session_send_async(
      get_session(), msg, G_PRIORITY_DEFAULT, req->cancellable,
      request_sent, req
    );
  else
    soup_session_send_and_read_async(
      get_session(), msg, G_PRIORITY_DEFAULT, req->cancellable,
      request_finished, req
    );

  return TRUE;
}

static void fetch_index_done(
  struct request *req,
  GBytes         *bytes,
  const char     *error
) {
  online_update_index_cb callback = req->callback;

  if (!bytes) {
    callback(NULL, error, req->user_data);
    return;
  }

  struct online_update_index *index = parse_index(bytes, &error);

  if (!index || error) {
    online_update_index_free(index);
    callback(NULL, error, req->user_data);
  } else {
    save_cached_index(bytes);
    callback(index, NULL, req->user_data);
  }
}

/* Append "&key=value" with the value percent-encoded. Every value goes
 * through here, including ones we generate ourselves: most come from
 * the system (uname, /etc/os-release, the build's VERSION) and could
 * otherwise inject a separator and forge extra parameters. A rule
 * about which fields are safe to skip would not survive maintenance.
 *
 * allowed lists characters to leave literal, or is NULL for none.
 */
static void append_param_allowing(
  GString    *query,
  const char *key,
  const char *value,
  const char *allowed
) {
  char *escaped = g_uri_escape_string(value, allowed, FALSE);

  g_string_append_printf(query, "&%s=%s", key, escaped);
  g_free(escaped);
}

static void append_param(
  GString    *query,
  const char *key,
  const char *value
) {
  append_param_allowing(query, key, value, NULL);
}

/* Read one KEY=value from /etc/os-release.
 *
 * That file is shell syntax, not INI, so GKeyFile cannot read it: it
 * hands back values with their quotes still attached, turning
 * VERSION_ID="12" into os=debian-%2212%22. The mistake hides easily,
 * because distributions that leave ID and VERSION_ID unquoted (Fedora
 * among them) parse correctly and make GKeyFile look adequate.
 *
 * Only the subset needed here is handled: a single-line value, either
 * bare or wholly quoted, with no escape sequences — which is what ID
 * and VERSION_ID are in practice.
 */
static char *get_os_release_value(const char *data, const char *key) {
  char  *prefix = g_strconcat(key, "=", NULL);
  char **lines = g_strsplit(data, "\n", -1);
  char  *value = NULL;

  for (int i = 0; lines[i] && !value; i++) {
    if (!g_str_has_prefix(lines[i], prefix))
      continue;

    char *raw = g_strstrip(g_strdup(lines[i] + strlen(prefix)));
    gsize len = strlen(raw);

    if (len >= 2 &&
        (raw[0] == '"' || raw[0] == '\'') &&
        raw[len - 1] == raw[0]) {
      value = g_strndup(raw + 1, len - 2);
      g_free(raw);
    } else {
      value = raw;
    }

    // A key present but empty is no more useful than one absent
    if (!*value) {
      g_free(value);
      value = NULL;
      break;
    }
  }

  g_strfreev(lines);
  g_free(prefix);

  return value;
}

/* "os" is sent only when both components are known. Half a record
 * (fedora- or -44) would pollute the distro tallies, whereas an
 * absent parameter unambiguously means "not determinable". Rolling
 * releases such as Arch legitimately have no VERSION_ID, so this is a
 * normal outcome and must not be logged.
 */
static void append_os_param(GString *query) {
  char *data = NULL;

  if (!g_file_get_contents("/etc/os-release", &data, NULL, NULL))
    return;

  char *id = get_os_release_value(data, "ID");
  char *version_id = get_os_release_value(data, "VERSION_ID");

  if (id && version_id) {
    char *os = g_strconcat(id, "-", version_id, NULL);

    append_param(query, "os", os);
    g_free(os);
  }

  g_free(version_id);
  g_free(id);
  g_free(data);
}

/* One "dev" parameter per connected device, as pid:version:hash.
 *
 * The hash is the first 16 hex characters of SHA256(serial): raw
 * serials never leave the machine, and a truncated hash is ample to
 * deduplicate reports and follow one device over time.
 *
 * Devices with no readable serial are skipped rather than sent with a
 * placeholder, which would collide every such device into a single
 * tally that reads as one hyperactive install; losing them is a known
 * undercount instead. Simulated cards are not connected devices.
 *
 * The ":" separators are left literal. A colon carries no meaning in a
 * query string, so escaping it would guard against nothing, and the
 * consent dialog and FAQ both show this request line to the user for
 * them to read: "821e%3A3.0.2761.0%3A..." defeats that purpose.
 */
static void append_dev_params(GString *query) {
  GArray *cards = alsa_get_cards();

  if (!cards)
    return;

  for (guint i = 0; i < cards->len; i++) {
    struct alsa_card *card = g_array_index(cards, struct alsa_card *, i);

    if (!card || card->num == SIMULATED_CARD_NUM || !card->serial)
      continue;

    char *version = card_firmware_version(card);

    if (!version)
      continue;

    char *hash = g_compute_checksum_for_string(
      G_CHECKSUM_SHA256, card->serial, -1
    );
    char *dev = g_strdup_printf(
      "%04x:%s:%.*s", card->pid, version, SERIAL_HASH_CHARS, hash
    );

    append_param_allowing(query, "dev", dev, ":");

    g_free(dev);
    g_free(hash);
    g_free(version);
  }
}

char *online_update_index_url(gboolean include_usage_info) {
  GString *url = g_string_new(get_index_url());

  g_string_append(url, "?prog=alsa-scarlett-gui");
  append_param(url, "ver", VERSION);

  if (include_usage_info) {
    struct utsname un;

    if (uname(&un) == 0)
      append_param(url, "kernel", un.release);

    append_os_param(url);
    append_dev_params(url);
  }

  return g_string_free(url, FALSE);
}

void online_update_fetch_index(
  gboolean               include_usage_info,
  online_update_index_cb callback,
  gpointer               user_data
) {
  char *url = online_update_index_url(include_usage_info);
  gboolean started = request_start(
    url, MAX_INDEX_SIZE, fetch_index_done, callback, user_data, NULL
  );
  g_free(url);

  if (!started)
    callback(NULL, "Invalid update index URL", user_data);
}

struct download {
  char                       *sha256;
  gsize                       size;
  char                       *fn;
  gpointer                    user_data;  // the caller's, not the request's
  online_update_progress_cb   progress;
};

/* The request layer reports raw byte counts; the caller wants them
 * against the size the index promised, which is the figure the
 * verification will hold the transfer to.
 */
static void download_progress(
  gsize    received,
  gsize    total,
  gpointer user_data
) {
  struct download *dl = user_data;

  if (dl->progress)
    dl->progress(received, dl->size ? dl->size : total, dl->user_data);
}

/* Write the verified data to a temporary file in the same directory
 * and rename it into place, so a partial or interrupted write can
 * never be mistaken for a complete download
 */
static const char *save_download(
  struct download *dl,
  gconstpointer    data,
  gsize            length
) {
  char *dir = g_path_get_dirname(dl->fn);
  int ok = g_mkdir_with_parents(dir, 0755) == 0;
  g_free(dir);

  if (!ok)
    return "Cannot create the firmware download directory";

  char *tmp_fn = g_strdup_printf("%s.partial", dl->fn);
  GError *gerror = NULL;
  const char *error = NULL;

  if (!g_file_set_contents(tmp_fn, data, length, &gerror)) {
    error = "Cannot write the downloaded firmware";
    fprintf(stderr, "Cannot write %s: %s\n", tmp_fn, gerror->message);
    g_clear_error(&gerror);
  } else if (rename(tmp_fn, dl->fn) != 0) {
    error = "Cannot save the downloaded firmware";
    fprintf(
      stderr, "Cannot rename %s: %s\n", tmp_fn, strerror(errno)
    );
    g_unlink(tmp_fn);
  }

  g_free(tmp_fn);

  return error;
}

static void download_done(
  struct request *req,
  GBytes         *bytes,
  const char     *error
) {
  online_update_download_cb callback = req->callback;
  struct download *dl = req->user_data;

  if (bytes) {
    gsize length;
    gconstpointer data = g_bytes_get_data(bytes, &length);

    if (length != dl->size) {
      error = "Downloaded firmware has the wrong size";
    } else {
      char *sha256 = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, data, length
      );
      gboolean sha256_ok = !strcmp(sha256, dl->sha256);
      g_free(sha256);

      error = sha256_ok
        ? save_download(dl, data, length)
        : "Downloaded firmware failed its checksum";
    }
  }

  callback(error ? NULL : dl->fn, error, dl->user_data);

  g_free(dl->sha256);
  g_free(dl->fn);
  g_free(dl);
}

char *online_update_download_path(const struct online_firmware *firmware) {
  // parse_firmware_entry() rejects entries without a usable basename
  return g_build_filename(
    g_get_user_cache_dir(), "alsa-scarlett-gui", "firmware",
    firmware->basename, NULL
  );
}

gboolean online_update_already_downloaded(
  const struct online_firmware *firmware
) {
  char *fn = online_update_download_path(firmware);
  GStatBuf st;
  gboolean present = FALSE;

  /* Anything here that is not what the index describes is deleted, so
   * that a file which can never be installed does not sit in the cache
   * being skipped. Otherwise the firmware reader rejects it, it never
   * becomes the best local version, and the status keeps asking for a
   * download that this function keeps declining.
   *
   * The size is the cheap test and the checksum is the one that decides;
   * a partial download fails the first, a corrupt one the second.
   *
   * Reached only when a download is wanted and a file is already there,
   * which does not happen once one has arrived intact.
   */
  if (g_stat(fn, &st) == 0 && S_ISREG(st.st_mode)) {
    char *contents;
    gsize length;

    if ((gsize)st.st_size == firmware->size &&
        g_file_get_contents(fn, &contents, &length, NULL)) {
      char *sha256 = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, (const guchar *)contents, length
      );

      present = !strcmp(sha256, firmware->sha256);

      g_free(sha256);
      g_free(contents);
    }

    if (!present)
      g_unlink(fn);
  }

  g_free(fn);

  return present;
}

void online_update_download(
  const struct online_firmware *firmware,
  online_update_download_cb     callback,
  gpointer                      user_data,
  online_update_progress_cb     progress
) {
  struct download *dl = g_new0(struct download, 1);

  dl->sha256 = g_strdup(firmware->sha256);
  dl->size = firmware->size;
  dl->user_data = user_data;
  dl->progress = progress;
  dl->fn = online_update_download_path(firmware);

  if (!request_start(
        firmware->url, firmware->size, download_done, callback, dl,
        download_progress
      )) {
    g_free(dl->sha256);
    g_free(dl->fn);
    g_free(dl);
    callback(NULL, "Invalid firmware download URL", user_data);
  }
}

#endif // HAVE_ONLINE_UPDATE
