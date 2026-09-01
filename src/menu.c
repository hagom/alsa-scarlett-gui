// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "about.h"
#include "app-settings.h"
#include "file.h"
#include "menu.h"
#include "optional-state.h"
#include "window-configuration.h"
#include "window-hardware.h"
#include "window-preferences.h"
#include "window-geometry.h"
#include "window-screenshot.h"

// mapping of action names to window offsets within struct alsa_card
struct window_action {
  const char *action_name;
  size_t      window_offset;
};

static const struct window_action window_actions[] = {
  { "routing",       offsetof(struct alsa_card, window_routing)       },
  { "mixer",         offsetof(struct alsa_card, window_mixer)         },
  { "levels",        offsetof(struct alsa_card, window_levels)        },
  { "configuration", offsetof(struct alsa_card, window_configuration) },
  { "dsp",           offsetof(struct alsa_card, window_dsp)           },
  { NULL, 0 }
};

static GtkWidget *get_card_window(struct alsa_card *card, size_t offset) {
  return *(GtkWidget **)((char *)card + offset);
}

static const struct window_action *find_window_action(const char *action_name) {
  for (const struct window_action *entry = window_actions;
       entry->action_name;
       entry++)
    if (strcmp(entry->action_name, action_name) == 0)
      return entry;
  return NULL;
}

static char *make_window_key(const char *action_name) {
  return g_strdup_printf("window-%s", action_name);
}

// set action state and window visibility (no persistence)
static void set_window_visible(
  GSimpleAction *action,
  GtkWidget     *window,
  gboolean       visible
) {
  g_action_change_state(G_ACTION(action), g_variant_new_boolean(visible));
  gtk_widget_set_visible(window, visible);
}

// toggle visibility; returns new state for callers that need to persist it
static gboolean toggle_visibility(GSimpleAction *action, GtkWidget *widget) {
  GVariant *state = g_action_get_state(G_ACTION(action));
  gboolean new_state = !g_variant_get_boolean(state);
  set_window_visible(action, widget, new_state);
  return new_state;
}

static void save_window_state(
  struct alsa_card *card,
  const char       *action_name,
  gboolean          visible
) {
  char *key = make_window_key(action_name);
  optional_state_save(card, CONFIG_SECTION_UI, key, visible ? "true" : "false");
  g_free(key);
}

// toggle visibility and save state (for card windows)
static void activate_window(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  struct alsa_card *card = data;
  const char *action_name = g_action_get_name(G_ACTION(action));

  const struct window_action *entry = find_window_action(action_name);
  if (!entry)
    return;

  GtkWidget *window = get_card_window(card, entry->window_offset);
  gboolean new_state = toggle_visibility(action, window);
  save_window_state(card, action_name, new_state);
}

// Open the configuration window (if not already open) and select the
// tab named by the string parameter; used for remote control/scripting
static void activate_config_tab(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  struct alsa_card *card = data;

  if (!card->window_configuration)
    return;

  if (!gtk_widget_get_visible(card->window_configuration)) {
    GAction *config_action = g_action_map_lookup_action(
      G_ACTION_MAP(card->window_main), "configuration"
    );
    set_window_visible(
      G_SIMPLE_ACTION(config_action), card->window_configuration, TRUE
    );
    save_window_state(card, "configuration", TRUE);
  }

  configuration_select_tab(card, g_variant_get_string(parameter, NULL));
}

#ifdef HAVE_ONLINE_UPDATE

/* App-wide, so its visibility goes in the app settings rather than any
 * card's state file.
 */
static void activate_preferences(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  app_settings_set_preferences_visible(
    toggle_visibility(action, window_preferences)
  );
}

#endif

static void activate_hardware(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  toggle_visibility(action, window_hardware);
}

// Set a documentation action's outcome state: empty when it worked, and
// otherwise why it did not, so a script driving the action is told what
// went wrong instead of having to read this process's stderr.
static void set_outcome(GSimpleAction *action, char *reason) {
  g_simple_action_set_state(
    action, g_variant_new_take_string(reason ? reason : g_strdup(""))
  );
}

/* Render a card window to a PNG file, named by "main" or a subwindow
 * action name (routing/mixer/levels/configuration/dsp); used for
 * documentation screenshots. The action's state reports the outcome:
 * a window the compositor will not bring forward has no frame to
 * capture, and a script needs to be told that rather than left to
 * guess why no file appeared.
 */
