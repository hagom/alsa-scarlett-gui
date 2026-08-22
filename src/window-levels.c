// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ctype.h>
#include <gtk/gtk.h>

#include "db.h"
#include "gtkdial.h"
#include "glow.h"
#include "hw-io-availability.h"
#include "iface-mixer.h"
#include "optional-state.h"
#include "peak-tracker.h"
#include "port-enable.h"
#include "stereo-link.h"
#include "widget-gain.h"
#include "widget-level-meter.h"
#include "window-dsp.h"
#include "window-geometry.h"
#include "window-levels.h"
#include "window-mixer.h"
#include "window-routing.h"

// gap between meters that aren't part of a stereo pair
#define METER_GAP 3

// smaller gap between the two meters of a stereo pair
#define PAIR_GAP 1

// space between the meter groups
#define GROUP_SPACING 10

// how long a peak is held on the meters, in ms
#define PEAK_HOLD_MS 1000

// rows of meter groups: the capture path, the playback path, the
// mixer, and anything the layout doesn't cover
#define METER_ROW_COUNT 4

// one meter, and the routing endpoint it displays the level of; the
// endpoint supplies the display name, the enable state (hidden when
// disabled) and the stereo link state (paired meters butt together)
struct level_meter {
  GtkWidget          *widget;
  GtkWidget          *num_label;
  struct routing_src *src;
  struct routing_snk *snk;

  // for a meter with no routing endpoint, the channel of the meter
  // element it reads; -1 otherwise
  int                 level_index;

  // channel number shown under the meter
  int                 num;

  // where the label sits, and how many meters it currently spans
  GtkWidget          *grid;
  int                 col;
  int                 label_span;
};

// what a group's expander needs to save its collapsed state
struct group_state {
  struct levels *data;
  char          *key;
  int            row;
};

// a group of meters of the same kind, e.g. "Source Analogue"
struct meter_group {
  // the box holding the group, hidden when the whole group is
  // disabled or collapsed
  GtkWidget *widget;

  // the expander the group collapses by, and the button in the row
  // title that brings it back
  GtkWidget *expander;
  GtkWidget *chip;

  int row;
  int first_meter;
  int meter_count;
};

struct levels {
  struct alsa_card   *card;
  struct alsa_elem   *level_meter_elem;
  GtkWidget          *top;
  GtkWidget          *box;

  // for each row of groups, the box its groups go in, the box that
  // holds that and the row's title, and the rule above it (none for
  // the first row); the box and rule are hidden when the row is empty
  GtkWidget          *rows[METER_ROW_COUNT];
  GtkWidget          *row_boxes[METER_ROW_COUNT];
  GtkWidget          *row_seps[METER_ROW_COUNT];

  // where the buttons for the row's collapsed groups go, beside its
  // title
  GtkWidget          *row_chip_boxes[METER_ROW_COUNT];
  struct level_meter *meters;
  int                 meter_count;
  GArray             *groups;

  // elements whose callbacks must be removed when the window goes away
  GPtrArray          *watched_elems;
};

