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

/* Register interest in status changes, called with the card whose
 * status was computed or discarded. A fetch completing recomputes every
 * connected card, so a notice built from a status can follow the index
 * rather than only the card.
 *
 * There is no unregister; the registration lasts for the process.
 */
void online_check_add_status_changed_callback(
  void (*callback)(struct alsa_card *card)
);

#endif // HAVE_ONLINE_UPDATE