static void activate_screenshot(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  struct alsa_card *card = data;
  const char *name, *path;
  g_variant_get(parameter, "(&s&s)", &name, &path);

  GtkWidget *window;
  if (strcmp(name, "main") == 0) {
    window = card->window_main;
  } else {
    const struct window_action *entry = find_window_action(name);
    window = entry ? get_card_window(card, entry->window_offset) : NULL;
  }

  if (!window) {
    set_outcome(action, g_strdup_printf("no %s window", name));
    return;
  }

  char *reason = NULL;
  if (!screenshot_widget_to_png(window, path, &reason)) {
    fprintf(stderr, "can't capture %s to %s: %s\n", name, path, reason);
    set_outcome(action, reason);
    return;
  }

  // Alongside the PNG, write a <path>.json sidecar (.png replaced with
  // .json where present) describing the documented regions within it.
  char *json_path;
  if (g_str_has_suffix(path, ".png"))
    json_path = g_strdup_printf("%.*s.json", (int)(strlen(path) - 4), path);
  else
    json_path = g_strconcat(path, ".json", NULL);

  char *image_name = g_path_get_basename(path);
  gboolean exported =
    export_widget_geometry_to_json(window, image_name, json_path);

  set_outcome(action, exported
    ? NULL
    : g_strdup_printf("captured %s but wrote no geometry sidecar", path));

  g_free(image_name);
  g_free(json_path);
}

// Save the card's complete configuration to the given path, in the
// same native format the presets and Save dialogue write. Unlike
// win.save this takes a path and shows no dialogue, so a script can
// capture the configuration.
static void activate_save_config(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  struct alsa_card *card = data;
  const char *path;
  g_variant_get(parameter, "&s", &path);

  if (save_native(card, path) < 0)
    fprintf(stderr, "can't save configuration to %s\n", path);
}

// Load a complete configuration from the given path. Unlike win.load
// this takes a path and shows no dialogue, so a script that has changed
// controls can put the card back as it found it. The native format
// carries the interface's own state as well as the device's, so this
// restores stereo links, port names and port visibility, which loading
// an ALSA state file cannot. The action's state reports the outcome,
// empty when the configuration was loaded: a script restoring the card
// to how it found it needs to know whether that happened.
static void activate_load_config(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  struct alsa_card *card = data;
  const char *path;
  g_variant_get(parameter, "&s", &path);

  set_outcome(action, load_native(card, path) < 0
    ? g_strdup_printf("can't load configuration from %s", path)
    : NULL);
}

// Set a control's value by name, as if the user had changed it in the
// UI, so documentation tooling can capture before/after states such as
// stereo link, port visibility, and custom names. The action's state
// reports the outcome, empty when the write was made: a name absent on
// this model would otherwise be indistinguishable from a change that
// took effect.
static void activate_set_control(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  struct alsa_card *card = data;
  const char *name, *value;
  g_variant_get(parameter, "(&s&s)", &name, &value);

  set_outcome(action, alsa_set_elem_by_name(card, name, value)
    ? NULL
    : g_strdup_printf("no control named %s", name));
}

/* Report a control's value by name into the action's state, so a script
 * can wait for a set-control to have taken effect rather than guess at a
 * delay. The value read is the one the interface holds, which for a real
 * card is updated when the driver's change event arrives, so it reflects
 * a write only once the interface has acted on it.
 *
 * The state is (found, value): a control with no custom name set reads
 * as empty, which a single string could not tell apart from a name that
 * does not exist on this model.
 */
static void activate_get_control(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  struct alsa_card *card = data;
  const char *name;
  g_variant_get(parameter, "&s", &name);

  struct alsa_elem *elem = get_elem_by_name(card->elems, name);
  char *value;

  if (!elem) {
    value = g_strdup("");

  } else if (elem->type == SND_CTL_ELEM_TYPE_BYTES) {
    size_t size = 0;
    const void *bytes = alsa_get_elem_bytes(elem, &size);

    // a byte control need not hold text, and a GVariant string that is
    // not valid UTF-8 aborts the process
    char *raw = bytes ? g_strndup(bytes, size) : g_strdup("");

    value = g_utf8_make_valid(raw, -1);
    g_free(raw);

  } else if (elem->count > 1) {

    // the cached value is refreshed by the change event for
    // single-valued elements only, so read the rest from the card
    long *values = alsa_get_elem_int_values(elem);
    GString *joined = g_string_new(NULL);

    for (int i = 0; values && i < elem->count; i++)
      g_string_append_printf(joined, i ? " %ld" : "%ld", values[i]);

    g_free(values);

    value = g_string_free(joined, FALSE);

  } else {
    value = g_strdup_printf("%ld", elem->value);
  }

  g_simple_action_set_state(
    action, g_variant_new("(bs)", elem != NULL, value)
  );

  g_free(value);
}

