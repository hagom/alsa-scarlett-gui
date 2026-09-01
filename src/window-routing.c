// SPDX-FileCopyrightText: 2022-2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gtkhelper.h"
#include "hw-io-availability.h"
#include "iface-mixer.h"
#include "routing-drag-line.h"
#include "routing-lines.h"
#include "routing-port-menu.h"
#include "stereo-link.h"
#include "stringhelper.h"
#include "widget-boolean.h"
#include "widget-rotated-label.h"
#include "widget-text-entry.h"
#include "window-geometry.h"
#include "window-levels.h"
#include "window-mixer.h"
#include "window-routing.h"
#include "custom-names.h"
#include "port-enable.h"

// clear all the routing sinks
static void routing_preset_clear(struct alsa_card *card) {
  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *r_snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );

    alsa_set_elem_value(r_snk->elem, 0);
  }
}

static void routing_preset_link(
  struct alsa_card *card,
  int               src_port_category,
  int               src_mod,
  int               snk_port_category
) {

  // find the first src port with the selected port category
  int start_src_idx;
  for (start_src_idx = 1;
       start_src_idx < card->routing_srcs->len;
       start_src_idx++) {
    struct routing_src *r_src = &g_array_index(
      card->routing_srcs, struct routing_src, start_src_idx
    );

    if (r_src->port_category == src_port_category)
      break;
  }

  // find the first snk port with the selected port category
  int snk_idx;
  for (snk_idx = 0;
       snk_idx < card->routing_snks->len;
       snk_idx++) {
    struct routing_snk *r_snk = &g_array_index(
      card->routing_snks, struct routing_snk, snk_idx
    );
    struct alsa_elem *elem = r_snk->elem;

    if (elem->port_category == snk_port_category)
      break;
  }

  // start assigning
  int src_idx = start_src_idx;
  int src_count = 0;
  while (src_idx < card->routing_srcs->len &&
         snk_idx < card->routing_snks->len) {

    // stop if no more of the selected src port category
    struct routing_src *r_src = &g_array_index(
      card->routing_srcs, struct routing_src, src_idx
    );
    if (r_src->port_category != src_port_category)
      break;

    // stop if no more of the selected snk port category
    struct routing_snk *r_snk = &g_array_index(
      card->routing_snks, struct routing_snk, snk_idx
    );
    struct alsa_elem *elem = r_snk->elem;

    if (elem->port_category != snk_port_category)
      break;

    // do the assignment
    alsa_set_elem_value(elem, r_src->id);

    // get the next index
    src_idx++;
    src_count++;
    snk_idx++;

    if (src_count == src_mod) {
      src_idx = start_src_idx;
      src_count = 0;
    }
  }
}

static void routing_preset_direct(struct alsa_card *card) {
  routing_preset_link(card, PC_HW, 0, PC_PCM);
  routing_preset_link(card, PC_PCM, 0, PC_HW);
}

static void routing_preset_preamp(struct alsa_card *card) {
  routing_preset_link(card, PC_HW, 0, PC_HW);
}

static void routing_preset_stereo_out(struct alsa_card *card) {
  routing_preset_link(card, PC_PCM, 2, PC_HW);
}

static void routing_preset(
  GSimpleAction    *action,
  GVariant         *value,
  struct alsa_card *card
) {
  const char *s = g_variant_get_string(value, NULL);

  if (strcmp(s, "clear") == 0) {
    routing_preset_clear(card);
  } else if (strcmp(s, "direct") == 0) {
    routing_preset_direct(card);
  } else if (strcmp(s, "preamp") == 0) {
    routing_preset_preamp(card);
  } else if (strcmp(s, "stereo_out") == 0) {
    routing_preset_stereo_out(card);
  }
}

static GtkWidget *make_preset_menu_button(struct alsa_card *card) {
  GMenu *menu = g_menu_new();

  g_menu_append(menu, "Clear", "routing.preset('clear')");
  g_menu_append(menu, "Direct", "routing.preset('direct')");
  g_menu_append(menu, "Preamp", "routing.preset('preamp')");
  g_menu_append(menu, "Stereo Out", "routing.preset('stereo_out')");

  GtkWidget *button = gtk_menu_button_new();
  gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_menu_button_set_label(GTK_MENU_BUTTON(button), "Presets");
  gtk_menu_button_set_menu_model(
    GTK_MENU_BUTTON(button),
    G_MENU_MODEL(menu)
  );

  GSimpleActionGroup *action_group = g_simple_action_group_new();
  GSimpleAction *action = g_simple_action_new_stateful(
    "preset", G_VARIANT_TYPE_STRING, NULL
  );
  g_action_map_add_action(G_ACTION_MAP(action_group), G_ACTION(action));
  g_signal_connect(
    action, "activate", G_CALLBACK(routing_preset), card
  );
  gtk_widget_insert_action_group(
    button, "routing", G_ACTION_GROUP(action_group)
  );

  return button;
}

// Forward declaration for hidden connection popover teardown
static void hide_hidden_peer_popover(struct alsa_card *card);

// Forward declaration for highlight drawing
static void draw_group_highlights(
  GtkDrawingArea *drawing_area,
  cairo_t        *cr,
  int             width,
  int             height,
  void           *user_data
);

// Ports of one routing group, for its heading's show/hide menu
struct group_menu_data {
  struct alsa_card *card;
  int               is_src;
  int               port_category;
};

// The menu is built when the button is clicked so that it always shows
// the current state
static void group_menu_create_popup(GtkMenuButton *button, gpointer user_data) {
  struct group_menu_data *data = user_data;

  gtk_menu_button_set_popover(
    button,
    routing_port_menu_group_popover(
      data->card, data->is_src, data->port_category
    )
  );
}

// The group heading is a menu button: its arrow is the only thing telling
// the user that the ports can be shown and hidden
static GtkWidget *make_group_heading_button(
  GtkWidget        *label,
  const char       *tooltip,
  struct alsa_card *card,
  int               is_src,
  int               port_category
) {
  struct group_menu_data *data = g_new0(struct group_menu_data, 1);

  data->card = card;
  data->is_src = is_src;
  data->port_category = port_category;

  GtkWidget *button = gtk_menu_button_new();

  gtk_menu_button_set_child(GTK_MENU_BUTTON(button), label);
  gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(button), TRUE);
  gtk_menu_button_set_create_popup_func(
    GTK_MENU_BUTTON(button), group_menu_create_popup, data, g_free
  );

  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "group-heading");
  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_widget_set_halign(button, GTK_ALIGN_CENTER);

  return button;
}

// Create a routing group with heading label and grid for widgets.
// Returns a container (hbox or vbox) with heading outside the grid.
// grid_out receives the inner grid for attaching socket/label widgets at row/col 0+
// The grid is wrapped in an overlay with a highlight drawing area below.
static GtkWidget *create_routing_group_grid(
  struct alsa_card *card,
  char             *name,
  char             *doc_id,
  char             *descr,
  char             *tooltip,
  int               is_src,
  int               port_category,
  GtkOrientation    orientation,
  GtkAlign          align,
  GtkWidget       **heading_label,
  GtkWidget       **grid_out
) {
  GtkWidget *grid = gtk_grid_new();
  gtk_widget_set_name(grid, name);
  gtk_grid_set_spacing(GTK_GRID(grid), 2);
  // Add margin for highlight padding (2px) to avoid clipping
  gtk_widget_set_margin_start(grid, 2);
  gtk_widget_set_margin_end(grid, 2);
  gtk_widget_set_margin_top(grid, 2);
  gtk_widget_set_margin_bottom(grid, 2);

  // Create overlay with highlight drawing area below the grid
  GtkWidget *overlay = gtk_overlay_new();
  GtkWidget *highlight_area = gtk_drawing_area_new();
  gtk_drawing_area_set_draw_func(
    GTK_DRAWING_AREA(highlight_area), draw_group_highlights, card, NULL
  );
  gtk_widget_set_can_target(highlight_area, FALSE);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), highlight_area);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), grid);
  gtk_overlay_set_measure_overlay(GTK_OVERLAY(overlay), grid, TRUE);

  GtkWidget *label = gtk_label_new(descr);
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);

  char *full_tooltip = g_strdup_printf(
    "%s.\n\nClick to show or hide individual ports.", tooltip
  );
  GtkWidget *heading = make_group_heading_button(
    label, full_tooltip, card, is_src, port_category
  );
  g_free(full_tooltip);

  if (heading_label)
    *heading_label = label;

  if (grid_out)
    *grid_out = grid;

  GtkWidget *container;

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    // Horizontal: hbox with label_vbox on left, overlay on right
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(overlay, "controls-content");

    // Wrap heading in vbox so additional labels can be added (e.g. Talkback)
    GtkWidget *label_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(label_vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(heading, align);
    gtk_box_append(GTK_BOX(label_vbox), heading);

    gtk_box_append(GTK_BOX(hbox), label_vbox);
    gtk_box_append(GTK_BOX(hbox), overlay);
    container = hbox;
  } else {
    // Vertical: vbox with heading on top, overlay below
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_halign(vbox, GTK_ALIGN_FILL);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(vbox, TRUE);
    gtk_widget_set_halign(heading, align);
    gtk_widget_set_halign(overlay, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(overlay, "controls-content");

    gtk_box_append(GTK_BOX(vbox), heading);
    gtk_box_append(GTK_BOX(vbox), overlay);

    container = vbox;
  }

  /* The heading titles the section in the documentation geometry export.
   * Narrow columns break their heading over two lines, which a title
   * does not want.
   */
  char *doc_title = g_strdup(descr);

  g_strdelimit(doc_title, "\n", ' ');
  widget_doc_register_group(container, doc_id, NULL, doc_title);
  g_free(doc_title);

  return container;
}

