// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HAVE_ONLINE_UPDATE

#include <stdio.h>
#include <string.h>

#include "alsa.h"
#include "app-settings.h"
#include "debug.h"
#include "firmware-dir.h"
#include "firmware-probe.h"
#include "firmware-store.h"
#include "online-check.h"
#include "online-update.h"
#include "scarlett4-firmware.h"

/* The index every comparison reads, owned by this file. Loaded from the
 * cache on first use and replaced when a fetch completes.
 *
 * Held rather than loaded per comparison because each card's status
 * borrows an entry from it, which is what lets a download start without
 * parsing the index again.
 */
static struct online_update_index *held_index;
static gboolean index_load_attempted;

static GSList *status_changed_callbacks;

void online_check_add_status_changed_callback(
  void (*callback)(struct alsa_card *card)
) {
  status_changed_callbacks = g_slist_prepend(
    status_changed_callbacks, callback
  );
}

static void status_changed(struct alsa_card *card) {
  for (GSList *l = status_changed_callbacks; l; l = l->next) {
    void (*callback)(struct alsa_card *) = l->data;

    callback(card);
  }
}

static void free_status(struct card_update_status *status) {
  if (!status)
    return;

  g_free(status->offered_version);
  g_free(status->installed_version);
  g_free(status);
}

/* The index the comparisons read, loading it from the cache the first
 * time it is asked for. NULL when nothing is cached or the cache cannot
 * be used, which reads as "nothing to compare against".
 */
static struct online_update_index *get_index(void) {
  if (!held_index && !index_load_attempted) {
    index_load_attempted = TRUE;
    held_index = online_update_load_cached_index();
  }

  return held_index;
}

/* Whether a card can be compared. Simulated cards take their pid from a
 * loaded state file, and a socket card waiting for its driver has
 * neither a readable firmware version nor its final driver type, which
 * is what selects the firmware generation.
 */
static gboolean card_is_comparable(struct alsa_card *card) {
  return card &&
         card->num != SIMULATED_CARD_NUM &&
         card->driver_type != DRIVER_TYPE_SOCKET_UNINIT;
}

/* The best firmware known for this card from any source, compared
 * against what it is running, or NULL when there is nothing to say.
 * Free with free_status().
 *
 * The index may be absent — never fetched, or update checks turned off
 * — leaving the firmware store as the only source. A local file newer
 * than the running firmware is worth reporting either way.
 */
static struct card_update_status *compute_status(struct alsa_card *card) {
  struct online_update_index *idx =
    app_settings_get_check_online_updates() ? get_index() : NULL;

  struct online_firmware *offered = idx ? g_hash_table_lookup(
    idx->firmware, GINT_TO_POINTER(card->pid)
  ) : NULL;

  const uint32_t *local = card_best_local_firmware(card);

  if (!offered && !local) {
    if (debug_enabled("update-check"))
      fprintf(
        stderr, "Update check: no firmware listed or held for %04x (%s)\n",
        card->pid, card->name
      );
    return NULL;
  }

  uint32_t have[FIRMWARE_VERSION_PARTS];

  if (!card_running_firmware_version(card, have)) {
    if (debug_enabled("update-check"))
      fprintf(
        stderr,
        "Update check: %04x (%s) reports no firmware version\n",
        card->pid, card->name
      );
    return NULL;
  }

  /* The best version from any source. A local file wins ties, since an
   * equal version already downloaded needs no download.
   */
  gboolean use_local = local &&
    (!offered || firmware_version_cmp(local, offered->version_parts) >= 0);

  const uint32_t *best = use_local ? local : offered->version_parts;

  struct card_update_status *status = g_malloc0(sizeof(*status));

  status->available = firmware_version_cmp(best, have) > 0;
  status->offered_version = use_local ?
    card_format_firmware_version(card, local) : g_strdup(offered->version);
  status->installed_version = card_format_firmware_version(card, have);
  status->have_local_file = use_local;
  status->firmware = offered;

  return status;
}

