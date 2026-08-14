// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <glib.h>

/* App-wide (not per-device) settings, persisted to
 * ~/.config/alsa-scarlett-gui/settings.conf.
 *
 * Both flags default to false. A false read is therefore ambiguous on
 * its own: it may mean "the user said no" or "the user has not been
 * asked". app_settings_get_online_choice_made() distinguishes the two,
 * and is what gates the first-run consent dialog. Recording a decision
 * is a separate step from setting the flags so that answering the
 * dialog with no to everything still counts as an answer.
 */

/* The answer is recorded as the version of the questions answered, so
 * that changing what is asked re-asks it. Raise this when a question
 * is added or what one discloses changes materially.
 */
#define ONLINE_CHOICE_VERSION 1

/* ~/.config/alsa-scarlett-gui, free with g_free(). */
char *app_settings_get_config_dir(void);

/* Create the config directory and open the named keyfile within it,
 * returning it along with its path (both to be freed by the caller),
 * or NULL if the directory could not be created. A keyfile that does
 * not exist yet loads as empty, ready to be written.
 */
GKeyFile *app_settings_load_keyfile(const char *name, char **out_path);

// Check online for firmware and software updates
gboolean app_settings_get_check_online_updates(void);
void app_settings_set_check_online_updates(gboolean value);

/* Send the usage census ping alongside the update check. Only
 * meaningful when check_online_updates is also set, since the ping
 * rides on the index fetch as query parameters.
 */
gboolean app_settings_get_send_usage_info(void);
void app_settings_set_send_usage_info(gboolean value);

/* Whether the user has answered the current questions about the two
 * flags above. False if they have never been asked, or were asked an
 * older set.
 */
gboolean app_settings_get_online_choice_made(void);
void app_settings_set_online_choice_made(void);

/* Whether the preferences window was open when the app last exited.
 * The per-card windows persist this in each card's state file; this one
 * has no card to belong to.
 */
gboolean app_settings_get_preferences_visible(void);
void app_settings_set_preferences_visible(gboolean value);