static void create_routing_grid(struct alsa_card *card) {
  GtkGrid *routing_grid = GTK_GRID(card->routing_grid = gtk_grid_new());

  int has_dsp = !!card->routing_in_count[PC_DSP];

  gtk_widget_set_halign(card->routing_grid, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(card->routing_grid, GTK_ALIGN_CENTER);

  GtkWidget *preset_menu_button = make_preset_menu_button(card);
  gtk_grid_attach(
    routing_grid, preset_menu_button, 0, 0, 1, 1
  );

  // All grids: container is vbox/hbox, grid is separate for widget attachment
  GtkWidget *hw_in_container = create_routing_group_grid(
    card, "routing_hw_in_grid", "hardware-inputs", "Hardware Inputs",
    "Hardware Inputs are the physical inputs on the interface",
    1, PC_HW,
    GTK_ORIENTATION_VERTICAL, GTK_ALIGN_END, NULL,
    &card->routing_hw_in_grid
  );
  GtkWidget *pcm_in_container = create_routing_group_grid(
    card, "routing_pcm_in_grid", "pcm-outputs", "PCM Outputs",
    "PCM Outputs are the digital audio channels sent from the PC to "
    "the interface over USB, used for audio playback",
    1, PC_PCM,
    GTK_ORIENTATION_VERTICAL, GTK_ALIGN_END, NULL,
    &card->routing_pcm_in_grid
  );
  GtkWidget *pcm_out_container = create_routing_group_grid(
    card, "routing_pcm_out_grid", "pcm-inputs", "PCM Inputs",
    "PCM Inputs are the digital audio channels sent from the interface "
    "to the PC over USB, use for audio recording",
    0, PC_PCM,
    GTK_ORIENTATION_VERTICAL, GTK_ALIGN_START, NULL,
    &card->routing_pcm_out_grid
  );
  GtkWidget *hw_out_container = create_routing_group_grid(
    card, "routing_hw_out_grid", "hardware-outputs", "Hardware Outputs",
    "Hardware Outputs are the physical outputs on the interface",
    0, PC_HW,
    GTK_ORIENTATION_VERTICAL, GTK_ALIGN_START, NULL,
    &card->routing_hw_out_grid
  );

  GtkWidget *dsp_in_container = NULL;
  GtkWidget *dsp_out_container = NULL;
  GtkWidget *mixer_in_container = NULL;
  GtkWidget *mixer_out_container = NULL;

  if (has_dsp) {
    dsp_in_container = create_routing_group_grid(
      card, "routing_dsp_in_grid", "dsp-inputs", "DSP\nInputs",
      "DSP Inputs are used to send audio to the DSP, which is used for "
      "features such as the input level meters, Air mode, and Autogain",
      0, PC_DSP,
      GTK_ORIENTATION_HORIZONTAL, GTK_ALIGN_CENTER, NULL,
      &card->routing_dsp_in_grid
    );
    dsp_out_container = create_routing_group_grid(
      card, "routing_dsp_out_grid", "dsp-outputs", "DSP\nOutputs",
      "DSP Outputs are used to send audio from the DSP after it has "
      "done its processing",
      1, PC_DSP,
      GTK_ORIENTATION_HORIZONTAL, GTK_ALIGN_CENTER, NULL,
      &card->routing_dsp_out_grid
    );
  }
  if (!card->has_fixed_mixer_inputs)
    mixer_in_container = create_routing_group_grid(
      card, "routing_mixer_in_grid", "mixer-inputs", "Mixer\nInputs",
      "Mixer Inputs are used to mix multiple audio channels together",
      0, PC_MIX,
      GTK_ORIENTATION_HORIZONTAL, GTK_ALIGN_CENTER,
      &card->routing_mixer_in_heading, &card->routing_mixer_in_grid
    );
  mixer_out_container = create_routing_group_grid(
    card, "routing_mixer_out_grid", "mixer-outputs",
    card->has_talkback ? "Mixer Outputs" : "Mixer\nOutputs",
    "Mixer Outputs are used to send audio from the mixer",
    1, PC_MIX,
    GTK_ORIENTATION_HORIZONTAL, GTK_ALIGN_CENTER,
    &card->routing_mixer_out_heading, &card->routing_mixer_out_grid
  );

  int left_col_num = 0;
  int dsp_col_num = has_dsp ? 1 : 0;
  int mix_col_num = dsp_col_num + 1;
  int right_col_num = mix_col_num + 1;

  // set minimum width on the mixer column to maintain spacing even when empty
  gtk_grid_set_column_homogeneous(routing_grid, FALSE);
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(spacer, 200, 1);
  gtk_grid_attach(routing_grid, spacer, mix_col_num, 0, 1, 1);

  gtk_grid_attach(routing_grid, hw_in_container, left_col_num, 1, 1, 1);
  gtk_grid_attach(routing_grid, pcm_in_container, left_col_num, 2, 1, 1);
  gtk_grid_attach(routing_grid, pcm_out_container, right_col_num, 1, 1, 1);
  gtk_grid_attach(routing_grid, hw_out_container, right_col_num, 2, 1, 1);
  if (has_dsp) {
    gtk_grid_attach(routing_grid, dsp_in_container, dsp_col_num, 0, 1, 1);
    gtk_grid_attach(routing_grid, dsp_out_container, dsp_col_num, 3, 1, 1);
  }
  if (!card->has_fixed_mixer_inputs)
    gtk_grid_attach(routing_grid, mixer_in_container, mix_col_num, 0, 1, 1);
  gtk_grid_attach(routing_grid, mixer_out_container, mix_col_num, 3, 1, 1);
  gtk_grid_set_spacing(routing_grid, 10);

  card->routing_src_label = gtk_label_new("↑\nSources →");
  gtk_label_set_justify(GTK_LABEL(card->routing_src_label), GTK_JUSTIFY_CENTER);
  gtk_grid_attach(routing_grid, card->routing_src_label, left_col_num, 3, 1, 1);

  card->routing_snk_label = gtk_label_new(
    card->has_fixed_mixer_inputs ? "Sinks\n↓" : "← Sinks\n↓"
  );
  gtk_label_set_justify(GTK_LABEL(card->routing_snk_label), GTK_JUSTIFY_CENTER);
  gtk_grid_attach(routing_grid, card->routing_snk_label, right_col_num, 0, 1, 1);
}

// None of what can be done to a port shows on the port itself, unlike
// the group headings with their menu arrow, so every port says so
#define PORT_MENU_HINT \
  "Drag to connect\n" \
  "Left-click to remove connections\n" \
  "Right-click for options"

// Add the hint to whatever the label update left in the tooltip, and give
// the socket the same text so that both halves of the port read the same
static void set_port_menu_tooltip(GtkWidget *label, GtkWidget *socket) {
  const char *current = gtk_widget_get_tooltip_text(label);

  // the label update paths that leave the tooltip alone would otherwise
  // add the hint again on every update
  if (current && g_str_has_suffix(current, PORT_MENU_HINT)) {
    if (socket)
      gtk_widget_set_tooltip_text(socket, current);
    return;
  }

  char *tooltip = current
    ? g_strdup_printf("%s\n\n%s", current, PORT_MENU_HINT)
    : g_strdup(PORT_MENU_HINT);

  gtk_widget_set_tooltip_text(label, tooltip);
  if (socket)
    gtk_widget_set_tooltip_text(socket, tooltip);

  g_free(tooltip);
}

static GtkWidget *make_socket_widget(void) {
  GtkWidget *w = gtk_picture_new_for_resource(
    "/vu/b4/alsa-scarlett-gui/icons/socket.svg"
  );
  gtk_widget_set_tooltip_text(w, PORT_MENU_HINT);
  gtk_widget_set_align(w, GTK_ALIGN_CENTER, GTK_ALIGN_CENTER);
  gtk_picture_set_can_shrink(GTK_PICTURE(w), FALSE);
  gtk_widget_set_margin(w, 2);
  gtk_widget_set_vexpand(w, FALSE);
  gtk_widget_set_hexpand(w, FALSE);
  return w;
}

// Update socket widget to show mono or stereo based on link state
// orientation: GTK_ORIENTATION_HORIZONTAL for side-by-side sockets
//              GTK_ORIENTATION_VERTICAL for stacked sockets
static void update_socket_widget(
  GtkWidget      *socket,
  int             is_linked,
  GtkOrientation  orientation
) {
  const char *resource;

  if (is_linked) {
    resource = orientation == GTK_ORIENTATION_HORIZONTAL
      ? "/vu/b4/alsa-scarlett-gui/icons/socket-stereo-h.svg"
      : "/vu/b4/alsa-scarlett-gui/icons/socket-stereo-v.svg";
  } else {
    resource = "/vu/b4/alsa-scarlett-gui/icons/socket.svg";
  }

  gtk_picture_set_resource(GTK_PICTURE(socket), resource);
}

// Get the appropriate socket orientation for a port category
// This refers to how L/R are arranged in the stereo socket icon
static GtkOrientation get_socket_orientation(int port_category) {
  // Mixer and DSP ports are arranged horizontally, L/R side-by-side
  // HW and PCM ports are arranged vertically, L/R stacked
  return (port_category == PC_MIX || port_category == PC_DSP)
    ? GTK_ORIENTATION_HORIZONTAL
    : GTK_ORIENTATION_VERTICAL;
}

// Mixer outputs are labelled by letter unless they have a name of their
// own, from the device or from the user, which is the name the mixer
// window shows
static int src_is_named(struct routing_src *src) {
  if (!src || !src->display_name)
    return 0;

  char *generic = get_src_generic_name(src);
  int named = strcmp(src->display_name, generic) != 0;

  g_free(generic);

  return named;
}

// A linked pair can be named as a pair, which is stored separately from
// the names of the two channels
static int src_pair_is_named(struct routing_src *src) {
  struct alsa_elem *elem = get_src_pair_name_elem(src);

  if (!elem)
    return 0;

  size_t size;
  const void *bytes = alsa_get_elem_bytes(elem, &size);

  return bytes && size && strnlen(bytes, size) > 0;
}

int mix_src_is_named(struct routing_src *src) {
  if (src->port_category != PC_MIX)
    return 0;

  if (!is_src_linked(src))
    return src_is_named(src);

  return src_pair_is_named(src) ||
         src_is_named(src) ||
         src_is_named(get_src_partner(src));
}

// Get stereo-aware base name for a source
// Returns newly allocated string that must be freed
char *get_src_letter_name(struct routing_src *src) {
  if (is_src_linked(src) && is_src_left_channel(src))
    return g_strdup_printf(
      "%c\xe2\x80\x93%c", src->port_num + 'A', src->port_num + 'B'
    );

  return g_strdup_printf("%c", src->port_num + 'A');
}

char *get_src_stereo_aware_name(struct routing_src *src) {
  if (is_src_linked(src) && is_src_left_channel(src)) {
    if (src->port_category == PC_MIX)
      return mix_src_is_named(src)
        ? get_src_pair_display_name(src)
        : get_src_letter_name(src);
    if (src->port_category == PC_DSP)
      return g_strdup_printf(
        "%d\xe2\x80\x93%d", src->lr_num, src->lr_num + 1
      );
    return get_src_pair_display_name(src);
  }

  if (mix_src_is_named(src))
    return g_strdup(src->display_name);

  return get_src_display_name_formatted(src);
}

// Get stereo-aware base name for a sink
// Returns newly allocated string that must be freed
char *get_snk_stereo_aware_name(struct routing_snk *snk) {
  if (is_snk_linked(snk) && is_snk_left_channel(snk)) {
    // Mixer/DSP use simple abbreviated stereo labels in routing window
    int port_cat = snk->elem->port_category;
    if (port_cat == PC_MIX || port_cat == PC_DSP) {
      int lr = snk->elem->lr_num;
      return g_strdup_printf("%d–%d", lr, lr + 1);
    } else {
      return get_snk_pair_display_name(snk);
    }
  }
  return get_snk_display_name_formatted(snk);
}

// Label width for mono/stereo display
#define LABEL_WIDTH_MONO   3
#define LABEL_WIDTH_STEREO 4
#define LABEL_WIDTH_NAMED  8

// Helper to check if a source should be visible in routing grid
static int is_src_visible(struct routing_src *r_src) {
  return is_routing_src_enabled(r_src) && should_display_src(r_src);
}

// Helper to check if a sink should be visible in routing grid
static int is_snk_visible(struct routing_snk *r_snk) {
  return is_routing_snk_enabled(r_snk) && should_display_snk(r_snk);
}

// Helper to unparent a widget from a grid if it has a parent
static void unparent_from_grid(GtkWidget *grid, GtkWidget *widget) {
  if (widget && gtk_widget_get_parent(widget))
    gtk_grid_remove(GTK_GRID(grid), widget);
}

// Get the routing grid for a source port category
static GtkWidget *get_src_grid(struct alsa_card *card, int port_category) {
  switch (port_category) {
    case PC_MIX: return card->routing_mixer_out_grid;
    case PC_DSP: return card->routing_dsp_out_grid;
    case PC_HW:  return card->routing_hw_in_grid;
    case PC_PCM: return card->routing_pcm_in_grid;
    default:     return NULL;
  }
}

// Get the routing grid for a sink port category
static GtkWidget *get_snk_grid(struct alsa_card *card, int port_category) {
  switch (port_category) {
    case PC_MIX: return card->routing_mixer_in_grid;
    case PC_DSP: return card->routing_dsp_in_grid;
    case PC_HW:  return card->routing_hw_out_grid;
    case PC_PCM: return card->routing_pcm_out_grid;
    default:     return NULL;
  }
}

// Check if port category uses horizontal layout (MIX/DSP)
static int is_horiz_port_category(int port_category) {
  return port_category == PC_MIX || port_category == PC_DSP;
}

// Release refs held on routing widgets.
// Called before the routing window is destroyed.
void cleanup_routing_widgets(struct alsa_card *card) {
  // Clear hover state to prevent dangling pointers
  hide_hidden_peer_popover(card);
  card->hovered_src = NULL;
  card->hovered_snk = NULL;
  card->routing_overlay = NULL;

  // Clean up source widgets
  if (card->routing_srcs) {
    for (int i = 1; i < card->routing_srcs->len; i++) {
      struct routing_src *r_src = &g_array_index(
        card->routing_srcs, struct routing_src, i
      );

      if (r_src->widget) {
        g_object_unref(r_src->widget);
        r_src->widget = NULL;
        r_src->widget2 = NULL;
      }
      if (r_src->label_widget) {
        g_object_unref(r_src->label_widget);
        r_src->label_widget = NULL;
      }
      if (r_src->rotated_label_widget) {
        g_object_unref(r_src->rotated_label_widget);
        r_src->rotated_label_widget = NULL;
      }
      if (r_src->talkback_widget) {
        g_object_unref(r_src->talkback_widget);
        r_src->talkback_widget = NULL;
      }
    }
  }

  // Clean up sink widgets
  if (card->routing_snks) {
    for (int i = 0; i < card->routing_snks->len; i++) {
      struct routing_snk *r_snk = &g_array_index(
        card->routing_snks, struct routing_snk, i
      );

      if (r_snk->socket_widget) {
        g_object_unref(r_snk->socket_widget);
        r_snk->socket_widget = NULL;
      }
      if (r_snk->label_widget) {
        g_object_unref(r_snk->label_widget);
        r_snk->label_widget = NULL;
      }
    }
  }
}

// The label a source should be showing under its socket: a name goes in
// the label drawn at an angle, while the lettering fits as it is, and is
// already on the talkback button when there is one
static GtkWidget *get_src_label_to_show(struct routing_src *r_src) {
  if (mix_src_is_named(r_src))
    return r_src->rotated_label_widget;

  if (r_src->talkback_widget)
    return NULL;

  return r_src->label_widget;
}

// Natural width of a column of the source grid: the widest of the
// socket, the talkback button under it, and the label under that
static int src_column_width(struct routing_src *r_src) {
  int width = 0;

  GtkWidget *widgets[] = {
    r_src->widget, r_src->talkback_widget, get_src_label_to_show(r_src)
  };

  for (int i = 0; i < G_N_ELEMENTS(widgets); i++) {
    int natural;

    if (!widgets[i])
      continue;

    gtk_widget_measure(
      widgets[i], GTK_ORIENTATION_HORIZONTAL, -1,
      NULL, &natural, NULL, NULL
    );

    if (natural > width)
      width = natural;
  }

  return width;
}

// A rotated label reaches back past its own column, towards the start
// of the group. The columns before it provide that room; the group
// grows only by what the furthest-reaching name still needs beyond
// them.
static void update_src_grid_overhang(
  struct alsa_card *card,
  int               port_category
) {
  GtkWidget *grid = get_src_grid(card, port_category);

  if (!grid)
    return;

  int spacing = gtk_grid_get_column_spacing(GTK_GRID(grid));
  int overhang = 0;
  int x = 0;

  for (int i = 1; i < card->routing_srcs->len; i++) {
    struct routing_src *r_src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );

    if (r_src->port_category != port_category || !is_src_visible(r_src))
      continue;

    if (r_src->rotated_label_widget && mix_src_is_named(r_src)) {
      int past_start =
        rotated_label_get_overhang(r_src->rotated_label_widget) - x;

      if (past_start > overhang)
        overhang = past_start;
    }

    x += src_column_width(r_src) + spacing;
  }

  // 2px of the margin is the padding the highlight needs
  gtk_widget_set_margin_start(grid, 2 + overhang);
}

