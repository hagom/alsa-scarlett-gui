// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "alsa.h"
#include "window-geometry.h"

// Object-data keys marking a documented widget: DOC_ELEM_KEY holds the
// ALSA element, DOC_NAME_KEY holds a name for widgets that have none.
#define DOC_ELEM_KEY "doc-elem"
#define DOC_NAME_KEY "doc-name"

void widget_doc_register(GtkWidget *widget, struct alsa_elem *elem) {
  g_object_set_data(G_OBJECT(widget), DOC_ELEM_KEY, elem);
}

void widget_doc_register_named(GtkWidget *widget, const char *name) {
  g_object_set_data_full(
    G_OBJECT(widget), DOC_NAME_KEY, g_strdup(name), g_free
  );
}

// A documented region within a window, in PNG pixel coordinates.
struct region {
  char *id;      // group only: stable id for the crop it drives
  char *title;   // group only: the section heading
  char *name;    // control only: the ALSA element name
  char *tooltip; // control only: the widget's tooltip, or NULL
  int   x, y, w, h;
};

// Stable id for a section, so the crop it drives has a predictable name
// (e.g. <pid>-main-inputs.png). Falls back to a slug of the title.
static char *region_id_for_title(const char *title) {
  char *lower = g_ascii_strdown(title, -1);

  const char *id = NULL;
  if (strstr(lower, "global"))
    id = "global";
  else if (strstr(lower, "input"))
    id = "inputs";
  else if (strstr(lower, "output"))
    id = "outputs";

  if (id) {
    g_free(lower);
    return g_strdup(id);
  }

  // slug: spaces and slashes to hyphens
  for (char *p = lower; *p; p++)
    if (*p == ' ' || *p == '/')
      *p = '-';
  return lower;
}

// Bounds of widget in root's coordinate space, scaled to PNG pixels.
// Rounds outward so a crop to this box never clips the region. Returns
// FALSE if the widget has no bounds relative to root (e.g. unmapped).
static gboolean region_bounds(
  GtkWidget       *widget,
  GtkWidget       *root,
  int              scale,
  struct region   *out
) {
  graphene_rect_t r;
  if (!gtk_widget_compute_bounds(widget, root, &r))
    return FALSE;

  int x0 = (int)floorf(r.origin.x * scale);
  int y0 = (int)floorf(r.origin.y * scale);
  int x1 = (int)ceilf((r.origin.x + r.size.width) * scale);
  int y1 = (int)ceilf((r.origin.y + r.size.height) * scale);

  out->x = x0;
  out->y = y0;
  out->w = x1 - x0;
  out->h = y1 - y0;

  return out->w > 0 && out->h > 0;
}

// Collect the documented regions. A "controls-label" widget titles a
// section, and its parent box is the section's extent (label plus
// content), which is what a section crop wants. A widget carrying a
// doc-elem is a control, recorded with its element name and tooltip.
static void collect_regions(
  GtkWidget *widget,
  GtkWidget *root,
  int        scale,
  GArray    *groups,
  GArray    *controls
) {
  // Skip unmapped subtrees: a widget on a hidden notebook page still
  // has an allocation compute_bounds would report, but it is not in
  // the captured image. Mapped-ness propagates, so pruning here drops
  // every off-tab descendant too.
  if (!gtk_widget_get_mapped(widget))
    return;

  if (gtk_widget_has_css_class(widget, "controls-label") &&
      GTK_IS_LABEL(widget)) {
    GtkWidget *section = gtk_widget_get_parent(widget);

    struct region region = {0};
    if (section && region_bounds(section, root, scale, &region)) {
      const char *title = gtk_label_get_text(GTK_LABEL(widget));
      region.id = region_id_for_title(title);
      region.title = g_strdup(title);
      g_array_append_val(groups, region);
    }
  }

  struct alsa_elem *elem = g_object_get_data(G_OBJECT(widget), DOC_ELEM_KEY);
  const char *named = g_object_get_data(G_OBJECT(widget), DOC_NAME_KEY);
  if (elem || named) {
    struct region region = {0};
    if (region_bounds(widget, root, scale, &region)) {
      region.name = g_strdup(elem ? elem->name : named);
      region.tooltip = g_strdup(gtk_widget_get_tooltip_text(widget));
      g_array_append_val(controls, region);
    }
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

// Append the "[ ... ]" body of a region array, then free its contents.
static void append_regions(
  GString *out,
  GArray  *regions,
  gboolean is_group
) {
  for (guint i = 0; i < regions->len; i++) {
    struct region *region = &g_array_index(regions, struct region, i);
    g_string_append(out, i ? ",\n" : "\n");

    if (is_group) {
      g_string_append(out, "    { \"id\": ");
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

    g_free(region->id);
    g_free(region->title);
    g_free(region->name);
    g_free(region->tooltip);
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

  GArray *groups = g_array_new(FALSE, FALSE, sizeof(struct region));
  GArray *controls = g_array_new(FALSE, FALSE, sizeof(struct region));
  collect_regions(root, root, scale, groups, controls);

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
  append_regions(out, groups, TRUE);
  g_string_append(out, ",\n  \"controls\": [");
  append_regions(out, controls, FALSE);
  g_string_append(out, "\n}\n");

  g_array_free(groups, TRUE);
  g_array_free(controls, TRUE);

  GError *error = NULL;
  gboolean ok = g_file_set_contents(filename, out->str, out->len, &error);
  if (!ok) {
    fprintf(stderr, "can't write %s: %s\n", filename, error->message);
    g_error_free(error);
  }

  g_string_free(out, TRUE);
  return ok;
}