// List the card's control names into the action's state, so a script can
// choose targets that exist on this model instead of assuming a
// particular one. The list covers the interface's own optional elements
// — stereo link, port name and port visibility — which are not kernel
// controls and so cannot be found through ALSA.
static void activate_list_controls(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  struct alsa_card *card = data;

  GPtrArray *names = g_ptr_array_new();

  for (int i = 0; i < card->elems->len; i++) {
    struct alsa_elem *elem = g_ptr_array_index(card->elems, i);

    if (elem->card && elem->name)
      g_ptr_array_add(names, elem->name);
  }
  g_ptr_array_add(names, NULL);

  g_simple_action_set_state(
    action, g_variant_new_strv((const char * const *)names->pdata, -1)
  );

  g_ptr_array_free(names, TRUE);
}

static void activate_quit(
  GSimpleAction *action,
  GVariant      *parameter,
  gpointer       data
) {
  g_application_quit(G_APPLICATION(data));
}

static const GActionEntry app_entries[] = {
  {"hardware", activate_hardware, NULL, "false"},
  {"quit",     activate_quit},
#ifdef HAVE_ONLINE_UPDATE
  {"preferences", activate_preferences, NULL, "false"},
#endif
};

struct menu_item {
  const char *label;
  const char *action_name;
  const char *accelerators[2];
};

struct menu_data {
  const char       *label;
  struct menu_item *items;
};

static const struct menu_data menus[] = {
  {
    "_File",
    (struct menu_item[]){
      { "_Load Configuration",   "win.load", { "<Control>O", NULL } },
      { "_Save Configuration",   "win.save", { "<Control>S", NULL } },
      { "_Interface Simulation", "win.sim",      { "<Control>I", NULL } },
      { "_Close",                "window.close", { "<Control>W", NULL } },
      { "E_xit",                 "app.quit",     { "<Control>Q", NULL } },
      {}
    }
  },
  {
    "_View",
    (struct menu_item[]){
      { "_Routing",       "win.routing",       { "<Control>R", NULL } },
      { "_Mixer",         "win.mixer",         { "<Control>M", NULL } },
      { "_Levels",        "win.levels",        { "<Control>L", NULL } },
      { "_DSP",           "win.dsp",           { "<Control>D", NULL } },
      { "_Configuration", "win.configuration", { "<Control>G", NULL } },
#ifdef HAVE_ONLINE_UPDATE
      { "_Preferences",   "app.preferences",   { "<Control>P", NULL } },
#endif
      {}
    }
  },
  {
    "_Help",
    (struct menu_item[]){
      { "_Supported Hardware", "app.hardware", { "<Control>H",     NULL } },
      { "_About",              "win.about",    { "<Control>slash", NULL } },
      {}
    }
  },
  {}
};

static void populate_submenu(
  GtkApplication         *app,
  GMenu                  *menu,
  const struct menu_data *data
) {
  GMenu *submenu = g_menu_new();
  g_menu_append_submenu(menu, data->label, G_MENU_MODEL(submenu));

  // An empty-initialised menu_item marks the end
  for (struct menu_item *item = data->items; item->label; item++) {
    g_menu_append(submenu, item->label, item->action_name);
    gtk_application_set_accels_for_action(
      app, item->action_name, item->accelerators
    );
  }
}

GMenu *create_app_menu(GtkApplication *app) {
  g_action_map_add_action_entries(
    G_ACTION_MAP(app), app_entries, G_N_ELEMENTS(app_entries), app
  );

  GMenu *menu = g_menu_new();

  for (const struct menu_data *menu_data = menus;
       menu_data->label;
       menu_data++)
    populate_submenu(app, menu, menu_data);

  return menu;
}

