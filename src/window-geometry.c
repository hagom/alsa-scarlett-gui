// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <math.h>
#include <stdio.h>

#include "alsa.h"
#include "window-geometry.h"

/* Object-data keys marking a documented widget: DOC_CONTROLS_KEY holds
 * the controls it represents, DOC_GROUPS_KEY the sections it belongs to.
 * Both are lists, because a widget can be part of more than one of
 * either: a mixer gain lies in both a mix's row and a mixer input's
 * column, and a talkback button is both the talkback switch and the
 * place its mix's name is written.
 */
#define DOC_CONTROLS_KEY "doc-controls"
#define DOC_GROUPS_KEY "doc-groups"

// One control a widget represents, by element or by name.
struct doc_control {
  struct alsa_elem *elem;
  char             *name;
};

static void doc_control_free(void *data) {
  struct doc_control *dc = data;

  g_free(dc->name);
  g_free(dc);
}

// One section a widget belongs to.
struct doc_group {
  char *group;
  char *id;
  char *title;
};

static void doc_group_free(void *data) {
  struct doc_group *dg = data;

  g_free(dg->group);
  g_free(dg->id);
  g_free(dg->title);
  g_free(dg);
}

// The widget's list of memberships of one kind, created on first use.
static GPtrArray *doc_list(
  GtkWidget      *widget,
  const char     *key,
  GDestroyNotify  free_func
) {
  GPtrArray *list = g_object_get_data(G_OBJECT(widget), key);

  if (!list) {
    list = g_ptr_array_new_with_free_func(free_func);
    g_object_set_data_full(
      G_OBJECT(widget), key, list, (GDestroyNotify)g_ptr_array_unref
    );
  }

  return list;
}

void widget_doc_register(GtkWidget *widget, struct alsa_elem *elem) {
  // callers pass the element they found, which the device may not have
  if (!elem)
    return;

  struct doc_control *dc = g_malloc0(sizeof(struct doc_control));

  dc->elem = elem;

  g_ptr_array_add(
    doc_list(widget, DOC_CONTROLS_KEY, doc_control_free), dc
  );
}

void widget_doc_register_named(GtkWidget *widget, const char *name) {
  struct doc_control *dc = g_malloc0(sizeof(struct doc_control));

  dc->name = g_strdup(name);

  g_ptr_array_add(
    doc_list(widget, DOC_CONTROLS_KEY, doc_control_free), dc
  );
}

void widget_doc_register_group(
  GtkWidget  *widget,
  const char *group,
  const char *instance,
  const char *title
) {
  struct doc_group *dg = g_malloc0(sizeof(struct doc_group));

  dg->group = g_strdup(group);
  dg->id = instance
    ? g_strdup_printf("%s-%s", group, instance)
    : g_strdup(group);
  dg->title = g_strdup(title);

  g_ptr_array_add(
    doc_list(widget, DOC_GROUPS_KEY, doc_group_free), dg
  );
}

// A documented region within a window, in PNG pixel coordinates.
struct region {
  char *id;      // what the widgets contributing to this region share
  char *group;   // group only: the documentation group it belongs to
  char *title;   // group only: the section heading
  char *name;    // control only: the ALSA element name
  char *tooltip; // control only: the widget's tooltip, or NULL
  int   x, y, w, h;
};

/* Regions collected so far, in the order they were first seen, with an
 * index so that a widget can join a region an earlier widget started.
 */
struct region_set {
  GArray     *regions;
  GHashTable *index;   // id -> position in regions, plus one
};

static void region_set_init(struct region_set *set) {
  set->regions = g_array_new(FALSE, FALSE, sizeof(struct region));
  set->index = g_hash_table_new(g_str_hash, g_str_equal);
}

static void region_set_clear(struct region_set *set) {
  // the index is keyed by the regions' own id strings, so it goes
  // before they are freed
  g_hash_table_destroy(set->index);

  for (guint i = 0; i < set->regions->len; i++) {
    struct region *region = &g_array_index(set->regions, struct region, i);

    g_free(region->id);
    g_free(region->group);
    g_free(region->title);
    g_free(region->name);
    g_free(region->tooltip);
  }

  g_array_free(set->regions, TRUE);
}

/* Bounds of widget in root's coordinate space, scaled to PNG pixels.
 * Rounds outward so a crop to this box never clips the region. Returns
 * FALSE if the widget has no bounds relative to root (e.g. unmapped).
 */
static gboolean widget_box(
  GtkWidget *widget,
  GtkWidget *root,
  int        scale,
  int       *x0,
  int       *y0,
  int       *x1,
  int       *y1
) {
  graphene_rect_t r;
  if (!gtk_widget_compute_bounds(widget, root, &r))
    return FALSE;

  *x0 = (int)floorf(r.origin.x * scale);
  *y0 = (int)floorf(r.origin.y * scale);
  *x1 = (int)ceilf((r.origin.x + r.size.width) * scale);
  *y1 = (int)ceilf((r.origin.y + r.size.height) * scale);

  return *x1 > *x0 && *y1 > *y0;
}

/* Add widget's box to the region called id, starting that region if
 * this is the first widget in it. Returns the region, or NULL when the
 * widget has no box to contribute. A region several widgets share ends
 * up as the union of their boxes: a routing port's socket and its
 * label, which sit apart in the grid, or a mixer row's label and every
 * gain along it.
 */