static int update_levels_controls(void *user_data) {
  struct levels *data = user_data;
  struct alsa_card *card = data->card;

  // main window was closed, stop the timer
  if (!card->window_main)
    return G_SOURCE_REMOVE;

  struct alsa_elem *level_meter_elem = data->level_meter_elem;

  // check which windows need updates
  int levels_visible = gtk_widget_get_visible(GTK_WIDGET(card->window_levels));
  int routing_visible = card->window_routing &&
                        gtk_widget_get_visible(GTK_WIDGET(card->window_routing));
  int mixer_visible = card->window_mixer &&
                      gtk_widget_get_visible(GTK_WIDGET(card->window_mixer));

  long *values = alsa_get_elem_int_values(level_meter_elem);

  // update the peak tracking clock for all widgets showing levels
  peak_tracker_tick();

  // update routing levels array (needed for the meters below, for
  // routing, mixer, and main window gains)
  if (card->routing_levels) {
    for (int i = 0; i < level_meter_elem->count; i++) {
      if (i < card->routing_levels_count) {
        double value = 20 * log10(values[i] / 4095.0);
        card->routing_levels[i] = value;
      }
    }

    if (routing_visible)
      gtk_widget_queue_draw(card->routing_lines);

    if (mixer_visible && card->mixer_glow)
      gtk_widget_queue_draw(card->mixer_glow);
  }

  // update level meters if levels window is visible
  if (levels_visible) {
    for (int i = 0; i < data->meter_count; i++) {
      struct level_meter *meter = &data->meters[i];

      if (!meter->widget)
        continue;

      int known = 1;
      double value;

      // a meter with an endpoint takes its level from it, which may
      // be the level of what the endpoint is routed to
      if (meter->snk)
        value = get_routing_snk_level(card, meter->snk, &known);
      else if (meter->src)
        value = get_routing_src_level(card, meter->src, &known);
      else if (meter->level_index >= 0 &&
               meter->level_index < level_meter_elem->count)
        value = 20 * log10(values[meter->level_index] / 4095.0);
      else
        continue;

      gtk_level_meter_set_unknown(GTK_LEVEL_METER(meter->widget), !known);

      if (known)
        gtk_level_meter_set_level(GTK_LEVEL_METER(meter->widget), value);
    }
  }

  // update mixer gain dial levels if mixer window is visible
  if (mixer_visible && card->mixer_gain_widgets) {
    for (GList *l = card->mixer_gain_widgets; l; l = l->next) {
      struct mixer_gain_widget *mg = l->data;

      // get the routing source connected to this mixer input
      if (!mg->r_snk || !mg->r_snk->elem)
        continue;

      // For stereo widgets, use max level across all routing sinks
      double level_db = -INFINITY;
      int snk_count = mg->r_snk_count > 0 ? mg->r_snk_count : 1;

      for (int snk_i = 0; snk_i < snk_count; snk_i++) {
        struct routing_snk *r_snk = snk_i < mg->r_snk_count ?
                                    mg->r_snks[snk_i] : mg->r_snk;
        if (!r_snk || !r_snk->elem)
          continue;

        int r_src_idx = alsa_get_elem_value(r_snk->elem);
        if (!r_src_idx)
          continue;

        struct routing_src *r_src = &g_array_index(
          card->routing_srcs, struct routing_src, r_src_idx
        );

        double src_level = get_routing_src_level_db(card, r_src);
        if (src_level > level_db)
          level_db = src_level;
      }

      // apply gain value to get post-gain level (in dB, so we add)
      if (mg->elem) {
        int gain_val = alsa_get_elem_value(mg->elem);
        double gain_db;

        if (mg->elem->dB_type == SND_CTL_TLVT_DB_LINEAR) {
          gain_db = linear_value_to_cdb(
            gain_val,
            mg->elem->min_val, mg->elem->max_val,
            mg->elem->min_cdB, mg->elem->max_cdB
          ) / 100.0;
        } else {
          double scale = (double)(mg->elem->max_cdB - mg->elem->min_cdB) / 100.0 /
                         (mg->elem->max_val - mg->elem->min_val);
          gain_db = (double)(gain_val - mg->elem->min_val) * scale +
                    mg->elem->min_cdB / 100.0;
        }

        level_db += gain_db;
      }

      // get the dial from the gain widget container
      GtkWidget *dial = get_gain_dial(mg->widget);
      if (dial)
        gtk_dial_set_level(GTK_DIAL(dial), level_db);
    }
  }

  // update input gain dial levels (main window is always visible when timer runs)
  if (card->input_gain_widgets) {
    for (GList *l = card->input_gain_widgets; l; l = l->next) {
      struct input_gain_widget *ig = l->data;

      if (!ig->r_src)
        continue;

      double level_db = get_routing_src_level_db(card, ig->r_src);

      GtkWidget *dial = get_gain_dial(ig->widget);
      if (dial)
        gtk_dial_set_level(GTK_DIAL(dial), level_db);
    }
  }

  // update output gain dial levels
  if (card->output_gain_widgets) {
    for (GList *l = card->output_gain_widgets; l; l = l->next) {
      struct output_gain_widget *og = l->data;

      if (!og->r_snk)
        continue;

      // use the sink level directly
      int index = get_routing_snk_level_index(og->r_snk);
      if (index < 0)
        continue;

      double level_db = card->routing_levels[index];

      // show -inf if muted by inactive monitor group
      if (is_snk_monitor_muted(og->r_snk))
        level_db = -INFINITY;

      GtkWidget *dial = get_gain_dial(og->widget);
      if (dial)
        gtk_dial_set_level(GTK_DIAL(dial), level_db);
    }
  }

  // update compressor curve levels if DSP window is visible
  int dsp_visible = card->window_dsp &&
                    gtk_widget_get_visible(GTK_WIDGET(card->window_dsp));
  if (dsp_visible && card->dsp_comp_widgets) {
    for (GList *l = card->dsp_comp_widgets; l; l = l->next) {
      struct dsp_comp_widget *dcw = l->data;

      double input_db = -80.0;
      double output_db = -80.0;

      // Get input level from DSP Input sink
      if (dcw->input_snk) {
        int idx = get_routing_snk_level_index(dcw->input_snk);
        if (idx >= 0 && idx < card->routing_levels_count)
          input_db = card->routing_levels[idx];
      }

      // Get output level from DSP Output source
      if (dcw->output_src)
        output_db = get_routing_src_level_db(card, dcw->output_src);

      gtk_compressor_curve_set_levels(dcw->curve, input_db, output_db);
    }
  }

  free(values);

  return 1;
}

// a group of meters, before the widgets are built
struct group_spec {
  char *name;
  int   row;
  int   count;
};

// each row is titled, so its groups are named by what distinguishes
// them from each other rather than by what they have in common
static const char *row_titles[METER_ROW_COUNT] = {
  "Inputs", "Outputs", "Mixer", "Other"
};

// how each row's collapsed state is keyed in the state file
static const char *row_ids[METER_ROW_COUNT] = {
  "inputs", "outputs", "mixer", "other"
};