// Print a card's status, once per change rather than once per comparison
static void report_status(struct alsa_card *card) {
  const struct card_update_status *status = card->update_status;

  if (!status)
    return;

  if (status->available)
    fprintf(
      stderr, "Update check: %04x (%s) firmware %s available, running %s%s\n",
      card->pid, card->name,
      status->offered_version, status->installed_version,
      status->have_local_file ? " (held locally)" : " (needs downloading)"
    );
  else if (debug_enabled("update-check"))
    fprintf(
      stderr,
      "Update check: %04x (%s) firmware is up to date at %s "
      "(best known %s)\n",
      card->pid, card->name,
      status->installed_version, status->offered_version
    );
}

/* Whether two statuses say the same thing, so that recomputing after a
 * fetch does not report a second time about firmware already reported.
 */
static gboolean status_is_same(
  const struct card_update_status *a,
  const struct card_update_status *b
) {
  if (!a || !b)
    return a == b;

  return a->available == b->available &&
         a->have_local_file == b->have_local_file &&
         g_strcmp0(a->offered_version, b->offered_version) == 0 &&
         g_strcmp0(a->installed_version, b->installed_version) == 0;
}

/* Recompute a card's status, reporting and notifying only when it says
 * something new.
 */
static void update_status(struct alsa_card *card) {
  struct card_update_status *previous = card->update_status;

  card->update_status = compute_status(card);

  gboolean same = status_is_same(previous, card->update_status);

  free_status(previous);

  if (same)
    return;

  report_status(card);
  status_changed(card);
}

const struct card_update_status *online_check_get_status(
  struct alsa_card *card
) {
  return card->update_status;
}

/* A download in flight, which outlives the card it is for: there is no
 * way to cancel the request, so the completion checks whether the card
 * is still connected rather than trusting the pointer.
 */
struct download_request {
  struct alsa_card *card;
};

static GSList *downloads;

// Whether this card is still in the card list
static gboolean card_still_present(struct alsa_card *card) {
  GArray *cards = alsa_get_cards();

  for (guint i = 0; cards && i < cards->len; i++)
    if (g_array_index(cards, struct alsa_card *, i) == card)
      return TRUE;

  return FALSE;
}

const struct card_download *online_check_get_download(
  struct alsa_card *card
) {
  return card->download;
}

static void free_download(struct card_download *download) {
  if (!download)
    return;

  g_free(download->error);
  g_free(download);
}

/* The download record for a card, created on first use. A card that has
 * never had firmware to fetch has none at all, which reads as "not
 * considered".
 */
static struct card_download *get_download(struct alsa_card *card) {
  if (!card->download)
    card->download = g_malloc0(sizeof(*card->download));

  return card->download;
}

static void set_download_error(
  struct alsa_card    *card,
  enum download_state  state,
  char                *error
) {
  struct card_download *download = get_download(card);

  download->state = state;
  g_free(download->error);
  download->error = error;

  status_changed(card);
}

/* Recompute every connected card's status.
 *
 * Whether any of them should start a download is a separate question,
 * asked by the callers that can answer it: a fetch that replaced the
 * index can offer firmware nobody has, while a download completing only
 * ever makes a card's own file local.
 */
static void recompare_all_cards(void) {
  GArray *cards = alsa_get_cards();

  for (guint i = 0; cards && i < cards->len; i++) {
    struct alsa_card *card = g_array_index(cards, struct alsa_card *, i);

    if (card_is_comparable(card))
      update_status(card);
  }
}

/* Pick up a downloaded file: the store keys on the best version per
 * device, so a new file is adopted by discarding the store and walking
 * every directory again rather than by adding the one file. That keeps
 * one code path deciding which firmware is best.
 *
 * No download is considered afterwards: the file that just arrived makes
 * its own card's status local, and no other card's offering has changed.
 */
static void reenumerate_firmware(void) {
  firmware_store_reset();
  firmware_enum_all();
  recompare_all_cards();
}

static void download_progress(gsize received, gsize total, gpointer user_data) {
  struct download_request *req = user_data;

  if (!card_still_present(req->card))
    return;

  struct card_download *download = get_download(req->card);

  download->received = received;
  download->total = total;

  status_changed(req->card);
}