static const GActionEntry win_entries[] = {
  {"about", activate_about},
  {"sim",   activate_sim}
};

void add_window_action_map(GtkWindow *w) {
  g_action_map_add_action_entries(
    G_ACTION_MAP(w), win_entries, G_N_ELEMENTS(win_entries), w
  );
}

static const GActionEntry load_save_entries[] = {
  {"load", activate_load},
  {"save", activate_save}
};

void add_load_save_action_map(struct alsa_card *card) {
  g_action_map_add_action_entries(
    G_ACTION_MAP(card->window_main),
    load_save_entries,
    G_N_ELEMENTS(load_save_entries),
    card
  );
}

static const GActionEntry screenshot_entries[] = {
  {"screenshot",    activate_screenshot,    "(ss)", "''"},
  {"set-control",   activate_set_control,   "(ss)", "''"},
  {"get-control",   activate_get_control,   "s",    "(false, '')"},
  {"list-controls", activate_list_controls, NULL,   "@as []"},
  {"save-config",   activate_save_config,   "s"},
  {"load-config",   activate_load_config,   "s",    "''"}
};

void add_screenshot_action_map(struct alsa_card *card) {
  g_action_map_add_action_entries(
    G_ACTION_MAP(card->window_main),
    screenshot_entries,
    G_N_ELEMENTS(screenshot_entries),
    card
  );
}

static const GActionEntry mixer_entries[] = {
  {"routing", activate_window, NULL, "false"},
  {"mixer",   activate_window, NULL, "false"}
};

static const GActionEntry levels_entries[] = {
  {"levels", activate_window, NULL, "false"}
};

static const GActionEntry configuration_entries[] = {
  {"configuration", activate_window,     NULL, "false"},
  {"config-tab",    activate_config_tab, "s"}
};

void add_mixer_action_map(struct alsa_card *card) {
  g_action_map_add_action_entries(
    G_ACTION_MAP(card->window_main),
    mixer_entries,
    G_N_ELEMENTS(mixer_entries),
    card
  );

  // Hide the levels menu item if there is no level meter support
  if (card->has_levels) {
    g_action_map_add_action_entries(
      G_ACTION_MAP(card->window_main),
      levels_entries,
      G_N_ELEMENTS(levels_entries),
      card
    );
  }
}

void add_configuration_action_map(struct alsa_card *card) {
  g_action_map_add_action_entries(
    G_ACTION_MAP(card->window_main),
    configuration_entries,
    G_N_ELEMENTS(configuration_entries),
    card
  );

  // Stateful action whose state lists the available configuration tab
  // ids, so a script can capture only the tabs that actually exist. The
  // configuration window is already built at this point.
  GSimpleAction *tabs = g_simple_action_new_stateful(
    "config-tabs", NULL, configuration_tab_ids(card)
  );
  g_action_map_add_action(G_ACTION_MAP(card->window_main), G_ACTION(tabs));
  g_object_unref(tabs);
}

static const GActionEntry dsp_entries[] = {
  {"dsp", activate_window, NULL, "false"}
};

void add_dsp_action_map(struct alsa_card *card) {
  g_action_map_add_action_entries(
    G_ACTION_MAP(card->window_main),
    dsp_entries,
    G_N_ELEMENTS(dsp_entries),
    card
  );
}

static gboolean should_restore_window(
  GHashTable *state,
  const char *action_name
) {
  char *key = make_window_key(action_name);
  const char *value = g_hash_table_lookup(state, key);
  g_free(key);
  return value && strcmp(value, "true") == 0;
}

static void restore_single_window(
  struct alsa_card              *card,
  const struct window_action    *entry,
  GHashTable                    *state
) {
  if (!should_restore_window(state, entry->action_name))
    return;

  GtkWidget *window = get_card_window(card, entry->window_offset);
  GAction *action = g_action_map_lookup_action(
    G_ACTION_MAP(card->window_main), entry->action_name
  );
  if (action)
    set_window_visible(G_SIMPLE_ACTION(action), window, TRUE);
}

void restore_window_visibility(struct alsa_card *card) {
  GHashTable *state = optional_state_load(card, CONFIG_SECTION_UI);
  if (!state)
    return;

  for (const struct window_action *entry = window_actions;
       entry->action_name;
       entry++)
    restore_single_window(card, entry, state);

  g_hash_table_destroy(state);
}
