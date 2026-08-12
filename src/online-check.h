// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HAVE_ONLINE_UPDATE

#include <glib.h>

#include "alsa.h"
#include "online-update.h"

/* The best firmware version known for a card from any source — a local
 * file in the firmware store, or an entry in the online index —
 * compared against the version the card is running.
 *
 * available says that best version is newer than the running one.
 * offered_version names it whether it came from a file or the index,
 * because a user asking what is available does not care which. The
 * comparison is against installed_version, what the device reports, so
 * a newer file in the store does not make the device look up to date.
 *
 * have_local_file says the best version is the local one, so it can be
 * installed without downloading anything. When it is false, firmware is
 * the index entry to download: it carries the url, checksum and
 * requirements. That pointer is into the held index and is valid until
 * a fetch replaces it, at which point every card's status is recomputed.
 * It is NULL when the index lists nothing for this card.
 */
struct card_update_status {
  gboolean                      available;
  char                         *offered_version;
  char                         *installed_version;
  gboolean                      have_local_file;
  const struct online_firmware *firmware;
};

/* Compare the cached index against the app's own version, and fetch a
 * fresh index when the last one is a day old.
 *
 * The comparison reads the cached index, so it reports on launches that
 * do not fetch. The fetch carries the usage census when send-usage-info
 * is set, and refreshes the cache for later comparisons.
 *
 * Does nothing when check-online-updates is false.
 */
void online_check_start(void);

/* Compute a card's update status, to be read with
 * online_check_get_status().
 *
 * Call when the card's controls are known. Recomputes if called again
 * for the same card, which happens when an FCP card is reported ready a
 * second time with its driver initialised.
 */
void online_check_card_ready(struct alsa_card *card);

// Discard a card's update status
void online_check_card_gone(struct alsa_card *card);

// A card's update status, or NULL if none has been computed
const struct card_update_status *online_check_get_status(
  struct alsa_card *card
);

/* The version of this app the index lists, when it is newer than the
 * one running, otherwise NULL. Valid until the index is replaced, which
 * the status changed callbacks report.
 */
const char *online_check_get_client_update(void);

/* How a card's automatic firmware download is getting on.
 *
 * A download starts as soon as a comparison finds firmware the index
 * offers and this machine does not hold, so that a user who clicks
 * Update usually finds it already there. Nothing is installed without
 * being asked for; only the transfer is automatic.
 *
 * NONE covers both "nothing to download" and "the file is already
 * held" — in the latter case the status reports have_local_file and
 * there is nothing for a download to do.
 */
enum download_state {
  DOWNLOAD_STATE_NONE,
  DOWNLOAD_STATE_RUNNING,
  DOWNLOAD_STATE_DONE,
  DOWNLOAD_STATE_FAILED,
  DOWNLOAD_STATE_BLOCKED  // requirements unmet; error says which
};

struct card_download {
  enum download_state  state;
  gsize                received;
  gsize                total;     // 0 if not yet known
  char                *error;     // FAILED or BLOCKED only
};

/* A card's download progress, or NULL if none has been considered for
 * it. Valid until the next status or progress change, both of which the
 * status changed callbacks report.
 */
const struct card_download *online_check_get_download(
  struct alsa_card *card
);

/* Start the download for this card if it has not been started, or retry
 * one that failed. Called when the user asks for the firmware and the
 * automatic download is not already in hand; a running or finished
 * download is left alone.
 *
 * Progress and completion are reported through the status changed
 * callbacks, not through a callback of its own: more than one widget may
 * be watching a download that nobody's click started.
 */
void online_check_start_download(struct alsa_card *card);

/* Register interest in status changes, called with the card whose
 * status was computed or discarded, or NULL when a fetch changed the
 * app's own available version. A fetch completing recomputes every
 * connected card, so a notice built from a status can follow the index
 * rather than only the card.
 *
 * There is no unregister; the registration lasts for the process.
 */
void online_check_add_status_changed_callback(
  void (*callback)(struct alsa_card *card)
);

#endif // HAVE_ONLINE_UPDATE
