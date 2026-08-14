// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <glib/gstdio.h>

#include "app-settings.h"

#define SETTINGS_FILE "settings.conf"
#define SECTION_ONLINE "online"
#define SECTION_UI "ui"

char *app_settings_get_config_dir(void) {
  return g_build_filename(
    g_get_user_config_dir(), "alsa-scarlett-gui", NULL
  );
}

GKeyFile *app_settings_load_keyfile(const char *name, char **out_path) {
  char *config_dir = app_settings_get_config_dir();

  if (g_mkdir_with_parents(config_dir, 0755) < 0) {
    g_warning("Failed to create config directory: %s", config_dir);
    g_free(config_dir);
    return NULL;
  }

  *out_path = g_build_filename(config_dir, name, NULL);
  g_free(config_dir);

  GKeyFile *key_file = g_key_file_new();
  g_key_file_load_from_file(key_file, *out_path, G_KEY_FILE_NONE, NULL);

  return key_file;
}

/* Open the settings file for reading, or NULL if it does not exist.
 * A missing file, section, or key all mean "not set", which for every
 * setting here is false or zero.
 */
static GKeyFile *load_settings(void) {
  char *config_dir = app_settings_get_config_dir();
  char *path = g_build_filename(config_dir, SETTINGS_FILE, NULL);
  g_free(config_dir);

  GKeyFile *key_file = g_key_file_new();
  gboolean loaded = g_key_file_load_from_file(
    key_file, path, G_KEY_FILE_NONE, NULL
  );

  g_free(path);

  if (loaded)
    return key_file;

  g_key_file_free(key_file);

  return NULL;
}

static gboolean get_boolean(const char *section, const char *key) {
  GKeyFile *key_file = load_settings();

  if (!key_file)
    return FALSE;

  gboolean value = g_key_file_get_boolean(key_file, section, key, NULL);

  g_key_file_free(key_file);

  return value;
}

static int get_integer(const char *section, const char *key) {
  GKeyFile *key_file = load_settings();

  if (!key_file)
    return 0;

  int value = g_key_file_get_integer(key_file, section, key, NULL);

  g_key_file_free(key_file);

  return value;
}

// Write the keyfile back, having set one key in it
static void save_settings(GKeyFile *key_file, char *path) {
  GError *error = NULL;

  if (!g_key_file_save_to_file(key_file, path, &error)) {
    g_warning("Failed to save settings file %s: %s", path, error->message);
    g_error_free(error);
  }

  g_key_file_free(key_file);
  g_free(path);
}

static void set_boolean(
  const char *section,
  const char *key,
  gboolean    value
) {
  char *path;
  GKeyFile *key_file = app_settings_load_keyfile(SETTINGS_FILE, &path);

  if (!key_file)
    return;

  g_key_file_set_boolean(key_file, section, key, value);
  save_settings(key_file, path);
}

static void set_integer(const char *section, const char *key, int value) {
  char *path;
  GKeyFile *key_file = app_settings_load_keyfile(SETTINGS_FILE, &path);

  if (!key_file)
    return;

  g_key_file_set_integer(key_file, section, key, value);
  save_settings(key_file, path);
}

gboolean app_settings_get_check_online_updates(void) {
  return get_boolean(SECTION_ONLINE, "check-online-updates");
}

/* The usage census rides on the index fetch as query parameters, so it
 * cannot be sent on its own: turning update checks off turns it off too.
 */
void app_settings_set_check_online_updates(gboolean value) {
  set_boolean(SECTION_ONLINE, "check-online-updates", value);

  if (!value)
    set_boolean(SECTION_ONLINE, "send-usage-info", FALSE);
}

// False whenever update checks are off, whatever the file says
gboolean app_settings_get_send_usage_info(void) {
  return get_boolean(SECTION_ONLINE, "check-online-updates") &&
         get_boolean(SECTION_ONLINE, "send-usage-info");
}

// Joining the census means turning on the check it is sent with
void app_settings_set_send_usage_info(gboolean value) {
  set_boolean(SECTION_ONLINE, "send-usage-info", value);

  if (value)
    set_boolean(SECTION_ONLINE, "check-online-updates", TRUE);
}

gboolean app_settings_get_online_choice_made(void) {
  return get_integer(SECTION_ONLINE, "choice-version") >=
         ONLINE_CHOICE_VERSION;
}

void app_settings_set_online_choice_made(void) {
  set_integer(SECTION_ONLINE, "choice-version", ONLINE_CHOICE_VERSION);
}

gboolean app_settings_get_preferences_visible(void) {
  return get_boolean(SECTION_UI, "window-preferences");
}

void app_settings_set_preferences_visible(gboolean value) {
  set_boolean(SECTION_UI, "window-preferences", value);
}