// the mixer takes a lot of room and is the least often watched, so it
// starts collapsed
static const gboolean row_expanded_default[METER_ROW_COUNT] = {
  TRUE, TRUE, FALSE, TRUE
};

// the row the meters of anything the layout doesn't cover go on
#define OTHER_ROW 3

/* How the window is laid out: the capture path along the top row, the
 * playback path along the middle row, and the mixer below it. Each
 * group holds the routing endpoints of one direction, port category
 * and hardware type, whether the hardware meters them or not.
 */
struct group_layout {
  int         row;
  int         is_src;
  int         category;

  // the hardware type for PC_HW groups, -1 for the other categories
  int         hw_type;

  const char *name;
};

static const struct group_layout group_layouts[] = {
  { 0, 1, PC_HW,  HW_TYPE_ANALOGUE, "Analogue" },
  { 0, 0, PC_DSP, -1,               "DSP In"   },
  { 0, 1, PC_DSP, -1,               "DSP Out"  },
  { 0, 1, PC_HW,  HW_TYPE_SPDIF,    "S/PDIF"   },
  { 0, 1, PC_HW,  HW_TYPE_ADAT,     "ADAT"     },
  { 0, 0, PC_PCM, -1,               "PCM"      },

  { 1, 1, PC_PCM, -1,               "PCM"      },
  { 1, 0, PC_HW,  HW_TYPE_ANALOGUE, "Analogue" },
  { 1, 0, PC_HW,  HW_TYPE_SPDIF,    "S/PDIF"   },
  { 1, 0, PC_HW,  HW_TYPE_ADAT,     "ADAT"     },

  { 2, 0, PC_MIX, -1,               "Inputs"   },
  { 2, 1, PC_MIX, -1,               "Outputs"  }
};

// does an endpoint of this category and hardware type belong in this
// group? the hardware type only matters to the PC_HW groups
static int endpoint_in_group(
  const struct group_layout *layout,
  int                        category,
  int                        hw_type
) {
  if (category != layout->category)
    return 0;

  return layout->hw_type < 0 || hw_type == layout->hw_type;
}

// Split a meter label like "Source Analogue 1", "Source Mix A" or
// "Sink Analogue 12" into the group name and the channel number. The
// number is zero for labels that don't end in one, and the caller
// numbers those meters by their position in the group.
static char *split_meter_label(const char *label, int *num) {
  *num = 0;

  int idx = strlen(label) - 1;

  if (idx < 2)
    return g_strdup(label);

  if (isdigit(label[idx])) {
    while (idx > 1 && isdigit(label[idx - 1]))
      idx--;
    *num = atoi(&label[idx]);
  } else if (label[idx] >= 'A' && label[idx] <= 'Z') {
    *num = label[idx] - 'A' + 1;
  } else {
    return g_strdup(label);
  }

  // the trailing characters are part of the name unless a space
  // separates them from it
  if (label[idx - 1] != ' ') {
    *num = 0;
    return g_strdup(label);
  }

  return g_strndup(label, idx - 1);
}

static struct group_spec *add_group(GArray *specs, char *name, int row) {
  struct group_spec spec = { name, row, 0 };

  g_array_append_val(specs, spec);

  return &g_array_index(specs, struct group_spec, specs->len - 1);
}

// Groups for the metered channels that no routing endpoint claims, so
// that nothing the hardware meters goes unshown. These meters have no
// endpoint and read their own channel of the meter element.
static void add_unclaimed_groups(struct levels *data, GArray *specs) {
  struct alsa_elem *elem = data->level_meter_elem;
  int endpoint_meters = data->meter_count;
  struct group_spec *spec = NULL;
  char *current = NULL;

  for (int i = 0; i < elem->count; i++) {
    int claimed = 0;

    for (int j = 0; j < endpoint_meters && !claimed; j++) {
      struct level_meter *m = &data->meters[j];

      claimed = (m->snk && m->snk->level_index == i) ||
                (m->src && m->src->level_index == i);
    }

    if (claimed)
      continue;

    int num = 0;
    char *name = elem->meter_labels
      ? split_meter_label(elem->meter_labels[i], &num)
      : g_strdup("Other");

    if (!current || strcmp(current, name)) {
      g_free(current);
      current = g_strdup(name);
      spec = add_group(specs, g_strdup(name), OTHER_ROW);
    }

    struct level_meter *meter = &data->meters[data->meter_count++];

    meter->level_index = i;
    spec->count++;
    meter->num = num ? num : spec->count;

    g_free(name);
  }

  g_free(current);
}