// Arrange source widgets in their grid.
// Removes all widgets and re-attaches only visible ones at consecutive positions.
void arrange_src_grid(struct alsa_card *card, int port_category) {
  GtkWidget *grid = get_src_grid(card, port_category);
  if (!grid)
    return;

  int is_horiz = is_horiz_port_category(port_category);
  int pos = 0;

  for (int i = 1; i < card->routing_srcs->len; i++) {
    struct routing_src *r_src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );

    if (r_src->port_category != port_category || !r_src->widget)
      continue;

    // Unparent all widgets
    unparent_from_grid(grid, r_src->widget);
    unparent_from_grid(grid, r_src->label_widget);
    unparent_from_grid(grid, r_src->rotated_label_widget);
    unparent_from_grid(grid, r_src->talkback_widget);

    if (!is_src_visible(r_src))
      continue;

    if (is_horiz) {
      // Horizontal: socket at row 0, talkback at row 1, label at row 2.
      // A name goes in the label drawn at an angle; the lettering fits
      // under the socket as it is, and is already on the talkback
      // button when there is one.
      gtk_grid_attach(GTK_GRID(grid), r_src->widget, pos, 0, 1, 1);

      if (r_src->talkback_widget)
        gtk_grid_attach(GTK_GRID(grid), r_src->talkback_widget, pos, 1, 1, 1);

      GtkWidget *label = get_src_label_to_show(r_src);

      if (label)
        gtk_grid_attach(GTK_GRID(grid), label, pos, 2, 1, 1);
    } else {
      // Vertical: label at col 0, socket at col 1
      gtk_grid_attach(GTK_GRID(grid), r_src->label_widget, 0, pos, 1, 1);
      gtk_grid_attach(GTK_GRID(grid), r_src->widget, 1, pos, 1, 1);
    }
    pos++;
  }

  update_src_grid_overhang(card, port_category);

  // the labels drawn at an angle are painted by the group's overlay, so
  // attaching or removing one leaves that to redraw
  GtkWidget *overlay = gtk_widget_get_parent(grid);
  if (overlay) {
    GtkWidget *painter = gtk_overlay_get_child(GTK_OVERLAY(overlay));

    if (painter)
      gtk_widget_queue_draw(painter);
  }

  if (card->routing_lines)
    gtk_widget_queue_draw(card->routing_lines);
}

// Arrange sink widgets in their grid.
void arrange_snk_grid(struct alsa_card *card, int port_category) {
  GtkWidget *grid = get_snk_grid(card, port_category);
  if (!grid)
    return;

  // Skip for fixed mixer inputs (no routing widgets)
  if (port_category == PC_MIX && card->has_fixed_mixer_inputs)
    return;

  int is_horiz = is_horiz_port_category(port_category);
  int pos = 0;

  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *r_snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );

    if (!r_snk->elem ||
        r_snk->elem->port_category != port_category ||
        !r_snk->socket_widget)
      continue;

    // Unparent all widgets
    unparent_from_grid(grid, r_snk->socket_widget);
    unparent_from_grid(grid, r_snk->label_widget);

    if (!is_snk_visible(r_snk))
      continue;

    if (is_horiz) {
      // Horizontal: label at row 0, socket at row 1 (sinks point down)
      gtk_grid_attach(GTK_GRID(grid), r_snk->label_widget, pos, 0, 1, 1);
      gtk_grid_attach(GTK_GRID(grid), r_snk->socket_widget, pos, 1, 1, 1);
    } else {
      // Vertical: socket at col 0, label at col 1
      gtk_grid_attach(GTK_GRID(grid), r_snk->socket_widget, 0, pos, 1, 1);
      gtk_grid_attach(GTK_GRID(grid), r_snk->label_widget, 1, pos, 1, 1);
    }
    pos++;
  }

  if (card->routing_lines)
    gtk_widget_queue_draw(card->routing_lines);
}

// Callback to update source socket and label when link state changes
static void src_socket_link_changed(struct alsa_elem *elem, void *private) {
  struct routing_src *src = private;

  if (!src->widget2)
    return;

  int linked = is_src_linked(src);
  GtkOrientation orient = get_socket_orientation(src->port_category);
  update_socket_widget(src->widget2, linked, orient);

  update_routing_src_label(src);

  // Rearrange grid (only after routing window is fully initialized)
  if (src->card->routing_lines)
    arrange_src_grid(src->card, src->port_category);
}

// Callback to update sink socket and label when link state changes
static void snk_socket_link_changed(struct alsa_elem *elem, void *private) {
  struct routing_snk *snk = private;

  if (!snk->socket_widget)
    return;

  int linked = is_snk_linked(snk);
  int port_category = snk->elem->port_category;
  GtkOrientation orient = get_socket_orientation(port_category);
  update_socket_widget(snk->socket_widget, linked, orient);

  update_hw_output_label(snk);

  // Rearrange grid (only after routing window is fully initialized)
  if (snk->elem->card->routing_lines)
    arrange_snk_grid(snk->elem->card, port_category);
}

// Helper to draw a rounded highlight rectangle
static void draw_highlight_rect(
  cairo_t               *cr,
  const graphene_rect_t *bounds
) {
  double x = graphene_rect_get_x(bounds);
  double y = graphene_rect_get_y(bounds);
  double w = graphene_rect_get_width(bounds);
  double h = graphene_rect_get_height(bounds);
  double radius = 4.0;

  // Add some padding
  double pad = 2.0;
  x -= pad;
  y -= pad;
  w += pad * 2;
  h += pad * 2;

  // Clamp radius to half the smallest dimension
  if (radius > w / 2) radius = w / 2;
  if (radius > h / 2) radius = h / 2;

  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - radius, y + radius, radius, -M_PI_2, 0);
  cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, M_PI_2);
  cairo_arc(cr, x + radius, y + h - radius, radius, M_PI_2, M_PI);
  cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3 * M_PI_2);
  cairo_close_path(cr);

  // #801010 colour
  cairo_set_source_rgb(cr, 0.5, 0.063, 0.063);
  cairo_fill(cr);
}

// Get combined bounds of socket + label (or socket + talkback) relative to a widget
// The label a source is showing: a mixer output swaps between the plain
// one and the one drawn at an angle, depending on whether it is named
static GtkWidget *get_src_label_in_use(struct routing_src *src) {
  if (src->rotated_label_widget &&
      gtk_widget_get_parent(src->rotated_label_widget))
    return src->rotated_label_widget;

  if (src->label_widget && gtk_widget_get_parent(src->label_widget))
    return src->label_widget;

  return NULL;
}

static int get_src_bounds_ex(
  struct routing_src *src,
  GtkWidget          *relative_to,
  graphene_rect_t    *bounds,
  int                 include_rotated
) {
  if (!src->widget2)
    return 0;

  graphene_rect_t socket_bounds;
  if (!gtk_widget_compute_bounds(src->widget2, relative_to, &socket_bounds))
    return 0;

  // For talkback sources, union socket and talkback bounds, extend to cell top
  if (src->talkback_widget) {
    graphene_rect_t talkback_bounds;
    if (!gtk_widget_compute_bounds(
          src->talkback_widget, relative_to, &talkback_bounds))
      return 0;

    graphene_rect_union(&socket_bounds, &talkback_bounds, bounds);

    // the name sits below the talkback button when there is one
    GtkWidget *label = get_src_label_in_use(src);
    graphene_rect_t label_bounds;

    if (label && (include_rotated || !is_rotated_label(label)) &&
        gtk_widget_compute_bounds(label, relative_to, &label_bounds))
      graphene_rect_union(bounds, &label_bounds, bounds);

    // Extend to cell top: compute how far socket is from grid top
    GtkWidget *grid = gtk_widget_get_parent(src->widget2);
    if (grid) {
      graphene_rect_t socket_in_grid;
      if (gtk_widget_compute_bounds(src->widget2, grid, &socket_in_grid)) {
        float extend = graphene_rect_get_y(&socket_in_grid);
        if (extend > 0) {
          graphene_rect_init(
            bounds,
            graphene_rect_get_x(bounds),
            graphene_rect_get_y(bounds) - extend,
            graphene_rect_get_width(bounds),
            graphene_rect_get_height(bounds) + extend
          );
        }
      }
    }
    return 1;
  }

  // Normal case: socket + whichever label is in use
  GtkWidget *label = get_src_label_in_use(src);

  // a rotated label is highlighted along with its text, not as the
  // narrow column it reserves
  if (label && !include_rotated && is_rotated_label(label))
    label = NULL;

  if (!label) {
    *bounds = socket_bounds;
    return 1;
  }

  graphene_rect_t label_bounds;
  if (!gtk_widget_compute_bounds(label, relative_to, &label_bounds))
    return 0;

  graphene_rect_union(&socket_bounds, &label_bounds, bounds);
  return 1;
}

static int get_src_bounds_relative(
  struct routing_src *src,
  GtkWidget          *relative_to,
  graphene_rect_t    *bounds
) {
  return get_src_bounds_ex(src, relative_to, bounds, 1);
}

static int get_snk_bounds_relative(
  struct routing_snk *snk,
  GtkWidget          *relative_to,
  graphene_rect_t    *bounds
) {
  if (!snk->socket_widget || !snk->label_widget)
    return 0;

  graphene_rect_t socket_bounds, label_bounds;
  if (!gtk_widget_compute_bounds(snk->socket_widget, relative_to, &socket_bounds))
    return 0;
  if (!gtk_widget_compute_bounds(snk->label_widget, relative_to, &label_bounds))
    return 0;

  graphene_rect_union(&socket_bounds, &label_bounds, bounds);
  return 1;
}

// Is the point on the source: on its socket, its label, or on the text
// of a label drawn at an angle, which lies outside the widget's own area
static int src_contains_point(
  struct routing_src     *src,
  GtkWidget              *relative_to,
  const graphene_point_t *point
) {
  graphene_rect_t bounds;

  if (get_src_bounds_ex(src, relative_to, &bounds, 0) &&
      graphene_rect_contains_point(&bounds, point))
    return 1;

  GtkWidget *label = get_src_label_in_use(src);

  return label && is_rotated_label(label) &&
         rotated_label_contains_point(
           label, relative_to, point->x, point->y, 2.0
         );
}

// Draw the highlight for a source, and its stereo partner, if it belongs
// to this group's overlay
// A rotated label is highlighted by its text, turned with it, rather
// than by the narrow column the widget reserves
static void draw_rotated_label_highlight(
  cairo_t            *cr,
  GtkWidget          *highlight_widget,
  struct routing_src *src
) {
  GtkWidget *label = get_src_label_in_use(src);

  if (!label || !is_rotated_label(label))
    return;

  rotated_label_append_outline(label, highlight_widget, cr, 2.0);

  // the same #801010 as the rectangular highlights
  cairo_set_source_rgb(cr, 0.5, 0.063, 0.063);
  cairo_fill(cr);
}

static void draw_src_highlight(
  cairo_t            *cr,
  GtkWidget          *highlight_widget,
  GtkWidget          *overlay,
  struct routing_src *src
) {
  GtkWidget *parent = src->widget2 ? gtk_widget_get_parent(src->widget2) : NULL;
  if (!parent || gtk_widget_get_parent(parent) != overlay)
    return;

  graphene_rect_t bounds;
  if (get_src_bounds_ex(src, highlight_widget, &bounds, 0))
    draw_highlight_rect(cr, &bounds);

  draw_rotated_label_highlight(cr, highlight_widget, src);

  if (!is_src_linked(src))
    return;

  struct routing_src *partner = get_src_partner(src);
  if (partner && get_src_bounds_ex(partner, highlight_widget, &bounds, 0))
    draw_highlight_rect(cr, &bounds);
}

