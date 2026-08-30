// SPDX-FileCopyrightText: 2022-2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "port-enable.h"
#include "routing-port-menu.h"
#include "stereo-link.h"
#include "window-routing.h"

// The routing groups, in the order they are listed in the hidden ports
// menu. The titles match the headings shown in the routing window.
struct port_group {
  int         is_src;
  int         port_category;
  const char *title;
};

static const struct port_group port_groups[] = {
  { 1, PC_HW,  "Hardware Inputs"  },
  { 1, PC_PCM, "PCM Outputs"      },
  { 1, PC_DSP, "DSP Outputs"      },
  { 1, PC_MIX, "Mixer Outputs"    },
  { 0, PC_HW,  "Hardware Outputs" },
  { 0, PC_PCM, "PCM Inputs"       },
  { 0, PC_DSP, "DSP Inputs"       },
  { 0, PC_MIX, "Mixer Inputs"     }
};

#define PORT_GROUP_COUNT ((int)G_N_ELEMENTS(port_groups))

static const struct port_group *find_port_group(
  int is_src,
  int port_category
) {
  for (int i = 0; i < PORT_GROUP_COUNT; i++)
    if (port_groups[i].is_src == is_src &&
        port_groups[i].port_category == port_category)
      return &port_groups[i];

  return NULL;
}

// Mixer and DSP ports are labelled with just a letter or number, which
// only makes sense next to the group heading
static int name_needs_group_title(int port_category) {
  return port_category == PC_MIX || port_category == PC_DSP;
}

static char *qualify_name(int is_src, int port_category, char *name) {
  if (!name_needs_group_title(port_category))
    return name;

  const struct port_group *group = find_port_group(is_src, port_category);
  if (!group)
    return name;

  char *qualified = g_strdup_printf("%s %s", group->title, name);
  g_free(name);

  return qualified;
}

char *get_src_qualified_name(struct routing_src *src) {
  return qualify_name(
    1, src->port_category, get_src_stereo_aware_name(src)
  );
}

char *get_snk_qualified_name(struct routing_snk *snk) {
  return qualify_name(
    0, snk->elem->port_category, get_snk_stereo_aware_name(snk)
  );
}

GtkWidget *routing_menu_popover_new(GtkWidget *widget, double x, double y) {
  GtkWidget *popover = gtk_popover_new();

  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_widget_set_parent(popover, widget);

  GdkRectangle rect = { .x = x, .y = y, .width = 1, .height = 1 };
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

  g_signal_connect(popover, "closed", G_CALLBACK(gtk_widget_unparent), NULL);

  return popover;
}

// Mixer sinks are only listed when the device lets them be routed
static int group_is_routable(
  struct alsa_card        *card,
  const struct port_group *group
) {
  return !(
    !group->is_src &&
    group->port_category == PC_MIX &&
    card->has_fixed_mixer_inputs
  );
}

// A checkbox that shows and sets the state of the port checkboxes listed
// under it, the way the column headings in the I/O configuration tabs do:
// ticked when every port below it is shown, mixed when only some are.
struct group_check {
  GtkWidget *check;
  GPtrArray *children;  // GtkCheckButton * of the ports below
  int        updating;
  int        was_inconsistent;
};

static void group_check_set_state(
  struct group_check *gc,
  int                 active,
  int                 inconsistent
) {
  gtk_check_button_set_active(GTK_CHECK_BUTTON(gc->check), active);
  gtk_check_button_set_inconsistent(
    GTK_CHECK_BUTTON(gc->check), inconsistent
  );

  gc->was_inconsistent = inconsistent;
}

static void group_check_update(struct group_check *gc) {
  if (gc->updating || !gc->children->len)
    return;

  int shown = 0;

  for (guint i = 0; i < gc->children->len; i++)
    if (gtk_check_button_get_active(g_ptr_array_index(gc->children, i)))
      shown++;

  gc->updating = 1;

  if (shown == gc->children->len)
    group_check_set_state(gc, 1, 0);
  else if (!shown)
    group_check_set_state(gc, 0, 0);
  else
    group_check_set_state(gc, 0, 1);

  gc->updating = 0;
}

static void group_check_toggled(GtkCheckButton *button, gpointer user_data) {
  struct group_check *gc = user_data;

  if (gc->updating)
    return;

  gc->updating = 1;

  // clicking a mixed checkbox hides everything below it
  int shown = gc->was_inconsistent
    ? 0
    : gtk_check_button_get_active(button);

  group_check_set_state(gc, shown, 0);

  for (guint i = 0; i < gc->children->len; i++)
    gtk_check_button_set_active(
      g_ptr_array_index(gc->children, i), shown
    );

  gc->updating = 0;
}

static void group_check_free(void *user_data) {
  struct group_check *gc = user_data;

  g_ptr_array_free(gc->children, TRUE);
  g_free(gc);
}

// The group checks live as long as the menu they are in
static struct group_check *group_check_new(
  GPtrArray  *group_checks,
  const char *title
) {
  struct group_check *gc = g_new0(struct group_check, 1);

  gc->children = g_ptr_array_new();
  gc->check = gtk_check_button_new_with_label(title);

  gtk_widget_add_css_class(gc->check, "heading");
  g_signal_connect(gc->check, "toggled", G_CALLBACK(group_check_toggled), gc);

  g_ptr_array_add(group_checks, gc);

  return gc;
}