// Build the groups the layout calls for, one meter per routing
// endpoint. Endpoints the hardware meters show their own level; the
// rest show the level of whatever they are routed to.
static GArray *build_groups(struct levels *data) {
  struct alsa_card *card = data->card;
  GArray *specs = g_array_new(FALSE, TRUE, sizeof(struct group_spec));

  for (int i = 0; i < G_N_ELEMENTS(group_layouts); i++) {
    const struct group_layout *layout = &group_layouts[i];
    struct group_spec *spec = add_group(
      specs, g_strdup(layout->name), layout->row
    );

    if (layout->is_src) {
      for (int j = 0; j < card->routing_srcs->len; j++) {
        struct routing_src *r_src = &g_array_index(
          card->routing_srcs, struct routing_src, j
        );

        if (!endpoint_in_group(layout, r_src->port_category, r_src->hw_type))
          continue;

        struct level_meter *meter = &data->meters[data->meter_count++];

        meter->src = r_src;
        meter->num = ++spec->count;
      }
    } else {
      for (int j = 0; j < card->routing_snks->len; j++) {
        struct routing_snk *r_snk = &g_array_index(
          card->routing_snks, struct routing_snk, j
        );

        if (!r_snk->elem ||
            !endpoint_in_group(
              layout, r_snk->elem->port_category, r_snk->elem->hw_type
            ))
          continue;

        struct level_meter *meter = &data->meters[data->meter_count++];

        meter->snk = r_snk;
        meter->num = ++spec->count;
      }
    }

    // a group the card has no ports for isn't shown at all
    if (!spec->count) {
      g_free(spec->name);
      g_array_remove_index(specs, specs->len - 1);
    }
  }

  add_unclaimed_groups(data, specs);

  return specs;
}

// A meter is shown when its port is enabled in the I/O configuration
// and available with the current Digital I/O mode and sample rate.
static int meter_enabled(struct level_meter *meter) {
  if (meter->snk)
    return is_routing_snk_enabled(meter->snk) &&
           is_routing_snk_available(meter->snk);
  if (meter->src)
    return is_routing_src_enabled(meter->src) &&
           is_routing_src_available(meter->src);

  return 1;
}

static int meter_linked(struct level_meter *meter) {
  if (meter->snk)
    return is_snk_linked(meter->snk);
  if (meter->src)
    return is_src_linked(meter->src);

  return 0;
}

static int meter_is_left_channel(struct level_meter *meter) {
  if (meter->snk)
    return is_snk_left_channel(meter->snk);
  if (meter->src)
    return is_src_left_channel(meter->src);

  return 0;
}

// What the meter is showing the level of; a linked channel is
// described by its pair's name rather than its own. A mixer input is
// named after the source feeding it, its own name saying no more than
// the group title and channel number already do.
static char *meter_display_name(struct level_meter *meter) {
  if (meter->snk) {
    if (meter->snk->elem->port_category == PC_MIX) {
      char *name = get_snk_source_display_name(meter->snk);

      if (name)
        return name;
    }

    return is_snk_linked(meter->snk)
      ? get_snk_pair_display_name(meter->snk)
      : g_strdup(meter->snk->display_name);
  }

  if (meter->src)
    return is_src_linked(meter->src)
      ? get_src_pair_display_name(meter->src)
      : g_strdup(meter->src->display_name);

  return NULL;
}

// the meter of this meter's stereo partner, or NULL if it has none
static struct level_meter *meter_partner(
  struct levels      *data,
  struct level_meter *meter
) {
  struct routing_snk *snk = meter->snk ? get_snk_partner(meter->snk) : NULL;
  struct routing_src *src = meter->src ? get_src_partner(meter->src) : NULL;

  if (!snk && !src)
    return NULL;

  for (int i = 0; i < data->meter_count; i++)
    if ((snk && data->meters[i].snk == snk) ||
        (src && data->meters[i].src == src))
      return &data->meters[i];

  return NULL;
}

// pair names abbreviated to fit in the space two channel numbers take
static const struct {
  const char *name;
  const char *label;
} pair_labels[] = {
  { "Spkr/Headphones", "Mon" },
  { "Bluetooth",       "BT"  }
};

// how many characters of a pair name fit in that space
#define MAX_PAIR_LABEL_LEN 4

// A name for a linked pair of analogue outputs, in place of its two
// channel numbers: an abbreviation where the pair has one, "HP" or
// "HP<n>" for a headphone output, or the pair's own name where that is
// short enough. NULL when the numbers say it better. Only the analogue
// outputs are named this way; everywhere else the numbers are what
// identifies a channel.
static char *meter_pair_label(struct level_meter *meter) {
  if (!meter->snk)
    return NULL;

  struct alsa_elem *elem = meter->snk->elem;

  if (elem->port_category != PC_HW || elem->hw_type != HW_TYPE_ANALOGUE)
    return NULL;

  char *name = get_snk_default_pair_name(meter->snk);

  if (!name)
    return NULL;

  for (int i = 0; i < G_N_ELEMENTS(pair_labels); i++) {
    if (strcmp(name, pair_labels[i].name))
      continue;

    g_free(name);

    return g_strdup(pair_labels[i].label);
  }

  int num = get_snk_pair_headphone_num(meter->snk);

  if (num >= 0) {
    g_free(name);

    return num ? g_strdup_printf("HP%d", num) : g_strdup("HP");
  }

  if (g_utf8_strlen(name, -1) <= MAX_PAIR_LABEL_LEN)
    return name;

  g_free(name);

  return NULL;
}