// Draw the highlight for a sink, and its stereo partner, if it belongs to
// this group's overlay
static void draw_snk_highlight(
  cairo_t            *cr,
  GtkWidget          *highlight_widget,
  GtkWidget          *overlay,
  struct routing_snk *snk
) {
  GtkWidget *parent = snk->socket_widget
    ? gtk_widget_get_parent(snk->socket_widget) : NULL;
  if (!parent || gtk_widget_get_parent(parent) != overlay)
    return;

  graphene_rect_t bounds;
  if (get_snk_bounds_relative(snk, highlight_widget, &bounds))
    draw_highlight_rect(cr, &bounds);

  if (!is_snk_linked(snk))
    return;

  struct routing_snk *partner = get_snk_partner(snk);
  if (partner && get_snk_bounds_relative(partner, highlight_widget, &bounds))
    draw_highlight_rect(cr, &bounds);
}

// Collect the source ids covered by a source: its own, plus its stereo
// partner's if linked. Returns the number of ids written to ids[2].
static int get_src_ids(struct routing_src *src, int *ids) {
  int count = 0;

  ids[count++] = src->id;

  if (is_src_linked(src)) {
    struct routing_src *partner = get_src_partner(src);
    if (partner)
      ids[count++] = partner->id;
  }

  return count;
}

// Collect the source ids a sink is fed from: its own, plus its stereo
// partner's if linked. Returns the number of ids written to ids[2].
static int get_snk_src_ids(struct routing_snk *snk, int *ids) {
  int count = 0;

  ids[count++] = snk->effective_source_idx;

  if (is_snk_linked(snk)) {
    struct routing_snk *partner = get_snk_partner(snk);
    if (partner)
      ids[count++] = partner->effective_source_idx;
  }

  return count;
}

static int ids_intersect(int *a, int a_count, int *b, int b_count) {
  for (int i = 0; i < a_count; i++)
    for (int j = 0; j < b_count; j++)
      if (a[i] == b[j])
        return 1;

  return 0;
}

// Remove the "Off" source id (0) in place; returns the remaining count
static int drop_off_ids(int *ids, int count) {
  int out = 0;

  for (int i = 0; i < count; i++)
    if (ids[i])
      ids[out++] = ids[i];

  return out;
}

// A rotated label reaches beyond its own column, so it is drawn here, on
// the widget that spans the whole group, and on top of the highlights so
// that a highlight sits behind its name
static void draw_rotated_labels(
  cairo_t          *cr,
  GtkWidget        *highlight_widget,
  GtkWidget        *overlay,
  struct alsa_card *card
) {
  for (int i = 1; i < card->routing_srcs->len; i++) {
    struct routing_src *src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );

    GtkWidget *label = src->rotated_label_widget;

    if (!label || !gtk_widget_get_parent(label))
      continue;

    if (gtk_widget_get_ancestor(label, GTK_TYPE_OVERLAY) != overlay)
      continue;

    rotated_label_draw_on(label, highlight_widget, cr);
  }
}

// Draw highlights for the hovered source/sink, and everything connected to
// it: the sinks fed by a source, plus the source and all other sinks sharing
// it when a sink is hovered
static void draw_hover_highlights(
  cairo_t          *cr,
  GtkWidget        *highlight_widget,
  GtkWidget        *overlay,
  struct alsa_card *card
) {
  // The connection is identified by the source id(s) involved
  int ids[2];
  int id_count = 0;

  if (card->hovered_src) {
    draw_src_highlight(cr, highlight_widget, overlay, card->hovered_src);
    id_count = get_src_ids(card->hovered_src, ids);
  } else if (card->hovered_snk) {
    draw_snk_highlight(cr, highlight_widget, overlay, card->hovered_snk);
    id_count = get_snk_src_ids(card->hovered_snk, ids);
  } else {
    return;
  }

  // Unconnected sinks (source "Off") have nothing on the other end
  id_count = drop_off_ids(ids, id_count);

  if (!id_count)
    return;

  // Highlight the source(s) of the connection
  for (int i = 1; i < card->routing_srcs->len; i++) {
    struct routing_src *src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );

    if (!src->widget2 || src == card->hovered_src)
      continue;

    // linked pairs are drawn from the left channel
    if (is_src_linked(src) && !is_src_left_channel(src))
      continue;

    int src_ids[2];
    int src_id_count = get_src_ids(src, src_ids);

    if (ids_intersect(ids, id_count, src_ids, src_id_count))
      draw_src_highlight(cr, highlight_widget, overlay, src);
  }

  // Highlight every sink fed by that source
  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );

    if (!snk->socket_widget || snk == card->hovered_snk)
      continue;

    // linked pairs are drawn from the left channel
    if (is_snk_linked(snk) && !is_snk_left_channel(snk))
      continue;

    int snk_src_ids[2];
    int snk_src_id_count = get_snk_src_ids(snk, snk_src_ids);

    if (ids_intersect(ids, id_count, snk_src_ids, snk_src_id_count))
      draw_snk_highlight(cr, highlight_widget, overlay, snk);
  }
}

// The group's own drawing: the hover highlights, and the labels that are
// drawn on top of them
static void draw_group_highlights(
  GtkDrawingArea *drawing_area,
  cairo_t        *cr,
  int             width,
  int             height,
  void           *user_data
) {
  struct alsa_card *card = user_data;
  GtkWidget *highlight_widget = GTK_WIDGET(drawing_area);

  // Get the grid (sibling in the overlay)
  GtkWidget *overlay = gtk_widget_get_parent(highlight_widget);
  if (!overlay)
    return;

  draw_hover_highlights(cr, highlight_widget, overlay, card);
  draw_rotated_labels(cr, highlight_widget, overlay, card);
}

// Queue redraw on all group highlight areas
static void queue_redraw_group_highlights(struct alsa_card *card) {
  GtkWidget *grids[] = {
    card->routing_hw_in_grid,
    card->routing_hw_out_grid,
    card->routing_pcm_in_grid,
    card->routing_pcm_out_grid,
    card->routing_dsp_in_grid,
    card->routing_dsp_out_grid,
    card->routing_mixer_in_grid,
    card->routing_mixer_out_grid,
  };

  for (int i = 0; i < 8; i++) {
    if (!grids[i])
      continue;
    // Grid's parent is the overlay; overlay's child is the highlight area
    GtkWidget *overlay = gtk_widget_get_parent(grids[i]);
    if (overlay) {
      GtkWidget *highlight_area = gtk_overlay_get_child(GTK_OVERLAY(overlay));
      if (highlight_area)
        gtk_widget_queue_draw(highlight_area);
    }
  }
}

// Get the channel a stereo pair is displayed as: the left channel when
// linked, the port itself otherwise
static struct routing_src *src_display_rep(struct routing_src *src) {
  if (src && is_src_linked(src) && !is_src_left_channel(src)) {
    struct routing_src *partner = get_src_partner(src);
    if (partner)
      return partner;
  }

  return src;
}

static struct routing_snk *snk_display_rep(struct routing_snk *snk) {
  if (snk && is_snk_linked(snk) && !is_snk_left_channel(snk)) {
    struct routing_snk *partner = get_snk_partner(snk);
    if (partner)
      return partner;
  }

  return snk;
}

// The hidden ports at the far end of a port's arrows: the sinks fed by a
// source, or the source feeding a sink. Listed once per stereo pair, as
// the channel the pair is displayed as.
// Returns an array of struct routing_snk *, empty if there are none.
static GPtrArray *get_hidden_snk_peers(
  struct alsa_card   *card,
  struct routing_src *src
) {
  GPtrArray *peers = g_ptr_array_new();

  int ids[2];
  int id_count = drop_off_ids(ids, get_src_ids(src, ids));

  if (!id_count)
    return peers;

  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );

    // read-only mixer sinks have no lines and no arrows
    if (snk->elem->port_category == PC_MIX && card->has_fixed_mixer_inputs)
      continue;

    if (is_routing_snk_enabled(snk))
      continue;

    if (snk_display_rep(snk) != snk)
      continue;

    int snk_ids[2];
    int snk_id_count = get_snk_src_ids(snk, snk_ids);

    if (ids_intersect(ids, id_count, snk_ids, snk_id_count))
      g_ptr_array_add(peers, snk);
  }

  return peers;
}

// Returns an array of struct routing_src *, empty if the sink's source is
// not hidden
static GPtrArray *get_hidden_src_peers(
  struct alsa_card   *card,
  struct routing_snk *snk
) {
  GPtrArray *peers = g_ptr_array_new();

  int ids[2];
  int id_count = drop_off_ids(ids, get_snk_src_ids(snk, ids));

  for (int i = 0; i < id_count; i++) {
    struct routing_src *src = &g_array_index(
      card->routing_srcs, struct routing_src, ids[i]
    );

    if (is_routing_src_enabled(src))
      continue;

    src = src_display_rep(src);

    if (peers->len && g_ptr_array_index(peers, peers->len - 1) == src)
      continue;

    g_ptr_array_add(peers, src);
  }

  return peers;
}

// One name per line, or NULL if there are no peers
static char *get_peer_names(GPtrArray *peers, int are_srcs) {
  if (!peers->len)
    return NULL;

  GString *text = g_string_new(NULL);

  for (int i = 0; i < peers->len; i++) {
    char *name = are_srcs
      ? get_src_qualified_name(g_ptr_array_index(peers, i))
      : get_snk_qualified_name(g_ptr_array_index(peers, i));

    if (text->len)
      g_string_append_c(text, '\n');

    g_string_append(text, name);
    g_free(name);
  }

  return g_string_free(text, FALSE);
}

// Place the popover on the side the arrows point towards, so that it
// stands where the hidden port would have been
static GtkPositionType arrow_dir_position(int direction) {
  switch (direction) {
    case ARROW_DIR_RIGHT: return GTK_POS_RIGHT;
    case ARROW_DIR_LEFT:  return GTK_POS_LEFT;
    case ARROW_DIR_UP:    return GTK_POS_TOP;
    default:              return GTK_POS_BOTTOM;
  }
}

static void hide_hidden_peer_popover(struct alsa_card *card) {
  if (!card->hidden_peer_popover)
    return;

  gtk_widget_unparent(card->hidden_peer_popover);
  card->hidden_peer_popover = NULL;
}

// Name the ports on the far end of the hovered port's hidden connections
static void show_hidden_peer_popover(struct alsa_card *card) {
  GtkWidget *overlay = card->routing_overlay;

  if (!overlay)
    return;

  GPtrArray *peers;
  char *names;
  const char *heading;
  int direction;
  graphene_rect_t bounds;

  if (card->hovered_src) {
    if (!get_src_bounds_relative(card->hovered_src, overlay, &bounds))
      return;

    peers = get_hidden_snk_peers(card, card->hovered_src);
    names = get_peer_names(peers, 0);
    heading = "Connected to:";
    direction = routing_src_arrow_dir(card->hovered_src);

  } else if (card->hovered_snk) {
    if (!get_snk_bounds_relative(card->hovered_snk, overlay, &bounds))
      return;

    peers = get_hidden_src_peers(card, card->hovered_snk);
    names = get_peer_names(peers, 1);
    heading = "Connected from:";
    direction = routing_snk_arrow_dir(card->hovered_snk);

  } else {
    return;
  }

  g_ptr_array_free(peers, TRUE);

  if (!names)
    return;

  GtkWidget *popover = gtk_popover_new();

  // the popover only reports; it must not take the pointer away from the
  // hover and drag handling on the overlay below it
  gtk_popover_set_autohide(GTK_POPOVER(popover), FALSE);
  gtk_widget_set_can_target(popover, FALSE);
  gtk_widget_set_can_focus(popover, FALSE);

  gtk_widget_set_parent(popover, overlay);
  gtk_popover_set_position(
    GTK_POPOVER(popover), arrow_dir_position(direction)
  );

  GdkRectangle rect = {
    .x      = bounds.origin.x,
    .y      = bounds.origin.y,
    .width  = bounds.size.width,
    .height = bounds.size.height
  };
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

  GtkWidget *heading_label = gtk_label_new(heading);
  gtk_widget_add_css_class(heading_label, "dim-label");
  gtk_widget_set_halign(heading_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), heading_label);

  GtkWidget *names_label = gtk_label_new(names);
  g_free(names);
  gtk_label_set_justify(GTK_LABEL(names_label), GTK_JUSTIFY_LEFT);
  gtk_widget_set_halign(names_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), names_label);

  gtk_popover_set_child(GTK_POPOVER(popover), box);

  card->hidden_peer_popover = popover;
  gtk_popover_popup(GTK_POPOVER(popover));
}

