// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config-display.h"
#include "config-helpers.h"
#include "gtkhelper.h"
#include "optional-state.h"
#include "window-configuration.h"
#include "window-levels.h"
#include "window-mixer.h"

// levels update rate options (Hz → ms)
static const struct {
  const char *label;
  int         hz;
  int         ms;
} levels_rates[] = {
  {  "5 Hz",  5, 200 },
  { "10 Hz", 10, 100 },
  { "20 Hz", 20,  50 },
};

#define LEVELS_RATE_COUNT \
  ((int)(sizeof(levels_rates) / sizeof(levels_rates[0])))

int levels_hz_to_ms(int hz) {
  for (int i = 0; i < LEVELS_RATE_COUNT; i++)
    if (levels_rates[i].hz == hz)
      return levels_rates[i].ms;
  return DEFAULT_LEVELS_INTERVAL_MS;
}

static int ms_to_rate_index(int ms) {
  for (int i = 0; i < LEVELS_RATE_COUNT; i++)
    if (levels_rates[i].ms == ms)
      return i;
  for (int i = 0; i < LEVELS_RATE_COUNT; i++)
    if (levels_rates[i].ms == DEFAULT_LEVELS_INTERVAL_MS)
      return i;
  return 0;
}

static int parse_bool_pref(
  GHashTable *state,
  const char *key,
  int         fallback
) {
  if (!state)
    return fallback;
  const char *val = g_hash_table_lookup(state, key);
  if (!val)
    return fallback;
  if (strcmp(val, "true") == 0)
    return 1;
  if (strcmp(val, "false") == 0)
    return 0;
  return fallback;
}

void load_display_prefs(struct alsa_card *card) {
  GHashTable *state = optional_state_load(
    card, CONFIG_SECTION_UI
  );

  card->pref_show_bottom_right_labels = parse_bool_pref(
    state, "mixer-show-bottom-right-labels", 0
  );

  const char *val = state
    ? g_hash_table_lookup(state, "levels-update-rate")
    : NULL;
  card->pref_levels_interval_ms =
    levels_hz_to_ms(val ? atoi(val) : 0);

  if (state)
    g_hash_table_destroy(state);
}

struct rate_buttons {
  struct alsa_card *card;
  GtkWidget        *buttons[LEVELS_RATE_COUNT];
};

static void on_bottom_right_labels_changed(
  GObject    *sw,
  GParamSpec *pspec,
  gpointer    data
) {
  struct alsa_card *card = data;

  card->pref_show_bottom_right_labels =
    gtk_switch_get_active(GTK_SWITCH(sw));
  optional_state_save(
    card, CONFIG_SECTION_UI,
    "mixer-show-bottom-right-labels",
    card->pref_show_bottom_right_labels ? "true" : "false"
  );
  rebuild_mixer_grid(card);
}

static void on_rate_button_toggled(
  GtkToggleButton *button,
  gpointer         data
) {
  struct rate_buttons *rb = data;

  if (!gtk_toggle_button_get_active(button))
    return;

  int idx = GPOINTER_TO_INT(
    g_object_get_data(G_OBJECT(button), "rate-index")
  );

  rb->card->pref_levels_interval_ms = levels_rates[idx].ms;

  char buf[8];
  snprintf(buf, sizeof(buf), "%d", levels_rates[idx].hz);
  optional_state_save(
    rb->card, CONFIG_SECTION_UI, "levels-update-rate", buf
  );

  restart_levels_timer(rb->card);
}

/* One setting: heading and control on the left, description on the
 * right, matching the startup tab.
 */
static void add_setting(
  GtkWidget  *grid,
  int        *grid_y,
  const char *label,
  GtkWidget  *control,
  const char *description
) {
  if (*grid_y) {
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 10);
    gtk_widget_set_margin_bottom(sep, 10);
    gtk_grid_attach(GTK_GRID(grid), sep, 0, (*grid_y)++, 2, 1);
  }

  GtkWidget *heading = config_bold_label(label);
  gtk_widget_set_valign(heading, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), heading, 0, *grid_y, 1, 1);

  gtk_widget_set_halign(control, GTK_ALIGN_START);
  gtk_widget_set_valign(control, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), control, 0, *grid_y + 1, 1, 1);

  gtk_grid_attach(
    GTK_GRID(grid),
    config_wrapped_help_label(description, 70),
    1, *grid_y, 1, 2
  );

  *grid_y += 2;
}

static GtkWidget *make_rate_buttons(struct alsa_card *card) {
  struct rate_buttons *rb = g_malloc0(sizeof(struct rate_buttons));
  rb->card = card;

  int active = ms_to_rate_index(card->pref_levels_interval_ms);

  GtkWidget *rate_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  for (int i = 0; i < LEVELS_RATE_COUNT; i++) {
    GtkWidget *btn = gtk_toggle_button_new_with_label(
      levels_rates[i].label
    );
    gtk_toggle_button_set_active(
      GTK_TOGGLE_BUTTON(btn), i == active
    );
    if (i > 0)
      gtk_toggle_button_set_group(
        GTK_TOGGLE_BUTTON(btn),
        GTK_TOGGLE_BUTTON(rb->buttons[0])
      );

    g_object_set_data(
      G_OBJECT(btn), "rate-index", GINT_TO_POINTER(i)
    );
    g_signal_connect(
      btn, "toggled", G_CALLBACK(on_rate_button_toggled), rb
    );

    rb->buttons[i] = btn;
    gtk_box_append(GTK_BOX(rate_box), btn);
  }

  g_object_weak_ref(G_OBJECT(rate_box), (GWeakNotify)g_free, rb);

  return rate_box;
}

void add_display_tab(GtkWidget *notebook, struct alsa_card *card) {
  GtkWidget *grid = gtk_grid_new();
  gtk_widget_set_margin(grid, 20);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 10);

  int grid_y = 0;

  GtkWidget *sw = gtk_switch_new();
  gtk_switch_set_active(
    GTK_SWITCH(sw), card->pref_show_bottom_right_labels
  );
  g_signal_connect(
    sw, "notify::active",
    G_CALLBACK(on_bottom_right_labels_changed), card
  );
  add_setting(
    grid, &grid_y, "Mixer Bottom/Right Labels", sw,
    "The mixer labels its inputs down the left side and its outputs "
    "across the top. Enable this to repeat them along the bottom and "
    "right edges as well, to make it easier to see which input and "
    "output a gain control belongs to on a large matrix."
  );

  if (card->has_levels)
    add_setting(
      grid, &grid_y, "Level Meter Update Rate",
      make_rate_buttons(card),
      "How often the level meters are redrawn, wherever they appear. "
      "A lower rate makes the meters less responsive but uses less CPU."
    );

  GtkWidget *scrolled = wrap_tab_content_scrolled(grid);
  g_object_set_data(G_OBJECT(scrolled), PAGE_ID_KEY, (gpointer)"display");
  gtk_notebook_append_page(
    GTK_NOTEBOOK(notebook), scrolled, gtk_label_new("Display")
  );
}