// Re-attach a label so it spans the given number of meters. The grid
// holds the only reference to it, so hold one of our own across the
// move.
static void set_label_span(struct level_meter *meter, int span) {
  if (meter->label_span == span)
    return;

  meter->label_span = span;

  g_object_ref(meter->num_label);
  gtk_grid_remove(GTK_GRID(meter->grid), meter->num_label);
  gtk_grid_attach(
    GTK_GRID(meter->grid), meter->num_label, meter->col, 1, span, 1
  );
  g_object_unref(meter->num_label);
}

// Label a meter with its channel number, or a linked pair of headphone
// outputs with one name across both of them.
static void update_meter_label(
  struct level_meter *meter,
  int                 paired,
  int                 left,
  int                 margin_start,
  int                 margin_end
) {
  char *pair = paired ? meter_pair_label(meter) : NULL;

  if (pair && !left) {
    gtk_widget_set_visible(meter->num_label, FALSE);
    g_free(pair);
    return;
  }

  if (pair) {
    gtk_label_set_text(GTK_LABEL(meter->num_label), pair);
    set_label_span(meter, 2);

    // the label covers both meters, so it takes the pair's outer
    // margins rather than this meter's
    gtk_widget_set_margin_start(meter->num_label, margin_start);
    gtk_widget_set_margin_end(meter->num_label, METER_GAP);

    g_free(pair);
    return;
  }

  char s[20];

  snprintf(s, sizeof(s), "%d", meter->num);
  gtk_label_set_text(GTK_LABEL(meter->num_label), s);
  set_label_span(meter, 1);

  gtk_widget_set_margin_start(meter->num_label, margin_start);
  gtk_widget_set_margin_end(meter->num_label, margin_end);
}

// whether any of a group's ports are enabled and available; a group
// with none is not shown at all, collapsed or not
static int group_has_enabled_meters(
  struct levels      *data,
  struct meter_group *group
) {
  for (int i = 0; i < group->meter_count; i++)
    if (meter_enabled(&data->meters[group->first_meter + i]))
      return 1;

  return 0;
}

// Apply the I/O configuration: disabled ports get no meter, and the
// meters of a stereo pair butt up against each other. A group with no
// enabled ports goes away entirely.
static void update_levels_layout(struct levels *data) {
  for (int i = 0; i < data->meter_count; i++) {
    struct level_meter *meter = &data->meters[i];

    if (!meter->widget)
      continue;

    int visible = meter_enabled(meter);

    gtk_widget_set_visible(meter->widget, visible);
    gtk_widget_set_visible(meter->num_label, visible);

    if (!visible)
      continue;

    struct level_meter *partner = meter_partner(data, meter);
    int paired = meter_linked(meter) && partner && meter_enabled(partner);
    int left = meter_is_left_channel(meter);

    int margin_start = paired && !left ? PAIR_GAP : METER_GAP;
    int margin_end = paired && left ? PAIR_GAP : METER_GAP;

    gtk_widget_set_margin_start(meter->widget, margin_start);
    gtk_widget_set_margin_end(meter->widget, margin_end);

    update_meter_label(meter, paired, left, margin_start, margin_end);

    char *name = meter_display_name(meter);

    if (name)
      gtk_widget_set_tooltip_text(meter->widget, name);

    g_free(name);
  }

  int row_visible[METER_ROW_COUNT] = { 0 };

  for (int i = 0; i < data->groups->len; i++) {
    struct meter_group *group = &g_array_index(
      data->groups, struct meter_group, i
    );
    int visible = group_has_enabled_meters(data, group);

    // a collapsed group is represented by its button in the row title
    // instead of by its box, and neither is shown while the row itself
    // is collapsed
    int expanded = gtk_expander_get_expanded(GTK_EXPANDER(group->expander));
    int row_open = gtk_expander_get_expanded(
      GTK_EXPANDER(data->row_boxes[group->row])
    );

    gtk_widget_set_visible(group->widget, visible && expanded);
    gtk_widget_set_visible(group->chip, visible && !expanded && row_open);

    if (visible)
      row_visible[group->row] = 1;
  }

  // A row with nothing in it takes up no space, title and all. A rule
  // goes between two rows only when both are showing their meters.
  int prev_expanded = 0;

  for (int i = 0; i < METER_ROW_COUNT; i++) {
    gtk_widget_set_visible(data->row_boxes[i], row_visible[i]);

    int expanded = row_visible[i] &&
      gtk_expander_get_expanded(GTK_EXPANDER(data->row_boxes[i]));

    if (data->row_seps[i])
      gtk_widget_set_visible(
        data->row_seps[i], expanded && prev_expanded
      );

    if (row_visible[i])
      prev_expanded = expanded;
  }
}

// Called when the Digital I/O mode or sample rate changes the set of
// available ports
void update_levels_availability(struct alsa_card *card) {
  if (card->levels_data)
    update_levels_layout(card->levels_data);
}

static void levels_layout_changed(struct alsa_elem *elem, void *user_data) {
  update_levels_layout(user_data);
}

static void watch_elem(struct levels *data, struct alsa_elem *elem) {
  if (!elem)
    return;

  alsa_elem_add_callback(elem, levels_layout_changed, data, NULL);
  g_ptr_array_add(data->watched_elems, elem);
}