// Redraw the highlights and lines for a changed hover, and show or hide
// the popover naming the hidden ports the arrows point to
static void update_routing_hover_ui(struct alsa_card *card) {
  hide_hidden_peer_popover(card);

  // during a drag the connections are still moving
  if (card->drag_type == DRAG_TYPE_NONE)
    show_hidden_peer_popover(card);

  queue_redraw_group_highlights(card);
  gtk_widget_queue_draw(card->routing_lines);
}

// Motion handler for overlay-based hover detection
static void routing_overlay_motion(
  GtkEventControllerMotion *controller,
  double                    x,
  double                    y,
  gpointer                  user_data
) {
  struct alsa_card *card = user_data;
  GtkWidget *overlay = gtk_event_controller_get_widget(
    GTK_EVENT_CONTROLLER(controller)
  );

  struct routing_src *new_hovered_src = NULL;
  struct routing_snk *new_hovered_snk = NULL;
  graphene_point_t point = GRAPHENE_POINT_INIT(x, y);

  // Check sources (skip "Off" at index 0)
  for (int i = 1; i < card->routing_srcs->len; i++) {
    struct routing_src *src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );

    if (!is_routing_src_enabled(src) || !src->widget2)
      continue;

    // For linked pairs, only hit-test the left channel
    if (is_src_linked(src) && !is_src_left_channel(src))
      continue;

    if (src_contains_point(src, overlay, &point)) {
      new_hovered_src = src;
      break;
    }
  }

  // Check sinks if no source was hit
  if (!new_hovered_src) {
    for (int i = 0; i < card->routing_snks->len; i++) {
      struct routing_snk *snk = &g_array_index(
        card->routing_snks, struct routing_snk, i
      );

      if (!is_routing_snk_enabled(snk))
        continue;

      // For linked pairs, only hit-test the left channel
      if (is_snk_linked(snk) && !is_snk_left_channel(snk))
        continue;

      graphene_rect_t bounds;
      if (!get_snk_bounds_relative(snk, overlay, &bounds))
        continue;

      if (graphene_rect_contains_point(&bounds, &point)) {
        new_hovered_snk = snk;
        break;
      }
    }
  }

  // Update hover state if changed
  if (new_hovered_src != card->hovered_src ||
      new_hovered_snk != card->hovered_snk) {
    card->hovered_src = new_hovered_src;
    card->hovered_snk = new_hovered_snk;
    update_routing_hover_ui(card);
  }
}

// Leave handler for overlay-based hover detection
static void routing_overlay_leave(
  GtkEventControllerMotion *controller,
  gpointer                  user_data
) {
  struct alsa_card *card = user_data;

  if (card->hovered_src || card->hovered_snk) {
    card->hovered_src = NULL;
    card->hovered_snk = NULL;
    update_routing_hover_ui(card);
  }
}

// Forward declarations for functions used by unified handlers
static struct alsa_elem *get_snk_routing_elem(struct routing_snk *r_snk);
static void route_src_to_snk(
  struct alsa_card   *card,
  struct routing_snk *r_snk,
  int                 src_id
);

// Find routing source at point (returns NULL if none)
static struct routing_src *find_src_at_point(
  struct alsa_card *card,
  GtkWidget        *relative_to,
  double            x,
  double            y
) {
  graphene_point_t point = GRAPHENE_POINT_INIT(x, y);

  for (int i = 1; i < card->routing_srcs->len; i++) {
    struct routing_src *src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );

    if (!is_routing_src_enabled(src) || !src->widget2)
      continue;

    if (is_src_linked(src) && !is_src_left_channel(src))
      continue;

    if (src_contains_point(src, relative_to, &point))
      return src;
  }

  return NULL;
}

// Find routing sink at point (returns NULL if none)
static struct routing_snk *find_snk_at_point(
  struct alsa_card *card,
  GtkWidget        *relative_to,
  double            x,
  double            y
) {
  graphene_point_t point = GRAPHENE_POINT_INIT(x, y);

  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );

    if (!is_routing_snk_enabled(snk))
      continue;

    if (is_snk_linked(snk) && !is_snk_left_channel(snk))
      continue;

    graphene_rect_t bounds;
    if (!get_snk_bounds_relative(snk, relative_to, &bounds))
      continue;

    if (graphene_rect_contains_point(&bounds, &point))
      return snk;
  }

  return NULL;
}

// Close the context menu popover a button belongs to
static void popdown_popover(GtkButton *button) {
  GtkWidget *popover = gtk_widget_get_ancestor(
    GTK_WIDGET(button), GTK_TYPE_POPOVER
  );

  if (popover)
    gtk_popover_popdown(GTK_POPOVER(popover));
}

// Callback for stereo link toggle button inside context menu popover
static void link_popover_clicked(GtkButton *button, gpointer user_data) {
  struct alsa_elem *link_elem = user_data;
  int current = alsa_get_elem_value(link_elem);
  alsa_set_elem_value(link_elem, !current);

  popdown_popover(button);
}

// Callbacks for the hide button inside the port context menu popover
static void hide_src_clicked(GtkButton *button, gpointer user_data) {
  set_src_port_enabled(user_data, 0);
  popdown_popover(button);
}

static void hide_snk_clicked(GtkButton *button, gpointer user_data) {
  set_snk_port_enabled(user_data, 0);
  popdown_popover(button);
}

// The name element the routing window's label for a port comes from: the
// pair's when it is linked, the channel's own otherwise.
static struct alsa_elem *get_src_name_elem(struct routing_src *src) {
  // DSP ports are labelled by number here, so a name would not show
  if (src->port_category == PC_DSP)
    return NULL;

  if (is_src_linked(src)) {
    struct alsa_elem *pair_elem = get_src_pair_name_elem(src_display_rep(src));

    if (pair_elem)
      return pair_elem;
  }

  return src->custom_name_elem;
}

static struct alsa_elem *get_snk_name_elem(struct routing_snk *snk) {
  int port_category = snk->elem->port_category;

  if (port_category == PC_MIX || port_category == PC_DSP)
    return NULL;

  if (is_snk_linked(snk)) {
    struct alsa_elem *pair_elem = get_snk_pair_name_elem(snk_display_rep(snk));

    if (pair_elem)
      return pair_elem;
  }

  return snk->custom_name_elem;
}

// The rename entry replaces the buttons in the same popover
struct rename_data {
  GtkWidget *stack;
  GtkWidget *page;
  GtkWidget *entry;
};

// Enter finishes the rename, as Escape does; the name is saved as it is
// typed, so there is nothing to commit
static void rename_entry_activate(GtkEntry *entry, gpointer user_data) {
  GtkWidget *popover = gtk_widget_get_ancestor(
    GTK_WIDGET(entry), GTK_TYPE_POPOVER
  );

  if (popover)
    gtk_popover_popdown(GTK_POPOVER(popover));
}

static void rename_clicked(GtkButton *button, gpointer user_data) {
  struct rename_data *data = user_data;

  gtk_stack_set_visible_child(GTK_STACK(data->stack), data->page);
  gtk_widget_grab_focus(data->entry);
}

// Build the rename page: the name as it is displayed now, and an entry
// bound to the element it comes from
static GtkWidget *make_rename_page(
  struct alsa_elem *name_elem,
  char             *title
) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

  GtkWidget *label = gtk_label_new(title);
  gtk_widget_add_css_class(label, "heading");
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), label);

  GtkWidget *entry = make_text_entry_alsa_elem(name_elem);
  g_signal_connect(entry, "activate", G_CALLBACK(rename_entry_activate), NULL);
  gtk_widget_set_tooltip_text(
    entry, "Clear the name to go back to the default"
  );
  gtk_box_append(GTK_BOX(box), entry);

  g_object_set_data(G_OBJECT(box), "entry", entry);

  return box;
}

// Callback for the show button inside the port context menu popover:
// unhides the ports at the far end of this port's hidden connections
struct show_peers_data {
  GPtrArray *peers;
  int        are_srcs;
};

static void show_peers_clicked(GtkButton *button, gpointer user_data) {
  struct show_peers_data *data = user_data;

  for (int i = 0; i < data->peers->len; i++) {
    void *peer = g_ptr_array_index(data->peers, i);

    if (data->are_srcs)
      set_src_port_enabled(peer, 1);
    else
      set_snk_port_enabled(peer, 1);
  }

  popdown_popover(button);
}

static void show_peers_data_free(void *user_data) {
  struct show_peers_data *data = user_data;

  g_ptr_array_free(data->peers, TRUE);
  g_free(data);
}

static GtkWidget *make_popover_button(
  const char    *label,
  const char    *tooltip,
  GCallback      callback,
  void          *user_data,
  GClosureNotify destroy
) {
  GtkWidget *button = gtk_button_new_with_label(label);

  gtk_widget_add_css_class(button, "flat");
  if (tooltip)
    gtk_widget_set_tooltip_text(button, tooltip);
  g_signal_connect_data(button, "clicked", callback, user_data, destroy, 0);

  return button;
}

// Right-click handler for the port context menu
static void routing_overlay_right_click(
  GtkGestureClick *gesture,
  int              n_press,
  double           x,
  double           y,
  gpointer         user_data
) {
  struct alsa_card *card = user_data;
  GtkWidget *widget = gtk_event_controller_get_widget(
    GTK_EVENT_CONTROLLER(gesture)
  );

  struct routing_src *r_src = find_src_at_point(card, widget, x, y);
  struct routing_snk *r_snk = find_snk_at_point(card, widget, x, y);

  if (!r_src && !r_snk)
    return;

  // Get the link, name, and enable elements for the clicked source or
  // sink. Ports at the end of the numbering have no channel to pair with.
  struct alsa_elem *link_elem = NULL;
  struct alsa_elem *name_elem = NULL;
  struct alsa_elem *enable_elem = NULL;
  int is_linked = 0;

  if (r_src) {
    if (src_has_valid_partner(r_src)) {
      link_elem = get_src_link_elem(r_src);
      is_linked = is_src_linked(r_src);
    }
    name_elem = get_src_name_elem(r_src);
    enable_elem = get_src_enable_elem(r_src);
  } else {
    if (snk_has_valid_partner(r_snk)) {
      link_elem = get_snk_link_elem(r_snk);
      is_linked = is_snk_linked(r_snk);
    }
    name_elem = get_snk_name_elem(r_snk);
    enable_elem = get_snk_enable_elem(r_snk);
  }

  if (!link_elem && !name_elem && !enable_elem)
    return;

  // the hidden ports this one is connected to, named by the popover shown
  // while hovering it
  GPtrArray *peers = r_src
    ? get_hidden_snk_peers(card, r_src)
    : get_hidden_src_peers(card, r_snk);

  // the rename entry takes the place of the buttons in the same popover,
  // which shrinks to fit it
  GtkWidget *stack = gtk_stack_new();
  gtk_stack_set_hhomogeneous(GTK_STACK(stack), FALSE);
  gtk_stack_set_vhomogeneous(GTK_STACK(stack), FALSE);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  if (link_elem)
    gtk_box_append(
      GTK_BOX(box),
      make_popover_button(
        is_linked ? "Unlink Stereo Pair" : "Link as Stereo Pair",
        NULL, G_CALLBACK(link_popover_clicked), link_elem, NULL
      )
    );

  GtkWidget *rename_page = NULL;

  if (name_elem) {
    char *title = r_src
      ? get_src_stereo_aware_name(r_src)
      : get_snk_stereo_aware_name(r_snk);

    rename_page = make_rename_page(name_elem, title);
    g_free(title);

    struct rename_data *data = g_new0(struct rename_data, 1);
    data->stack = stack;
    data->page = rename_page;
    data->entry = g_object_get_data(G_OBJECT(rename_page), "entry");

    gtk_box_append(
      GTK_BOX(box),
      make_popover_button(
        "Rename\xe2\x80\xa6",
        "Give this port a name of your own",
        G_CALLBACK(rename_clicked), data, (GClosureNotify)g_free
      )
    );
  }

  if (peers->len) {
    char *label;
    const char *tooltip;

    if (peers->len == 1) {
      char *name = r_src
        ? get_snk_qualified_name(g_ptr_array_index(peers, 0))
        : get_src_qualified_name(g_ptr_array_index(peers, 0));

      label = g_strdup_printf("Show %s", name);
      tooltip = "Show the hidden port at the other end of this connection";
      g_free(name);
    } else {
      label = g_strdup("Show Connected Ports");
      tooltip = "Show the hidden ports at the other end of the connections "
                "to this port";
    }

    struct show_peers_data *data = g_new0(struct show_peers_data, 1);
    data->peers = peers;
    data->are_srcs = !!r_snk;

    gtk_box_append(
      GTK_BOX(box),
      make_popover_button(
        label, tooltip,
        G_CALLBACK(show_peers_clicked), data,
        (GClosureNotify)show_peers_data_free
      )
    );
    g_free(label);
  } else {
    g_ptr_array_free(peers, TRUE);
  }

  if (enable_elem)
    gtk_box_append(
      GTK_BOX(box),
      make_popover_button(
        "Hide",
        "Hide this port. Any connection to it is kept, and shown by an "
        "arrow on the port at the other end.",
        r_src ? G_CALLBACK(hide_src_clicked) : G_CALLBACK(hide_snk_clicked),
        r_src ? (void *)r_src : (void *)r_snk,
        NULL
      )
    );

  gtk_stack_add_named(GTK_STACK(stack), box, "menu");
  if (rename_page)
    gtk_stack_add_named(GTK_STACK(stack), rename_page, "rename");

  GtkWidget *popover = routing_menu_popover_new(widget, x, y);
  gtk_popover_set_child(GTK_POPOVER(popover), stack);
  gtk_popover_popup(GTK_POPOVER(popover));
}