// One port checkbox: exactly one of src and snk is set
struct port_check_data {
  struct routing_src *src;
  struct routing_snk *snk;
  struct group_check *gc;
};

static void port_check_toggled(GtkCheckButton *check, gpointer user_data) {
  struct port_check_data *data = user_data;
  int shown = gtk_check_button_get_active(check);

  if (data->src)
    set_src_port_enabled(data->src, shown);
  else
    set_snk_port_enabled(data->snk, shown);

  group_check_update(data->gc);
}

static void append_port_check(
  GtkWidget          *list,
  struct group_check *gc,
  struct routing_src *src,
  struct routing_snk *snk,
  char               *name,
  int                 shown
) {
  GtkWidget *check = gtk_check_button_new_with_label(name);
  g_free(name);

  gtk_check_button_set_active(GTK_CHECK_BUTTON(check), shown);

  struct port_check_data *data = g_new0(struct port_check_data, 1);
  data->src = src;
  data->snk = snk;
  data->gc = gc;

  g_signal_connect_data(
    check, "toggled", G_CALLBACK(port_check_toggled), data,
    (GClosureNotify)g_free, 0
  );

  gtk_box_append(GTK_BOX(list), check);
  g_ptr_array_add(gc->children, check);
}

// Append a checkbox for each port of the group that the routing window
// draws a socket for.
// Returns the number of checkboxes appended.
static int append_group_rows(
  GtkWidget               *list,
  struct group_check      *gc,
  struct alsa_card        *card,
  const struct port_group *group
) {
  int count = 0;

  if (!group_is_routable(card, group))
    return 0;

  if (group->is_src) {
    // index 0 is the "Off" source, which has no socket
    for (int i = 1; i < card->routing_srcs->len; i++) {
      struct routing_src *src = &g_array_index(
        card->routing_srcs, struct routing_src, i
      );

      if (src->port_category != group->port_category)
        continue;

      if (!should_display_src(src) || !get_src_enable_elem(src))
        continue;

      int shown = is_routing_src_enabled(src);

      append_port_check(
        list, gc, src, NULL, get_src_stereo_aware_name(src), shown
      );
      count++;
    }
  } else {
    for (int i = 0; i < card->routing_snks->len; i++) {
      struct routing_snk *snk = &g_array_index(
        card->routing_snks, struct routing_snk, i
      );

      if (snk->elem->port_category != group->port_category)
        continue;

      if (!should_display_snk(snk) || !get_snk_enable_elem(snk))
        continue;

      int shown = is_routing_snk_enabled(snk);

      append_port_check(
        list, gc, NULL, snk, get_snk_stereo_aware_name(snk), shown
      );
      count++;
    }
  }

  return count;
}

// Wrap the checkbox list so that a long list scrolls rather than growing
// the menu past the bottom of the screen
static GtkWidget *make_scrolled_list(GtkWidget *list) {
  GtkWidget *scrolled = gtk_scrolled_window_new();

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
  gtk_scrolled_window_set_policy(
    GTK_SCROLLED_WINDOW(scrolled),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC
  );
  gtk_scrolled_window_set_propagate_natural_width(
    GTK_SCROLLED_WINDOW(scrolled), TRUE
  );
  gtk_scrolled_window_set_propagate_natural_height(
    GTK_SCROLLED_WINDOW(scrolled), TRUE
  );
  gtk_scrolled_window_set_max_content_height(
    GTK_SCROLLED_WINDOW(scrolled), 400
  );

  return scrolled;
}

// The ports are listed indented under the checkbox that controls them all
static GtkWidget *make_indented_list(void) {
  GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

  gtk_widget_set_margin_start(list, 20);

  return list;
}

static void destroy_unparented(GtkWidget *widget) {
  g_object_ref_sink(widget);
  g_object_unref(widget);
}

// Wrap the menu contents in an unparented popover, handing the group
// checks to it to own
static GtkWidget *make_port_menu_popover(
  GtkWidget *box,
  GPtrArray *group_checks
) {
  GtkWidget *popover = gtk_popover_new();

  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_popover_set_child(GTK_POPOVER(popover), box);

  g_object_set_data_full(
    G_OBJECT(popover), "group-checks", group_checks,
    (GDestroyNotify)g_ptr_array_unref
  );

  return popover;
}

static GPtrArray *group_checks_new(void) {
  return g_ptr_array_new_with_free_func(group_check_free);
}

GtkWidget *routing_port_menu_group_popover(
  struct alsa_card *card,
  int               is_src,
  int               port_category
) {
  const struct port_group *group = find_port_group(is_src, port_category);

  if (!group)
    return NULL;

  GPtrArray *group_checks = group_checks_new();
  struct group_check *gc = group_check_new(group_checks, group->title);
  GtkWidget *list = make_indented_list();

  if (!append_group_rows(list, gc, card, group)) {
    destroy_unparented(list);
    destroy_unparented(gc->check);
    g_ptr_array_unref(group_checks);
    return NULL;
  }

  group_check_update(gc);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_box_append(GTK_BOX(box), gc->check);
  gtk_box_append(GTK_BOX(box), make_scrolled_list(list));

  return make_port_menu_popover(box, group_checks);
}