// watch the elements that decide whether a meter is displayed, how it
// is spaced, and what its tooltip says
static void watch_meter_elems(struct levels *data, struct level_meter *meter) {
  if (meter->snk) {
    // the routing element itself, as a mixer input is named after the
    // source feeding it
    watch_elem(data, meter->snk->elem);
    watch_elem(data, meter->snk->enable_elem);
    watch_elem(data, meter->snk->link_elem);
    watch_elem(data, meter->snk->custom_name_elem);
    watch_elem(data, meter->snk->pair_name_elem);
  } else if (meter->src) {
    watch_elem(data, meter->src->enable_elem);
    watch_elem(data, meter->src->link_elem);
    watch_elem(data, meter->src->custom_name_elem);
    watch_elem(data, meter->src->pair_name_elem);
  }
}

// the state file key for a row's expanded state
static char *make_row_key(int row) {
  return g_strdup_printf("levels-row-%s", row_ids[row]);
}

// the state file key for a group's expanded state: the row it is on
// and its name reduced to lower case with the punctuation of names
// like "S/PDIF" replaced
static char *make_group_key(int row, const char *name) {
  char *slug = g_ascii_strdown(name, -1);

  for (char *c = slug; *c; c++)
    if (!g_ascii_isalnum(*c))
      *c = '-';

  char *key = g_strdup_printf("levels-group-%s-%s", row_ids[row], slug);

  g_free(slug);

  return key;
}

// whether a collapsible section is expanded: what the state file says,
// or the given default if it says nothing about it
static gboolean state_expanded(
  GHashTable *state,
  const char *key,
  gboolean    def
) {
  if (!state)
    return def;

  const char *value = g_hash_table_lookup(state, key);

  if (!value)
    return def;

  return strcmp(value, "true") == 0;
}

// whether a row is expanded
static gboolean row_expanded(GHashTable *state, int row) {
  char *key = make_row_key(row);
  gboolean expanded = state_expanded(state, key, row_expanded_default[row]);

  g_free(key);

  return expanded;
}

// A group collapses by its title, keeping its place in the row so the
// meters beside it don't move.
// whether any other group on the row has meters to show and is
// expanded to show them
static int row_has_other_expanded_group(
  struct levels *data,
  GtkWidget     *expander,
  int            row
) {
  for (int i = 0; i < data->groups->len; i++) {
    struct meter_group *group = &g_array_index(
      data->groups, struct meter_group, i
    );

    if (group->row != row || group->expander == expander)
      continue;

    if (group_has_enabled_meters(data, group) &&
        gtk_expander_get_expanded(GTK_EXPANDER(group->expander)))
      return 1;
  }

  return 0;
}

static void group_expanded_changed(
  GtkExpander *expander,
  GParamSpec  *pspec,
  gpointer     user_data
) {
  struct group_state *gs = user_data;
  struct levels *data = gs->data;
  gboolean expanded = gtk_expander_get_expanded(expander);

  // collapsing the last group of a row would leave the row expanded
  // with nothing in it, so collapse the row and leave the group alone
  if (!expanded &&
      !row_has_other_expanded_group(data, GTK_WIDGET(expander), gs->row)) {
    gtk_expander_set_expanded(expander, TRUE);
    gtk_expander_set_expanded(
      GTK_EXPANDER(data->row_boxes[gs->row]), FALSE
    );
    return;
  }

  gtk_widget_set_vexpand(GTK_WIDGET(expander), expanded);

  update_levels_layout(gs->data);

  optional_state_save(
    gs->data->card, CONFIG_SECTION_UI, gs->key, expanded ? "true" : "false"
  );
}

// the row title's button for a collapsed group brings the group back
static void group_chip_clicked(GtkButton *button, gpointer user_data) {
  gtk_expander_set_expanded(GTK_EXPANDER(user_data), TRUE);
}

// a button that looks like a collapsed expander: an arrow and the
// group's name
static GtkWidget *create_group_chip(const char *name, GtkWidget *expander) {
  GtkWidget *contents = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  gtk_box_append(
    GTK_BOX(contents), gtk_image_new_from_icon_name("pan-end-symbolic")
  );

  GtkWidget *label = gtk_label_new(name);
  gtk_widget_add_css_class(label, "meter-group-title");
  gtk_box_append(GTK_BOX(contents), label);

  GtkWidget *button = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(button), contents);
  gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
  gtk_widget_add_css_class(button, "meter-group-chip");

  g_signal_connect(
    button, "clicked", G_CALLBACK(group_chip_clicked), expander
  );

  return button;
}

static void group_state_free(void *user_data, GClosure *closure) {
  struct group_state *gs = user_data;

  g_free(gs->key);
  g_free(gs);
}

/* Name a meter's region in the geometry export by what it meters: the
 * routing endpoint where it has one, otherwise the hardware's own label
 * for the metered channel. Endpoint names match those the routing
 * window exports, so documentation of a port can point at both.
 */