// The text of a label drawn at an angle is outside the widget that
// carries the tooltip, so nothing would show it without this
static gboolean routing_overlay_query_tooltip(
  GtkWidget  *widget,
  int         x,
  int         y,
  gboolean    keyboard,
  GtkTooltip *tooltip,
  gpointer    user_data
) {
  struct alsa_card *card = user_data;

  if (keyboard)
    return FALSE;

  struct routing_src *r_src = find_src_at_point(card, widget, x, y);
  if (!r_src)
    return FALSE;

  GtkWidget *label = get_src_label_in_use(r_src);
  if (!label || !is_rotated_label(label))
    return FALSE;

  const char *text = gtk_widget_get_tooltip_text(label);
  if (!text)
    return FALSE;

  gtk_tooltip_set_text(tooltip, text);

  return TRUE;
}

// Unified click handler using hit-testing
static void routing_overlay_click(
  GtkGestureClick *gesture,
  int              n_press,
  double           x,
  double           y,
  gpointer         user_data
) {
  struct alsa_card *card = user_data;
  GtkWidget *widget = gtk_event_controller_get_widget(
    GTK_EVENT_CONTROLLER(gesture)
  );

  struct routing_src *r_src = find_src_at_point(card, widget, x, y);
  struct routing_snk *r_snk = find_snk_at_point(card, widget, x, y);

  if (r_src) {
    // Clear all sinks connected to this source
    for (int i = 0; i < card->routing_snks->len; i++) {
      struct routing_snk *snk = &g_array_index(
        card->routing_snks, struct routing_snk, i
      );

      int connected = snk->effective_source_idx == r_src->id;
      if (!connected && is_src_linked(r_src)) {
        struct routing_src *partner = get_src_partner(r_src);
        if (partner)
          connected = snk->effective_source_idx == partner->id;
      }

      if (!connected)
        continue;

      struct alsa_elem *target_elem = get_snk_routing_elem(snk);
      if (target_elem && target_elem == snk->elem)
        alsa_set_elem_value(target_elem, 0);
    }
  } else if (r_snk) {
    struct alsa_elem *target_elem = get_snk_routing_elem(r_snk);

    if (!target_elem || target_elem != r_snk->elem)
      return;

    if (is_snk_linked(r_snk)) {
      struct routing_snk *partner = get_snk_partner(r_snk);
      if (partner) {
        route_src_to_snk(card, r_snk, 0);
        route_src_to_snk(card, partner, 0);
        return;
      }
    }

    alsa_set_elem_value(target_elem, 0);
  }
}

// Drag source prepare handler - hit-test to find drag origin
static GdkContentProvider *routing_drag_prepare(
  GtkDragSource *source,
  double         x,
  double         y,
  gpointer       user_data
) {
  struct alsa_card *card = user_data;
  GtkWidget *widget = gtk_event_controller_get_widget(
    GTK_EVENT_CONTROLLER(source)
  );

  struct routing_src *r_src = find_src_at_point(card, widget, x, y);
  struct routing_snk *r_snk = find_snk_at_point(card, widget, x, y);

  // the connections are about to move, so stop naming their far ends
  if (r_src || r_snk)
    hide_hidden_peer_popover(card);

  if (r_src) {
    card->drag_type = DRAG_TYPE_SRC;
    card->src_drag = r_src;
    return gdk_content_provider_new_typed(G_TYPE_INT, r_src->id);
  } else if (r_snk) {
    card->drag_type = DRAG_TYPE_SNK;
    card->snk_drag = r_snk;
    return gdk_content_provider_new_typed(G_TYPE_INT, 0x8000 | r_snk->idx);
  }

  return NULL;
}

// Check if a routing operation from source to sink is valid
static int is_routing_valid(
  struct alsa_card   *card,
  struct routing_src *r_src,
  struct routing_snk *r_snk
) {
  // Reject drops on muted sinks
  if (is_snk_monitor_muted(r_snk))
    return 0;

  // Reject stereo source → mono sink
  if (r_src && is_src_linked(r_src) && !is_snk_linked(r_snk))
    return 0;

  // Reject mixer → mixer routing if device doesn't support it
  if (!card->mixer_has_mix_srcs &&
      r_src && r_src->port_category == PC_MIX &&
      r_snk->elem->port_category == PC_MIX)
    return 0;

  return 1;
}

// Drag source end handler
static void routing_drag_end(
  GtkDragSource *source,
  GdkDrag       *drag,
  gboolean       delete_data,
  gpointer       user_data
) {
  struct alsa_card *card = user_data;

  card->drag_type = DRAG_TYPE_NONE;
  card->src_drag = NULL;
  card->snk_drag = NULL;

  // Recompute hover from last known position (coords are relative to routing_grid)
  if (card->drag_x >= 0 && card->drag_y >= 0) {
    card->hovered_src = find_src_at_point(
      card, card->routing_grid, card->drag_x, card->drag_y
    );
    card->hovered_snk = find_snk_at_point(
      card, card->routing_grid, card->drag_x, card->drag_y
    );
  }

  // Clear drag coords now that we're done with them
  card->drag_x = -1;
  card->drag_y = -1;

  update_routing_hover_ui(card);
  gtk_widget_queue_draw(card->drag_line);
}

// Drop target motion handler - hit-test for drop feedback
static GdkDragAction routing_drop_motion(
  GtkDropTarget *target,
  double         x,
  double         y,
  gpointer       user_data
) {
  struct alsa_card *card = user_data;
  GtkWidget *widget = gtk_event_controller_get_widget(
    GTK_EVENT_CONTROLLER(target)
  );

  struct routing_src *r_src = find_src_at_point(card, widget, x, y);
  struct routing_snk *r_snk = find_snk_at_point(card, widget, x, y);

  // Update drag target and hover state for visual feedback
  if (card->drag_type == DRAG_TYPE_SRC && r_snk &&
      is_routing_valid(card, card->src_drag, r_snk)) {
    card->snk_drag = r_snk;
    card->hovered_snk = r_snk;
    card->hovered_src = NULL;
    update_routing_hover_ui(card);
    gtk_widget_queue_draw(card->drag_line);
    return GDK_ACTION_COPY;
  } else if (card->drag_type == DRAG_TYPE_SNK && r_src &&
             is_routing_valid(card, r_src, card->snk_drag)) {
    card->src_drag = r_src;
    card->hovered_src = r_src;
    card->hovered_snk = NULL;
    update_routing_hover_ui(card);
    gtk_widget_queue_draw(card->drag_line);
    return GDK_ACTION_COPY;
  }

  // Clear any previous drop target and hover state
  if (card->drag_type == DRAG_TYPE_SRC) {
    card->snk_drag = NULL;
    card->hovered_snk = NULL;
  } else if (card->drag_type == DRAG_TYPE_SNK) {
    card->src_drag = NULL;
    card->hovered_src = NULL;
  }

  update_routing_hover_ui(card);
  gtk_widget_queue_draw(card->drag_line);
  return 0;
}

// Drop target leave handler
static void routing_drop_leave(
  GtkDropTarget *target,
  gpointer       user_data
) {
  struct alsa_card *card = user_data;

  if (card->drag_type == DRAG_TYPE_SRC) {
    card->snk_drag = NULL;
    card->hovered_snk = NULL;
  } else if (card->drag_type == DRAG_TYPE_SNK) {
    card->src_drag = NULL;
    card->hovered_src = NULL;
  }

  update_routing_hover_ui(card);
  gtk_widget_queue_draw(card->drag_line);
}

// Drop target drop handler - perform the routing connection
static gboolean routing_drop(
  GtkDropTarget *target,
  const GValue  *value,
  double         x,
  double         y,
  gpointer       user_data
) {
  struct alsa_card *card = user_data;
  GtkWidget *widget = gtk_event_controller_get_widget(
    GTK_EVENT_CONTROLLER(target)
  );

  struct routing_src *r_src = find_src_at_point(card, widget, x, y);
  struct routing_snk *r_snk = find_snk_at_point(card, widget, x, y);

  // Dropped on sink while dragging from source
  if (card->drag_type == DRAG_TYPE_SRC && r_snk && card->src_drag) {
    struct routing_src *drag_src = card->src_drag;

    if (!is_routing_valid(card, drag_src, r_snk))
      return FALSE;

    if (is_snk_linked(r_snk)) {
      struct routing_snk *snk_partner = get_snk_partner(r_snk);
      if (snk_partner) {
        if (is_src_linked(drag_src)) {
          // Stereo → Stereo: route L→L, R→R
          struct routing_src *src_partner = get_src_partner(drag_src);
          if (src_partner) {
            route_src_to_snk(card, r_snk, drag_src->id);
            route_src_to_snk(card, snk_partner, src_partner->id);
            return TRUE;
          }
        } else {
          // Mono → Stereo: route same source to both
          route_src_to_snk(card, r_snk, drag_src->id);
          route_src_to_snk(card, snk_partner, drag_src->id);
          return TRUE;
        }
      }
    }

    route_src_to_snk(card, r_snk, drag_src->id);
    return TRUE;
  }

  // Dropped on source while dragging from sink
  if (card->drag_type == DRAG_TYPE_SNK && r_src && card->snk_drag) {
    struct routing_snk *drag_snk = card->snk_drag;

    if (!is_routing_valid(card, r_src, drag_snk))
      return FALSE;

    if (is_snk_linked(drag_snk)) {
      struct routing_snk *snk_partner = get_snk_partner(drag_snk);
      if (snk_partner) {
        if (is_src_linked(r_src)) {
          // Stereo → Stereo: route L→L, R→R
          struct routing_src *src_partner = get_src_partner(r_src);
          if (src_partner) {
            route_src_to_snk(card, drag_snk, r_src->id);
            route_src_to_snk(card, snk_partner, src_partner->id);
            return TRUE;
          }
        } else {
          // Mono → Stereo: route same source to both
          route_src_to_snk(card, drag_snk, r_src->id);
          route_src_to_snk(card, snk_partner, r_src->id);
          return TRUE;
        }
      }
    }

    route_src_to_snk(card, drag_snk, r_src->id);
    return TRUE;
  }

  return FALSE;
}

// Check if a sink is currently muted (in inactive monitor group)
int is_snk_monitor_muted(struct routing_snk *r_snk) {
  struct alsa_elem *elem = r_snk->elem;

  // Only HW analogue outputs can be muted by monitor group switching
  if (elem->port_category != PC_HW || elem->hw_type != HW_TYPE_ANALOGUE)
    return 0;

  // If not in any group, not muted
  if (!snk_in_any_monitor_group(r_snk))
    return 0;

  int active = get_active_monitor_group(elem->card);
  if (active < 0)
    return 0;

  return !snk_in_monitor_group(r_snk, active);
}

// Get the element to modify for routing changes to a sink.
// Returns the group source elem if sink is in active monitor group,
// otherwise returns the routing elem. Returns NULL if sink is muted.
static struct alsa_elem *get_snk_routing_elem(struct routing_snk *r_snk) {
  struct alsa_elem *elem = r_snk->elem;

  // Only HW analogue outputs can be affected by monitor groups
  if (elem->port_category != PC_HW || elem->hw_type != HW_TYPE_ANALOGUE)
    return elem;

  // If not in any group, use normal routing
  if (!snk_in_any_monitor_group(r_snk))
    return elem;

  int active = get_active_monitor_group(elem->card);
  if (active < 0)
    return elem;

  if (snk_in_monitor_group(r_snk, active))
    return r_snk->monitor_groups[active].source;

  // In a group, but not the active one - muted
  return NULL;
}

// Convert routing source ID to monitor group source ID.
// Monitor group source enums have different indices, so we need to search.
static int routing_src_to_vg_src(struct alsa_card *card, int routing_src_id) {
  // Search the map for the routing source ID
  for (int i = 0; i < card->monitor_group_src_map_count; i++) {
    if (card->monitor_group_src_map[i] == routing_src_id)
      return i;
  }
  // Not found - return 0 (Off)
  return 0;
}

// Route a source to a sink, handling VG source conversion
static void route_src_to_snk(
  struct alsa_card   *card,
  struct routing_snk *snk,
  int                 src_id
) {
  struct alsa_elem *target = get_snk_routing_elem(snk);
  if (!target)
    return;

  if (target != snk->elem)
    src_id = routing_src_to_vg_src(card, src_id);

  alsa_set_elem_value(target, src_id);
}

