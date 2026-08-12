// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HAVE_ONLINE_UPDATE

#include <stdio.h>
#include <string.h>

#include "alsa.h"
#include "app-settings.h"
#include "debug.h"
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

void online_check_card_ready(struct alsa_card *card) {
  if (!card_is_comparable(card))
    return;

  /* Not gated on check-online-updates: a firmware file already on this
   * machine is worth reporting with the network never touched. The
   * setting gates the index, in compute_status().
   */
  update_status(card);
}

void online_check_card_gone(struct alsa_card *card) {
  if (!card->update_status)
    return;

  free_status(card->update_status);
  card->update_status = NULL;

  status_changed(card);
}

const struct card_update_status *online_check_get_status(
  struct alsa_card *card
) {
  return card->update_status;
}

// Recompute every connected card against the index now held
static void recompare_all_cards(void) {
  GArray *cards = alsa_get_cards();

  for (guint i = 0; cards && i < cards->len; i++) {
    struct alsa_card *card = g_array_index(cards, struct alsa_card *, i);

    if (card_is_comparable(card))
      update_status(card);
  }
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