static void register_meter_doc(
  struct levels      *data,
  struct level_meter *meter
) {
  if (meter->snk) {
    widget_doc_register(meter->widget, meter->snk->elem);
    return;
  }

  if (meter->src) {
    widget_doc_register_named(meter->widget, meter->src->name);
    return;
  }

  char **labels = data->level_meter_elem->meter_labels;

  if (labels && meter->level_index >= 0)
    widget_doc_register_named(meter->widget, labels[meter->level_index]);
}

static void create_meter_group(
  struct levels      *data,
  struct meter_group *group,
  GHashTable         *state,
  const char         *name
) {
  int row = group->row;
  int first_meter = group->first_meter;
  int meter_count = group->meter_count;

  GtkWidget *box = group->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(box, "controls-content");
  gtk_widget_add_css_class(box, "meter-group");

  GtkWidget *title = gtk_label_new(name);
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_widget_add_css_class(title, "meter-group-title");

  struct group_state *gs = g_malloc0(sizeof(struct group_state));

  gs->data = data;
  gs->key = make_group_key(row, name);
  gs->row = row;

  gboolean expanded = state_expanded(state, gs->key, TRUE);

  GtkWidget *expander = group->expander = gtk_expander_new(NULL);
  gtk_expander_set_label_widget(GTK_EXPANDER(expander), title);
  gtk_expander_set_expanded(GTK_EXPANDER(expander), expanded);
  gtk_widget_set_vexpand(expander, expanded);
  gtk_box_append(GTK_BOX(box), expander);

  g_signal_connect_data(
    expander, "notify::expanded",
    G_CALLBACK(group_expanded_changed), gs, group_state_free, 0
  );

  group->chip = create_group_chip(name, expander);
  gtk_box_append(GTK_BOX(data->row_chip_boxes[row]), group->chip);
  gtk_box_append(GTK_BOX(data->rows[row]), box);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
  gtk_expander_set_child(GTK_EXPANDER(expander), grid);

  // one scale for the whole group; it shares the meters' layout, so its
  // labels line up with their tick marks
  GtkWidget *scale = gtk_level_meter_scale_new();
  gtk_widget_set_vexpand(scale, TRUE);
  gtk_widget_set_margin_end(scale, METER_GAP);
  gtk_grid_attach(GTK_GRID(grid), scale, 0, 0, 1, 1);

  for (int i = 0; i < meter_count; i++) {
    struct level_meter *meter = &data->meters[first_meter + i];

    meter->widget = gtk_level_meter_new();
    gtk_widget_set_vexpand(meter->widget, TRUE);
    gtk_level_meter_set_peak_hold(
      GTK_LEVEL_METER(meter->widget), PEAK_HOLD_MS
    );
    gtk_grid_attach(GTK_GRID(grid), meter->widget, i + 1, 0, 1, 1);
    register_meter_doc(data, meter);

    char s[20];
    snprintf(s, sizeof(s), "%d", meter->num);
    meter->num_label = gtk_label_new(s);
    gtk_widget_add_css_class(meter->num_label, "meter-number");
    meter->grid = grid;
    meter->col = i + 1;
    meter->label_span = 1;
    gtk_grid_attach(GTK_GRID(grid), meter->num_label, meter->col, 1, 1, 1);

    watch_meter_elems(data, meter);
  }
}

static void on_destroy(struct levels *data, GtkWidget *widget) {
  // timer is cancelled in card_destroy_callback before we get here
  for (int i = 0; i < data->watched_elems->len; i++)
    alsa_elem_remove_callbacks_by_data(
      g_ptr_array_index(data->watched_elems, i), data
    );

  data->card->levels_data = NULL;

  g_ptr_array_free(data->watched_elems, TRUE);
  g_array_free(data->groups, TRUE);
  g_free(data->meters);
  g_free(data);
}

// The rule between two rows, spanning the middle half of the window:
// the columns are of equal width, and the rule covers the middle two
// of the four.
static GtkWidget *create_row_separator(void) {
  GtkWidget *grid = gtk_grid_new();

  gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
  gtk_widget_set_margin_top(grid, 5);
  gtk_widget_set_margin_bottom(grid, 5);

  gtk_grid_attach(
    GTK_GRID(grid), gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), 0, 0, 1, 1
  );
  gtk_grid_attach(
    GTK_GRID(grid),
    gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 1, 0, 2, 1
  );
  gtk_grid_attach(
    GTK_GRID(grid), gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), 3, 0, 1, 1
  );

  return grid;
}

static void row_expanded_changed(
  GtkExpander *expander,
  GParamSpec  *pspec,
  gpointer     user_data
) {
  struct levels *data = user_data;
  gboolean expanded = gtk_expander_get_expanded(expander);

  gtk_widget_set_vexpand(GTK_WIDGET(expander), expanded);

  // the rules above and below this row depend on its state
  update_levels_layout(data);

  for (int i = 0; i < METER_ROW_COUNT; i++) {
    if (data->row_boxes[i] != GTK_WIDGET(expander))
      continue;

    char *key = make_row_key(i);

    optional_state_save(
      data->card, CONFIG_SECTION_UI, key, expanded ? "true" : "false"
    );

    g_free(key);
    return;
  }
}