static struct region *region_add(
  struct region_set *set,
  const char        *id,
  GtkWidget         *widget,
  GtkWidget         *root,
  int                scale
) {
  int x0, y0, x1, y1;
  if (!widget_box(widget, root, scale, &x0, &y0, &x1, &y1))
    return NULL;

  gpointer pos = g_hash_table_lookup(set->index, id);

  if (pos) {
    struct region *region = &g_array_index(
      set->regions, struct region, GPOINTER_TO_UINT(pos) - 1
    );

    int rx1 = MAX(region->x + region->w, x1);
    int ry1 = MAX(region->y + region->h, y1);

    region->x = MIN(region->x, x0);
    region->y = MIN(region->y, y0);
    region->w = rx1 - region->x;
    region->h = ry1 - region->y;

    return region;
  }

  struct region region = {0};

  region.id = g_strdup(id);
  region.x = x0;
  region.y = y0;
  region.w = x1 - x0;
  region.h = y1 - y0;
  g_array_append_val(set->regions, region);

  guint i = set->regions->len;
  struct region *added = &g_array_index(set->regions, struct region, i - 1);

  // keyed by the region's own copy of the id, so the two live and die
  // together
  g_hash_table_insert(set->index, added->id, GUINT_TO_POINTER(i));

  return added;
}

/* Collect the documented regions. A widget carrying doc-groups is in
 * each of those sections, recorded with the id its crop takes and its
 * title. One carrying doc-controls is part of each of those controls,
 * recorded with its name and tooltip.
 */
static void collect_regions(
  GtkWidget         *widget,
  GtkWidget         *root,
  int                scale,
  struct region_set *groups,
  struct region_set *controls
) {
  /* Skip unmapped subtrees: a widget on a hidden notebook page still
   * has an allocation compute_bounds would report, but it is not in
   * the captured image. Mapped-ness propagates, so pruning here drops
   * every off-tab descendant too.
   */
  if (!gtk_widget_get_mapped(widget))
    return;

  GPtrArray *dgs = g_object_get_data(G_OBJECT(widget), DOC_GROUPS_KEY);

  for (guint i = 0; dgs && i < dgs->len; i++) {
    struct doc_group *dg = g_ptr_array_index(dgs, i);
    struct region *region = region_add(groups, dg->id, widget, root, scale);

    // the widget that starts a section titles it
    if (region && !region->group) {
      region->group = g_strdup(dg->group);
      region->title = g_strdup(dg->title);
    }
  }

  GPtrArray *dcs = g_object_get_data(G_OBJECT(widget), DOC_CONTROLS_KEY);

  for (guint i = 0; dcs && i < dcs->len; i++) {
    struct doc_control *dc = g_ptr_array_index(dcs, i);
    const char *name = dc->elem ? dc->elem->name : dc->name;

    if (!name)
      continue;

    struct region *region = region_add(controls, name, widget, root, scale);

    if (region && !region->name)
      region->name = g_strdup(name);

    // a later widget of the same control supplies the tooltip only
    // where the earlier ones had none
    if (region && !region->tooltip)
      region->tooltip = g_strdup(gtk_widget_get_tooltip_text(widget));
  }

  for (GtkWidget *child = gtk_widget_get_first_child(widget);
       child;
       child = gtk_widget_get_next_sibling(child))
    collect_regions(child, root, scale, groups, controls);
}

// Append s as a quoted, escaped JSON string (null becomes JSON null).
static void append_json_string(GString *out, const char *s) {
  if (!s) {
    g_string_append(out, "null");
    return;
  }
  g_string_append_c(out, '"');
  for (const char *p = s; *p; p++) {
    if (*p == '"' || *p == '\\')
      g_string_append_c(out, '\\');
    g_string_append_c(out, *p);
  }
  g_string_append_c(out, '"');
}

// Append the "[ ... ]" body of a region array.
static void append_regions(
  GString  *out,
  GArray   *regions,
  gboolean  is_group
) {
  for (guint i = 0; i < regions->len; i++) {
    struct region *region = &g_array_index(regions, struct region, i);
    g_string_append(out, i ? ",\n" : "\n");

    if (is_group) {
      g_string_append(out, "    { \"group\": ");
      append_json_string(out, region->group);
      g_string_append(out, ", \"id\": ");
      append_json_string(out, region->id);
      g_string_append(out, ", \"title\": ");
      append_json_string(out, region->title);
    } else {
      g_string_append(out, "    { \"name\": ");
      append_json_string(out, region->name);
      g_string_append(out, ", \"tooltip\": ");
      append_json_string(out, region->tooltip);
    }

    g_string_append_printf(
      out,
      ", \"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d }",
      region->x, region->y, region->w, region->h
    );
  }
  g_string_append(out, regions->len ? "\n  ]" : "]");
}

gboolean export_widget_geometry_to_json(
  GtkWidget  *root,
  const char *image_name,
  const char *filename
) {
  if (!root || !gtk_widget_get_mapped(root))
    return FALSE;

  int scale = gtk_widget_get_scale_factor(root);

  struct region_set groups, controls;

  region_set_init(&groups);
  region_set_init(&controls);
  collect_regions(root, root, scale, &groups, &controls);

  GString *out = g_string_new("{\n");
  g_string_append(out, "  \"image\": ");
  append_json_string(out, image_name);
  g_string_append_printf(
    out,
    ",\n  \"scale\": %d,\n  \"width\": %d,\n  \"height\": %d,\n",
    scale,
    gtk_widget_get_width(root) * scale,
    gtk_widget_get_height(root) * scale
  );

  g_string_append(out, "  \"groups\": [");
  append_regions(out, groups.regions, TRUE);
  g_string_append(out, ",\n  \"controls\": [");
  append_regions(out, controls.regions, FALSE);
  g_string_append(out, "\n}\n");

  region_set_clear(&groups);
  region_set_clear(&controls);

  GError *error = NULL;
  gboolean ok = g_file_set_contents(filename, out->str, out->len, &error);
  if (!ok) {
    fprintf(stderr, "can't write %s: %s\n", filename, error->message);
    g_error_free(error);
  }

  g_string_free(out, TRUE);
  return ok;
}