// Set up a source socket widget (stereo handling only - click/drag at overlay level)
static void setup_src_socket(struct routing_src *r_src) {
  GtkWidget *socket = make_socket_widget();
  r_src->widget = g_object_ref(socket);
  r_src->widget2 = socket;

  g_object_set_data(G_OBJECT(socket), "routing_src", r_src);
  gtk_widget_add_css_class(socket, "route-label");

  widget_doc_register_named(socket, r_src->name);

  // Stereo link handling
  struct alsa_elem *link_elem = get_src_link_elem(r_src);
  if (link_elem) {
    alsa_elem_add_callback(link_elem, src_socket_link_changed, r_src, NULL);
    if (is_src_linked(r_src)) {
      GtkOrientation orient = get_socket_orientation(r_src->port_category);
      update_socket_widget(socket, 1, orient);
    }
  }

  // Pair name change handling
  struct alsa_elem *pair_name_elem = get_src_pair_name_elem(r_src);
  if (pair_name_elem)
    alsa_elem_add_callback(pair_name_elem, src_socket_link_changed, r_src, NULL);
}

// Create and configure a routing label widget
// Returns a ref'd widget; caller should store the pointer
static GtkWidget *make_routing_label(
  const char *name,
  const char *data_key,
  void       *data_value,
  int         port_category,
  int         is_linked,
  float       vert_xalign
) {
  GtkWidget *label = gtk_label_new(name);
  g_object_ref(label);

  gtk_widget_add_css_class(label, "route-label");
  g_object_set_data(G_OBJECT(label), data_key, data_value);

  if (is_horiz_port_category(port_category)) {
    int width = is_linked ? LABEL_WIDTH_STEREO : LABEL_WIDTH_MONO;
    gtk_label_set_width_chars(GTK_LABEL(label), width);
  } else {
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_FILL);
    gtk_label_set_xalign(GTK_LABEL(label), vert_xalign);
  }

  return label;
}

// Set up a source label widget. A mixer output gets a second one drawn
// at an angle, for when its name is too long to sit under its socket.
static void setup_src_label(struct routing_src *r_src, const char *name) {
  r_src->label_widget = make_routing_label(
    name, "routing_src", r_src,
    r_src->port_category, is_src_linked(r_src), 1.0
  );

  if (r_src->port_category == PC_MIX) {
    r_src->rotated_label_widget = g_object_ref(rotated_label_new());
    gtk_widget_add_css_class(r_src->rotated_label_widget, "route-label");
    rotated_label_set_markup(r_src->rotated_label_widget, name);
  }

  set_port_menu_tooltip(r_src->label_widget, r_src->widget2);

  // same name as the socket, so the documented region covers both
  widget_doc_register_named(r_src->label_widget, r_src->name);
}

// Set up a sink socket widget (stereo handling only - click/drag at overlay level)
static void setup_snk_socket(struct routing_snk *r_snk) {
  GtkWidget *socket = make_socket_widget();
  r_snk->socket_widget = g_object_ref(socket);

  g_object_set_data(G_OBJECT(socket), "routing_snk", r_snk);
  gtk_widget_add_css_class(socket, "route-label");

  widget_doc_register(socket, r_snk->elem);

  // Stereo link handling
  struct alsa_elem *link_elem = get_snk_link_elem(r_snk);
  if (link_elem) {
    alsa_elem_add_callback(link_elem, snk_socket_link_changed, r_snk, NULL);
    if (is_snk_linked(r_snk)) {
      GtkOrientation orient = get_socket_orientation(r_snk->elem->port_category);
      update_socket_widget(socket, 1, orient);
    }
  }

  // Pair name change handling
  struct alsa_elem *pair_name_elem = get_snk_pair_name_elem(r_snk);
  if (pair_name_elem)
    alsa_elem_add_callback(pair_name_elem, snk_socket_link_changed, r_snk, NULL);
}

// Set up a sink label widget
static void setup_snk_label(struct routing_snk *r_snk, const char *name) {
  r_snk->label_widget = make_routing_label(
    name, "routing_snk", r_snk,
    r_snk->elem->port_category, is_snk_linked(r_snk), 0.0
  );

  set_port_menu_tooltip(r_snk->label_widget, r_snk->socket_widget);

  // same element as the socket, so the documented region covers both
  widget_doc_register(r_snk->label_widget, r_snk->elem);
}

static GtkWidget *make_talkback_mix_widget(
  struct alsa_card   *card,
  struct routing_src *r_src
) {
  // Use lr_num to construct the element name (lr_num 1='A', 2='B', etc.)
  char talkback_elem_name[80];
  snprintf(
    talkback_elem_name, 80,
    "Talkback Mix %c Playback Switch",
    'A' + r_src->lr_num - 1
  );
  struct alsa_elem *talkback_elem =
    get_elem_by_name(card->elems, talkback_elem_name);
  if (!talkback_elem)
    return NULL;

  // Store the element in the routing source for sync callbacks
  r_src->talkback_elem = talkback_elem;

  // Register talkback sync callback for stereo linking
  stereo_link_register_talkback_callback(r_src);

  // The name goes on the label below; the button stays lettered so that
  // it keeps the width of its socket
  char *display_name = get_src_letter_name(r_src);
  GtkWidget *button = make_boolean_alsa_elem(
    talkback_elem, display_name, display_name
  );
  g_free(display_name);

  gtk_widget_set_tooltip_text(
    button,
    "Talkback: with this enabled, the level of the talkback mic in this "
    "mix is raised while the Talkback control is on."
  );

  /* The button takes the label's place under the socket and carries the
   * mix's name, so it is part of the mix's region as well as being the
   * talkback control in its own right.
   */
  widget_doc_register_named(button, r_src->name);

  return button;
}

// Helper to set label styling for availability
static void set_availability_label(
  GtkWidget  *label,
  const char *base_name,
  int         available,
  const char *tooltip
) {
  // a rotated label takes markup rather than text, and has no separate
  // "use markup" state to reset
  if (is_rotated_label(label)) {
    char *escaped = g_markup_escape_text(base_name, -1);
    char *markup = available
      ? g_strdup(escaped)
      : g_strdup_printf("<span color=\"#808080\"><s>%s</s></span>", escaped);

    rotated_label_set_markup(label, markup);
    gtk_widget_set_tooltip_text(label, available ? NULL : tooltip);

    g_free(markup);
    g_free(escaped);

    return;
  }

  if (!available) {
    char *markup = g_strdup_printf(
      "<span color=\"#808080\"><s>%s</s></span>", base_name
    );
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_widget_set_tooltip_text(label, tooltip);
    g_free(markup);
  } else {
    gtk_label_set_text(GTK_LABEL(label), base_name);
    gtk_label_set_use_markup(GTK_LABEL(label), FALSE);
    gtk_widget_set_tooltip_text(label, NULL);
  }
}

// Update the cached effective source index for a routing sink.
// Uses cached element pointers for performance.
void update_snk_effective_source(struct routing_snk *r_snk) {
  struct alsa_elem *elem = r_snk->elem;
  struct alsa_card *card = elem->card;

  // Only HW analogue outputs can be affected by speaker switching
  if (elem->port_category != PC_HW || elem->hw_type != HW_TYPE_ANALOGUE) {
    r_snk->effective_source_idx = alsa_get_elem_value(elem);
    return;
  }

  // If not in any group, use normal routing
  if (!snk_in_any_monitor_group(r_snk)) {
    r_snk->effective_source_idx = alsa_get_elem_value(elem);
    return;
  }

  int active = get_active_monitor_group(card);

  if (active >= 0) {
    struct monitor_group_elems *mg = &r_snk->monitor_groups[active];

    if (snk_in_monitor_group(r_snk, active) && mg->source) {
      // In the active group - use its group source
      int vg_idx = alsa_get_elem_value(mg->source);
      if (vg_idx < card->monitor_group_src_map_count) {
        r_snk->effective_source_idx = card->monitor_group_src_map[vg_idx];
        return;
      }
    } else {
      // In a group, but not the active one - muted
      r_snk->effective_source_idx = 0;
      return;
    }
  }

  r_snk->effective_source_idx = alsa_get_elem_value(elem);
}

// Update hardware output label to show monitor group status
static void update_hw_output_label_text(struct routing_snk *r_snk) {
  struct alsa_elem *elem = r_snk->elem;
  struct alsa_card *card = elem->card;

  if (!r_snk->label_widget)
    return;

  // Get the display name (stereo-aware, handles custom names)
  char *base_name = get_snk_stereo_aware_name(r_snk);

  // PCM sinks are "PCM Inputs" in routing = capture to PC
  if (elem->port_category == PC_PCM) {
    set_availability_label(
      r_snk->label_widget, base_name, is_routing_snk_available(r_snk),
      "Unavailable at current sample rate"
    );
    g_free(base_name);
    return;
  }

  // Mixer/DSP sinks - update availability styling
  if (elem->port_category == PC_MIX || elem->port_category == PC_DSP) {
    set_availability_label(
      r_snk->label_widget, base_name, is_routing_snk_available(r_snk),
      "Mixer unavailable at current sample rate"
    );
    g_free(base_name);
    return;
  }

  // Only hardware outputs get monitor group indicators
  if (elem->port_category != PC_HW) {
    g_free(base_name);
    return;
  }

  // Non-analogue outputs - check S/PDIF and ADAT availability
  if (elem->hw_type != HW_TYPE_ANALOGUE) {
    if (is_digital_io_type(elem->hw_type)) {
      set_availability_label(
        r_snk->label_widget, base_name, is_routing_snk_available(r_snk),
        "Unavailable with current Digital I/O mode and sample rate"
      );
    } else {
      gtk_label_set_text(GTK_LABEL(r_snk->label_widget), base_name);
      gtk_label_set_use_markup(GTK_LABEL(r_snk->label_widget), FALSE);
      gtk_widget_set_tooltip_text(r_snk->label_widget, NULL);
    }
    g_free(base_name);
    return;
  }

  // If no monitor group controls or membership, or no group is
  // active, just use the base name
  int active = get_active_monitor_group(card);

  if (!snk_in_any_monitor_group(r_snk) || active < 0) {
    gtk_label_set_text(GTK_LABEL(r_snk->label_widget), base_name);
    gtk_label_set_use_markup(GTK_LABEL(r_snk->label_widget), FALSE);
    g_free(base_name);
    return;
  }

  char *label_text;

  if (snk_in_monitor_group(r_snk, active)) {
    // Active group indicator: green for Main, red for the Alt groups
    label_text = g_strdup_printf(
      "%s <span color=\"%s\"><small>%s</small></span>",
      base_name,
      active == 0 ? "#4a4" : "#f66",
      card->monitor_group_names[active]
    );
  } else {
    // In a group, but not the active one - strikethrough
    label_text = g_strdup_printf("<s>%s</s>", base_name);
  }
  gtk_label_set_markup(GTK_LABEL(r_snk->label_widget), label_text);

  g_free(label_text);
  g_free(base_name);
}

void update_hw_output_label(struct routing_snk *r_snk) {
  if (!r_snk->label_widget)
    return;

  update_hw_output_label_text(r_snk);
  set_port_menu_tooltip(r_snk->label_widget, r_snk->socket_widget);
}

// Update routing source label (stereo-aware name with availability)
static void update_routing_src_label_text(struct routing_src *r_src) {
  if (!r_src->label_widget)
    return;

  char *name = get_src_stereo_aware_name(r_src);
  int available = is_routing_src_available(r_src);
  const char *tooltip = NULL;

  switch (r_src->port_category) {
    case PC_MIX:
      tooltip = "Mixer unavailable at current sample rate";
      break;
    case PC_HW:
      if (is_digital_io_type(r_src->hw_type))
        tooltip =
          "Unavailable with current Digital I/O mode and sample rate";
      break;
    case PC_PCM:
      tooltip = "Unavailable at current sample rate";
      break;
  }

  set_availability_label(r_src->label_widget, name, available, tooltip);

  if (r_src->rotated_label_widget)
    set_availability_label(
      r_src->rotated_label_widget, name, available, tooltip
    );

  // For horizontal grids, set width based on mono/stereo
  if (is_horiz_port_category(r_src->port_category) &&
      GTK_IS_LABEL(r_src->label_widget)) {
    int width = is_src_linked(r_src) ? LABEL_WIDTH_STEREO : LABEL_WIDTH_MONO;

    gtk_label_set_width_chars(GTK_LABEL(r_src->label_widget), width);
  }

  // Preserve tooltip for ellipsised labels (vertical orientation)
  if (available &&
      GTK_IS_LABEL(r_src->label_widget) &&
      gtk_label_get_ellipsize(
        GTK_LABEL(r_src->label_widget)
      ) != PANGO_ELLIPSIZE_NONE)
    gtk_widget_set_tooltip_text(r_src->label_widget, name);

  g_free(name);
}