GtkWidget *create_levels_controls(struct alsa_card *card) {
  struct alsa_elem *level_meter_elem = get_elem_by_name(
    card->elems, "Level Meter"
  );

  if (!level_meter_elem) {
    printf("Level Meter control not found\n");
    return NULL;
  }

  struct levels *data = g_malloc0(sizeof(struct levels));

  data->card = card;
  data->level_meter_elem = level_meter_elem;
  // one meter per routing endpoint, plus one for every metered channel
  // that no endpoint claims
  int max_meters = level_meter_elem->count +
                   card->routing_srcs->len + card->routing_snks->len;

  data->meters = g_malloc0(max_meters * sizeof(struct level_meter));

  for (int i = 0; i < max_meters; i++)
    data->meters[i].level_index = -1;
  data->groups = g_array_new(FALSE, TRUE, sizeof(struct meter_group));
  data->watched_elems = g_ptr_array_new();

  GtkWidget *top = data->top = gtk_frame_new(NULL);
  gtk_widget_add_css_class(top, "window-frame");

  GtkWidget *box = data->box = gtk_box_new(
    GTK_ORIENTATION_VERTICAL, GROUP_SPACING
  );
  gtk_frame_set_child(GTK_FRAME(top), box);

  GHashTable *state = optional_state_load(card, CONFIG_SECTION_UI);

  // each row is titled by what its groups have in common and collapses
  // by its title, and a rule separates it from the row below; each
  // group in a row is as wide as its own meters
  for (int i = 0; i < METER_ROW_COUNT; i++) {
    if (i) {
      data->row_seps[i] = create_row_separator();
      gtk_box_append(GTK_BOX(box), data->row_seps[i]);
    }

    GtkWidget *title = gtk_label_new(row_titles[i]);
    gtk_widget_add_css_class(title, "controls-label");
    gtk_widget_add_css_class(title, "meter-row-title");

    // the row's title, then a button for each of its collapsed groups
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, GROUP_SPACING);
    gtk_box_append(GTK_BOX(header), title);

    data->row_chip_boxes[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_append(GTK_BOX(header), data->row_chip_boxes[i]);

    gboolean expanded = row_expanded(state, i);

    data->row_boxes[i] = gtk_expander_new(NULL);
    gtk_expander_set_label_widget(GTK_EXPANDER(data->row_boxes[i]), header);
    gtk_expander_set_expanded(GTK_EXPANDER(data->row_boxes[i]), expanded);

    // a collapsed row claims none of the vertical space
    gtk_widget_set_vexpand(data->row_boxes[i], expanded);

    g_signal_connect(
      data->row_boxes[i], "notify::expanded",
      G_CALLBACK(row_expanded_changed), data
    );

    data->rows[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, GROUP_SPACING);
    gtk_widget_set_vexpand(data->rows[i], TRUE);
    gtk_expander_set_child(GTK_EXPANDER(data->row_boxes[i]), data->rows[i]);

    // the expander rather than its content, so a crop of the row takes
    // in the title above it
    widget_doc_register_group(
      data->row_boxes[i], row_ids[i], NULL, row_titles[i]
    );

    gtk_box_append(GTK_BOX(box), data->row_boxes[i]);
  }

  GArray *specs = build_groups(data);

  int first_meter = 0;

  for (int i = 0; i < specs->len; i++) {
    struct group_spec *spec = &g_array_index(specs, struct group_spec, i);

    struct meter_group group = {
      .row = spec->row,
      .first_meter = first_meter,
      .meter_count = spec->count
    };

    create_meter_group(data, &group, state, spec->name);

    g_array_append_val(data->groups, group);

    first_meter += spec->count;
    g_free(spec->name);
  }

  g_array_free(specs, TRUE);

  if (state)
    g_hash_table_destroy(state);

  // a restored state with every group of a row collapsed would show
  // the row expanded and empty; collapse the row instead
  for (int i = 0; i < METER_ROW_COUNT; i++) {
    if (row_has_other_expanded_group(data, NULL, i))
      continue;

    int changed = 0;

    for (int j = 0; j < data->groups->len; j++) {
      struct meter_group *group = &g_array_index(
        data->groups, struct meter_group, j
      );

      if (group->row != i || !group_has_enabled_meters(data, group))
        continue;

      gtk_expander_set_expanded(GTK_EXPANDER(group->expander), TRUE);
      changed = 1;
    }

    // a row with nothing to show is left as it is; it isn't displayed
    if (changed)
      gtk_expander_set_expanded(GTK_EXPANDER(data->row_boxes[i]), FALSE);
  }

  update_levels_layout(data);

  card->levels_data = data;
  card->levels_timer = g_timeout_add(
    card->pref_levels_interval_ms, update_levels_controls, data
  );
  g_object_weak_ref(G_OBJECT(box), (GWeakNotify)on_destroy, data);

  return top;
}

void restart_levels_timer(struct alsa_card *card) {
  if (!card->levels_data)
    return;

  if (card->levels_timer) {
    g_source_remove(card->levels_timer);
    card->levels_timer = 0;
  }

  card->levels_timer = g_timeout_add(
    card->pref_levels_interval_ms,
    update_levels_controls,
    card->levels_data
  );
}