static void download_finished(
  const char *fn,
  const char *error,
  gpointer    user_data
) {
  struct download_request *req = user_data;
  struct alsa_card *card = req->card;

  downloads = g_slist_remove(downloads, req);
  g_free(req);

  if (error)
    fprintf(stderr, "Update check: download failed: %s\n", error);
  else if (debug_enabled("update-check"))
    fprintf(stderr, "Update check: downloaded %s\n", fn);

  /* Re-enumerate regardless of whether the card is still here: the file
   * is on disk and belongs in the store for whatever asks next.
   */
  if (!error)
    reenumerate_firmware();

  if (!card_still_present(card))
    return;

  if (error) {
    set_download_error(card, DOWNLOAD_STATE_FAILED, g_strdup(error));
    return;
  }

  /* reenumerate_firmware() has already recomputed and reported this
   * card's status, which now says have_local_file.
   */
  struct card_download *download = get_download(card);

  download->state = DOWNLOAD_STATE_DONE;
  download->received = download->total;

  status_changed(card);
}

/* Begin the transfer, having established that there is something to
 * transfer and that this machine can install it.
 */
static void begin_download(
  struct alsa_card                *card,
  const struct card_update_status *status
) {
  struct download_request *req = g_malloc0(sizeof(*req));

  req->card = card;

  downloads = g_slist_prepend(downloads, req);

  struct card_download *download = get_download(card);

  download->state = DOWNLOAD_STATE_RUNNING;
  download->received = 0;
  download->total = status->firmware->size;
  g_clear_pointer(&download->error, g_free);

  if (debug_enabled("update-check"))
    fprintf(
      stderr, "Update check: downloading firmware %s for %04x (%s)\n",
      status->firmware->version, card->pid, card->name
    );

  status_changed(card);

  online_update_download(
    status->firmware, download_finished, req, download_progress
  );
}

/* Consider downloading the firmware the index offers for this card.
 * Called after every comparison, so it must be idempotent: a download
 * already running, already done, or already known to be blocked is left
 * as it is.
 *
 * retry restarts a failed download, which is what a user clicking
 * Update after a failure is asking for. Without it a failure stands
 * until the next launch, since a recomputed status calls this again.
 */
static void consider_download(struct alsa_card *card, gboolean retry) {
  const struct card_update_status *status = online_check_get_status(card);

  // Nothing offered, or the file is already here
  if (!status || !status->available || status->have_local_file ||
      !status->firmware) {
    if (card->download)
      card->download->state = DOWNLOAD_STATE_NONE;
    return;
  }

  struct card_download *download = card->download;

  if (download)
    switch (download->state) {
      case DOWNLOAD_STATE_RUNNING:
      case DOWNLOAD_STATE_DONE:
        return;
      case DOWNLOAD_STATE_FAILED:
        if (!retry)
          return;
        break;
      case DOWNLOAD_STATE_NONE:
      case DOWNLOAD_STATE_BLOCKED:
        break;
    }

  /* Already on disk from an earlier run, so there is nothing to fetch
   * even though the status still asks for a download.
   *
   * That combination is reachable: the file the index points at may hold
   * a version other than the one the index claims, or one no newer than
   * the device already runs, in which case it never becomes the best
   * version and the status keeps asking. Without this the same firmware
   * would be downloaded again on every launch.
   *
   * A file that does not match the index is deleted rather than kept, so
   * a corrupt one is replaced on the next attempt instead of sticking.
   */
  if (!retry && online_update_already_downloaded(status->firmware)) {
    if (debug_enabled("update-check"))
      fprintf(
        stderr,
        "Update check: firmware %s for %04x (%s) is already downloaded "
        "but is not the best version available; not fetching it again\n",
        status->firmware->version, card->pid, card->name
      );

    struct card_download *d = get_download(card);

    d->state = DOWNLOAD_STATE_DONE;
    d->received = d->total = status->firmware->size;
    return;
  }

  /* Requirements are checked before the transfer rather than before the
   * install: fetching firmware this machine cannot install wastes the
   * transfer and leaves a file in the cache that every later comparison
   * would offer as though it were installable.
   */
  char *fcp_version = card_fcp_server_version(card);
  char *requirement =
    online_firmware_requirement_error(status->firmware, fcp_version);

  g_free(fcp_version);

  if (requirement) {
    // Say so once, not on every recomparison
    if (!download || download->state != DOWNLOAD_STATE_BLOCKED) {
      fprintf(
        stderr, "Update check: %04x (%s) cannot install firmware %s: %s\n",
        card->pid, card->name, status->firmware->version, requirement
      );
      set_download_error(card, DOWNLOAD_STATE_BLOCKED, requirement);
    } else {
      g_free(requirement);
    }
    return;
  }

  begin_download(card, status);
}