void update_routing_src_label(struct routing_src *r_src) {
  if (!r_src->label_widget)
    return;

  update_routing_src_label_text(r_src);
  set_port_menu_tooltip(r_src->label_widget, r_src->widget2);

  if (r_src->rotated_label_widget)
    set_port_menu_tooltip(r_src->rotated_label_widget, NULL);

  // naming a mixer output swaps the label under it for the one drawn at
  // an angle, and unnaming it swaps back
  if (r_src->rotated_label_widget &&
      get_src_label_in_use(r_src) != get_src_label_to_show(r_src))
    arrange_src_grid(r_src->card, r_src->port_category);

  // a renamed label reaches a different distance past its column
  else if (r_src->rotated_label_widget)
    update_src_grid_overhang(r_src->card, r_src->port_category);
}

// Update mixer heading labels for availability at high sample rates
static void update_mixer_headings(struct alsa_card *card) {
  int available = get_sample_rate_category(card->current_sample_rate) != SR_HIGH;

  if (card->routing_mixer_in_heading)
    set_availability_label(
      card->routing_mixer_in_heading, "Mixer\nInputs", available,
      "Mixer unavailable at current sample rate"
    );

  if (card->routing_mixer_out_heading) {
    const char *name = card->has_talkback ? "Mixer Outputs" : "Mixer\nOutputs";
    set_availability_label(
      card->routing_mixer_out_heading, name, available,
      "Mixer unavailable at current sample rate"
    );
  }
}

// Update all PCM labels when channel availability changes
void update_all_pcm_labels(struct alsa_card *card) {
  update_levels_availability(card);

  if (!card->window_routing)
    return;

  // Update PCM sources
  for (int i = 0; i < card->routing_srcs->len; i++) {
    struct routing_src *r_src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );
    if (r_src->port_category == PC_PCM)
      update_routing_src_label(r_src);
  }

  // Update PCM sinks
  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *r_snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );
    if (r_snk->elem->port_category == PC_PCM)
      update_hw_output_label(r_snk);
  }
}

// Update all HW I/O and mixer labels when availability changes
void update_all_hw_io_labels(struct alsa_card *card) {
  update_hw_io_limits(card);
  update_levels_availability(card);

  if (!card->window_routing || !card->routing_snks || !card->routing_srcs)
    return;

  // Update routing sinks
  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *r_snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );
    update_hw_output_label(r_snk);
  }

  // Update routing sources (only categories with availability logic)
  for (int i = 0; i < card->routing_srcs->len; i++) {
    struct routing_src *r_src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );
    if (r_src->port_category == PC_HW ||
        r_src->port_category == PC_MIX)
      update_routing_src_label(r_src);
  }

  // Update mixer headings
  update_mixer_headings(card);
}

// Callback when monitor group related controls change
static void monitor_group_changed(struct alsa_elem *elem, void *data) {
  struct alsa_card *card = elem->card;

  // Update all hardware output labels and effective sources
  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *r_snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );
    update_snk_effective_source(r_snk);
    update_hw_output_label(r_snk);
  }
}

// Callback when digital I/O mode changes
static void digital_io_mode_changed(struct alsa_elem *elem, void *data) {
  update_all_hw_io_labels(elem->card);
}

static void routing_updated(struct alsa_elem *elem, void *data) {
  struct alsa_card *card = elem->card;
  struct routing_snk *r_snk = data;

  if (r_snk)
    update_snk_effective_source(r_snk);

  update_mixer_labels(card);
  queue_redraw_group_highlights(card);
  gtk_widget_queue_draw(card->routing_lines);
}

// Create sink widgets (socket and label). Grid attachment handled by arrange functions.
static void make_routing_alsa_elem(struct routing_snk *r_snk) {
  struct alsa_elem *elem = r_snk->elem;
  struct alsa_card *card = elem->card;

  // Skip fixed mixer inputs (no routing widgets)
  if (elem->port_category == PC_MIX && card->has_fixed_mixer_inputs)
    return;

  char *name = get_snk_display_name_formatted(r_snk);
  setup_snk_socket(r_snk);
  setup_snk_label(r_snk, name);
  g_free(name);

  // Initialise Main/Alt and availability label state
  update_hw_output_label(r_snk);

  alsa_elem_add_callback(elem, routing_updated, r_snk, NULL);
}

static void add_routing_widgets(
  struct alsa_card *card,
  GtkWidget        *routing_overlay
) {
  GArray *r_snks = card->routing_snks;

  // Clear widget pointers (handles window reopen after close)
  for (int i = 0; i < card->routing_srcs->len; i++) {
    struct routing_src *r_src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );
    r_src->widget = NULL;
    r_src->widget2 = NULL;
    r_src->label_widget = NULL;
    r_src->rotated_label_widget = NULL;
    r_src->talkback_widget = NULL;
  }
  for (int i = 0; i < r_snks->len; i++) {
    struct routing_snk *r_snk = &g_array_index(r_snks, struct routing_snk, i);
    r_snk->socket_widget = NULL;
    r_snk->label_widget = NULL;
  }

  // Create sink widgets (grid attachment handled by arrange functions)
  for (int i = 0; i < r_snks->len; i++) {
    struct routing_snk *r_snk = &g_array_index(r_snks, struct routing_snk, i);
    make_routing_alsa_elem(r_snk);
  }

  if (!card->routing_out_count[PC_MIX])
    return;

  // Create source widgets (grid attachment handled by arrange functions)
  // Start at 1 to skip the "Off" input
  for (int i = 1; i < card->routing_srcs->len; i++) {
    struct routing_src *r_src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );

    char *name = get_src_display_name_formatted(r_src);
    setup_src_socket(r_src);
    setup_src_label(r_src, name);
    g_free(name);

    // Create talkback widget for mixer outputs if device has talkback
    if (r_src->port_category == PC_MIX && card->has_talkback) {
      GtkWidget *w = make_talkback_mix_widget(card, r_src);
      if (w)
        r_src->talkback_widget = g_object_ref(w);
    }
  }

  // Arrange all grids (handles visibility and removes empty rows/columns)
  arrange_src_grid(card, PC_DSP);
  arrange_src_grid(card, PC_MIX);
  arrange_src_grid(card, PC_HW);
  arrange_src_grid(card, PC_PCM);
  arrange_snk_grid(card, PC_DSP);
  arrange_snk_grid(card, PC_MIX);
  arrange_snk_grid(card, PC_HW);
  arrange_snk_grid(card, PC_PCM);

  card->routing_lines = gtk_drawing_area_new();
  gtk_widget_set_can_target(card->routing_lines, FALSE);
  gtk_drawing_area_set_draw_func(
    GTK_DRAWING_AREA(card->routing_lines), draw_routing_lines, card, NULL
  );
  gtk_overlay_add_overlay(
    GTK_OVERLAY(routing_overlay), card->routing_lines
  );

  // Set up monitor group label updates
  // Register callbacks on speaker switching controls
  struct alsa_elem *ss_enum = get_elem_by_name(
    card->elems, "Speaker Switching Playback Enum"
  );
  struct alsa_elem *ss_switch = get_elem_by_name(
    card->elems, "Speaker Switching Playback Switch"
  );
  struct alsa_elem *ss_alt = get_elem_by_name(
    card->elems, "Speaker Switching Alt Playback Switch"
  );

  if (ss_enum)
    alsa_elem_add_callback(ss_enum, monitor_group_changed, NULL, NULL);
  if (ss_switch)
    alsa_elem_add_callback(ss_switch, monitor_group_changed, NULL, NULL);
  if (ss_alt)
    alsa_elem_add_callback(ss_alt, monitor_group_changed, NULL, NULL);

  if (card->monitor_group_select_elem)
    alsa_elem_add_callback(
      card->monitor_group_select_elem, monitor_group_changed, NULL, NULL
    );

  // Register callbacks on the group enable and source controls
  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );

    for (int g = 0; g < card->monitor_group_count; g++) {
      struct monitor_group_elems *mg = &snk->monitor_groups[g];

      if (mg->enable)
        alsa_elem_add_callback(mg->enable, monitor_group_changed, NULL, NULL);
      if (mg->source)
        alsa_elem_add_callback(mg->source, monitor_group_changed, NULL, NULL);
    }
  }

  // Initialize effective source indices for all sinks
  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *r_snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );
    update_snk_effective_source(r_snk);
  }

  update_mixer_labels(card);

  // set initial visibility of routing sections based on port enable states
  update_routing_section_visibility(card);
}

GtkWidget *create_routing_controls(struct alsa_card *card) {
  if (!card->sample_capture_elem)
    return NULL;

  create_routing_grid(card);

  GtkWidget *top = gtk_frame_new(NULL);
  gtk_widget_add_css_class(top, "window-frame");
  gtk_widget_add_css_class(top, "window-routing");

  GtkWidget *routing_overlay = gtk_overlay_new();
  gtk_widget_add_css_class(routing_overlay, "window-content");
  gtk_widget_add_css_class(routing_overlay, "window-routing");
  gtk_frame_set_child(GTK_FRAME(top), routing_overlay);
  card->routing_overlay = routing_overlay;

  gtk_overlay_set_child(GTK_OVERLAY(routing_overlay), card->routing_grid);

  add_routing_widgets(card, routing_overlay);

  // update HW I/O labels for availability based on Digital I/O mode
  // and sample rate (must be after widgets are created)
  update_all_hw_io_labels(card);

  // Trigger link state callbacks for linked pairs to update socket icons
  // and labels (must be after all other label updates)
  for (int i = 0; i < card->routing_srcs->len; i++) {
    struct routing_src *src = &g_array_index(
      card->routing_srcs, struct routing_src, i
    );
    if (!is_src_left_channel(src))
      continue;
    struct alsa_elem *link_elem = get_src_link_elem(src);
    if (link_elem && alsa_get_elem_value(link_elem))
      alsa_elem_change(link_elem);
  }
  for (int i = 0; i < card->routing_snks->len; i++) {
    struct routing_snk *snk = &g_array_index(
      card->routing_snks, struct routing_snk, i
    );
    if (!is_snk_left_channel(snk))
      continue;
    struct alsa_elem *link_elem = get_snk_link_elem(snk);
    if (link_elem && alsa_get_elem_value(link_elem))
      alsa_elem_change(link_elem);
  }

  // register callback for digital I/O mode changes
  if (card->digital_io_mode_elem)
    alsa_elem_add_callback(
      card->digital_io_mode_elem, digital_io_mode_changed, NULL, NULL
    );

  add_drop_controller_motion(card, routing_overlay);

  // The text of a label drawn at an angle lies outside the widget that
  // holds its tooltip, so the overlay answers for it
  gtk_widget_set_has_tooltip(routing_overlay, TRUE);
  g_signal_connect(
    routing_overlay, "query-tooltip",
    G_CALLBACK(routing_overlay_query_tooltip), card
  );

  // Add hover detection via motion controller on overlay
  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "motion", G_CALLBACK(routing_overlay_motion), card);
  g_signal_connect(motion, "leave", G_CALLBACK(routing_overlay_leave), card);
  gtk_widget_add_controller(routing_overlay, motion);

  // Add unified click handler (hit-tests against combined socket+label bounds)
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "released", G_CALLBACK(routing_overlay_click), card);
  gtk_widget_add_controller(routing_overlay, GTK_EVENT_CONTROLLER(click));

  // Add right-click handler for stereo link context menu
  GtkGesture *right_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), 3);
  g_signal_connect(
    right_click, "released",
    G_CALLBACK(routing_overlay_right_click), card
  );
  gtk_widget_add_controller(
    routing_overlay, GTK_EVENT_CONTROLLER(right_click)
  );

  // Add unified drag source (hit-tests on prepare)
  GtkDragSource *drag_source = gtk_drag_source_new();
  gtk_drag_source_set_actions(drag_source, GDK_ACTION_COPY);
  g_signal_connect(drag_source, "prepare", G_CALLBACK(routing_drag_prepare), card);
  g_signal_connect(drag_source, "drag-end", G_CALLBACK(routing_drag_end), card);
  GdkPaintable *paintable = gdk_paintable_new_empty(1, 1);
  gtk_drag_source_set_icon(drag_source, paintable, 0, 0);
  g_object_unref(paintable);
  gtk_widget_add_controller(routing_overlay, GTK_EVENT_CONTROLLER(drag_source));

  // Add unified drop target (hit-tests on motion/drop)
  GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_COPY);
  g_signal_connect(drop_target, "motion", G_CALLBACK(routing_drop_motion), card);
  g_signal_connect(drop_target, "leave", G_CALLBACK(routing_drop_leave), card);
  g_signal_connect(drop_target, "drop", G_CALLBACK(routing_drop), card);
  gtk_widget_add_controller(routing_overlay, GTK_EVENT_CONTROLLER(drop_target));

  return top;
}