void online_check_start_download(struct alsa_card *card) {
  consider_download(card, TRUE);
}

void online_check_card_ready(struct alsa_card *card) {
  if (!card_is_comparable(card))
    return;

  /* Not gated on check-online-updates: a firmware file already on this
   * machine is worth reporting with the network never touched. The
   * setting gates the index, in compute_status().
   */
  update_status(card);
  consider_download(card, FALSE);
}

void online_check_card_gone(struct alsa_card *card) {
  free_download(card->download);
  card->download = NULL;

  if (!card->update_status)
    return;

  free_status(card->update_status);
  card->update_status = NULL;

  status_changed(card);
}

/* The newer version of this app the index lists, or NULL. Points into
 * the held index, so it is recomputed whenever that is replaced.
 */
static const char *client_update;

const char *online_check_get_client_update(void) {
  return client_update;
}

/* Compare the app's own version against the index, reporting a newer
 * release once rather than once per comparison.
 */
static void check_client_version(void) {
  static char *reported;

  struct online_update_index *idx = get_index();

  client_update = NULL;

  if (!idx)
    return;

  for (GList *l = idx->clients; l; l = l->next) {
    struct online_client *client = l->data;

    if (g_strcmp0(client->name, "alsa-scarlett-gui") != 0)
      continue;

    if (online_client_version_cmp(client->version, VERSION) > 0)
      client_update = client->version;

    // A fetch reporting what the cache already did says nothing new
    if (g_strcmp0(reported, client->version) == 0)
      return;

    g_free(reported);
    reported = g_strdup(client->version);

    if (client_update)
      fprintf(
        stderr, "Update check: alsa-scarlett-gui %s available, running %s\n",
        client->version, VERSION
      );
    else if (debug_enabled("update-check"))
      fprintf(
        stderr,
        "Update check: alsa-scarlett-gui is up to date at %s "
        "(index lists %s)\n",
        VERSION, client->version
      );
    return;
  }

  if (debug_enabled("update-check"))
    fprintf(stderr, "Update check: index lists no alsa-scarlett-gui\n");
}

/* Adopt a freshly fetched index. The cards are recomputed before the
 * previous index is freed, because each status borrows an entry from it.
 */
static void index_fetched(
  struct online_update_index *fetched,
  const char                 *error,
  gpointer                    user_data
) {
  if (!fetched) {
    fprintf(stderr, "Update check: %s\n", error ? error : "failed");
    return;
  }

  struct online_update_index *previous = held_index;

  held_index = fetched;
  index_load_attempted = TRUE;

  const char *was = client_update;

  check_client_version();
  recompare_all_cards();

  /* A fresh index can offer firmware no card holds, which is the case
   * automatic downloading exists for. Considered after every status is
   * recomputed, so each decision reads a current one.
   */
  GArray *cards = alsa_get_cards();

  for (guint i = 0; cards && i < cards->len; i++) {
    struct alsa_card *card = g_array_index(cards, struct alsa_card *, i);

    if (card_is_comparable(card))
      consider_download(card, FALSE);
  }

  /* A new app version alone changes no card's status, so nothing above
   * has notified about it.
   */
  if (g_strcmp0(was, client_update) != 0)
    status_changed(NULL);

  online_update_index_free(previous);
}

void online_check_start(void) {
  if (!app_settings_get_check_online_updates())
    return;

  check_client_version();

  if (!online_update_check_is_due()) {
    if (debug_enabled("update-check"))
      fprintf(stderr, "Update check: checked within the last day\n");
    return;
  }

  online_update_fetch_index(
    app_settings_get_send_usage_info(), index_fetched, NULL
  );
}

#endif // HAVE_ONLINE_UPDATE
