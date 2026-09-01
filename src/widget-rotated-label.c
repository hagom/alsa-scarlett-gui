// SPDX-FileCopyrightText: 2022-2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <math.h>

#include "gtkhelper.h"
#include "widget-rotated-label.h"

// Key under which the state below hangs off the drawing area
#define ROTATED_LABEL_KEY "rotated-label"

struct rotated_label {
  char *markup;
  int   text_w;
  int   text_h;
};

static void rotated_label_free(gpointer data) {
  struct rotated_label *info = data;

  g_free(info->markup);
  g_free(info);
}

static struct rotated_label *rotated_label_get(GtkWidget *widget) {
  return g_object_get_data(G_OBJECT(widget), ROTATED_LABEL_KEY);
}

int is_rotated_label(GtkWidget *widget) {
  return widget && rotated_label_get(widget) != NULL;
}

// Measure the text and ask for a column just wide enough for the height
// of the rotated text, and a row tall enough for all of it
static void rotated_label_measure(GtkWidget *widget) {
  struct rotated_label *info = rotated_label_get(widget);

  info->text_w = 0;
  info->text_h = 0;

  if (info->markup && *info->markup) {
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, NULL);

    pango_layout_set_markup(layout, info->markup, -1);
    pango_layout_get_pixel_size(layout, &info->text_w, &info->text_h);
    g_object_unref(layout);
  }

  gtk_widget_set_size_request(
    widget,
    (int)ceil(info->text_h * ROTATED_LABEL_SIN),
    (int)ceil(
      info->text_w * ROTATED_LABEL_SIN + info->text_h * ROTATED_LABEL_COS
    )
  );

  // the text is painted by the widget that surrounds this one, so that
  // is what has to be redrawn when it changes
  GtkWidget *overlay = gtk_widget_get_ancestor(widget, GTK_TYPE_OVERLAY);
  GtkWidget *painter = overlay
    ? gtk_overlay_get_child(GTK_OVERLAY(overlay)) : NULL;

  gtk_widget_queue_draw(painter ? painter : widget);
}

// How far the text reaches to the left of the widget. The widget is
// aligned to the start of its cell and allocated only the width it
// asks for, so that width is where rotated_label_transform() anchors
// the end of the text, however wide the cell around it is.
int rotated_label_get_overhang(GtkWidget *widget) {
  struct rotated_label *info = rotated_label_get(widget);

  if (!info)
    return 0;

  int overhang = (int)ceil(
    info->text_w * ROTATED_LABEL_COS - info->text_h * ROTATED_LABEL_SIN
  );

  return overhang > 0 ? overhang : 0;
}

// Put the origin at the top right of the widget with the axes turned to
// match the text, so that the text and anything drawn behind it share
// the same coordinates
static int rotated_label_transform(
  GtkWidget *widget,
  GtkWidget *target,
  cairo_t   *cr
) {
  struct rotated_label *info = rotated_label_get(widget);

  if (!info || !info->markup || !*info->markup)
    return 0;

  if (!gtk_widget_get_mapped(widget))
    return 0;

  graphene_point_t origin;
  if (!gtk_widget_compute_point(
        widget, target, &GRAPHENE_POINT_INIT(0, 0), &origin
      ))
    return 0;

  // the text rises to the right and ends at the top right corner, so
  // that the end of the name meets the port it belongs to
  cairo_translate(cr, origin.x + gtk_widget_get_width(widget), origin.y);
  cairo_rotate(cr, -ROTATED_LABEL_ANGLE);
  cairo_translate(cr, -info->text_w, 0);

  return 1;
}

// The same transform as a matrix, for turning a point in the
// surrounding widget's coordinates into the text's own
static int rotated_label_matrix(
  GtkWidget      *widget,
  GtkWidget      *target,
  cairo_matrix_t *matrix
) {
  struct rotated_label *info = rotated_label_get(widget);

  if (!info || !info->markup || !*info->markup)
    return 0;

  if (!gtk_widget_get_mapped(widget))
    return 0;

  graphene_point_t origin;
  if (!gtk_widget_compute_point(
        widget, target, &GRAPHENE_POINT_INIT(0, 0), &origin
      ))
    return 0;

  cairo_matrix_init_identity(matrix);
  cairo_matrix_translate(
    matrix, origin.x + gtk_widget_get_width(widget), origin.y
  );
  cairo_matrix_rotate(matrix, -ROTATED_LABEL_ANGLE);
  cairo_matrix_translate(matrix, -info->text_w, 0);

  return 1;
}

int rotated_label_contains_point(
  GtkWidget *widget,
  GtkWidget *target,
  double     x,
  double     y,
  double     pad
) {
  struct rotated_label *info = rotated_label_get(widget);
  cairo_matrix_t matrix;

  if (!rotated_label_matrix(widget, target, &matrix))
    return 0;

  if (cairo_matrix_invert(&matrix) != CAIRO_STATUS_SUCCESS)
    return 0;

  cairo_matrix_transform_point(&matrix, &x, &y);

  return x >= -pad && x <= info->text_w + pad &&
         y >= -pad && y <= info->text_h + pad;
}

void rotated_label_append_outline(
  GtkWidget *widget,
  GtkWidget *target,
  cairo_t   *cr,
  double     pad
) {
  struct rotated_label *info = rotated_label_get(widget);

  cairo_save(cr);

  if (rotated_label_transform(widget, target, cr))
    cairo_rectangle(
      cr, -pad, -pad, info->text_w + pad * 2, info->text_h + pad * 2
    );

  cairo_restore(cr);
}

void rotated_label_draw_on(
  GtkWidget *widget,
  GtkWidget *target,
  cairo_t   *cr
) {
  struct rotated_label *info = rotated_label_get(widget);

  if (!info || !info->markup || !*info->markup)
    return;

  PangoLayout *layout = gtk_widget_create_pango_layout(widget, NULL);
  pango_layout_set_markup(layout, info->markup, -1);

  cairo_save(cr);

  if (rotated_label_transform(widget, target, cr)) {
    GdkRGBA color;

    gtk_widget_get_color_compat(widget, &color);
    gdk_cairo_set_source_rgba(cr, &color);
    pango_cairo_show_layout(cr, layout);
  }

  cairo_restore(cr);

  g_object_unref(layout);
}

// The theme's font is not known until the widget has been realized, so
// measure again once it has
static void rotated_label_realize(GtkWidget *widget, gpointer user_data) {
  rotated_label_measure(widget);
}

GtkWidget *rotated_label_new(void) {
  GtkWidget *widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  struct rotated_label *info = g_new0(struct rotated_label, 1);

  g_object_set_data_full(
    G_OBJECT(widget), ROTATED_LABEL_KEY, info, rotated_label_free
  );

  gtk_widget_set_valign(widget, GTK_ALIGN_START);
  gtk_widget_set_halign(widget, GTK_ALIGN_START);

  g_signal_connect(
    widget, "realize", G_CALLBACK(rotated_label_realize), NULL
  );

  return widget;
}

void rotated_label_set_markup(GtkWidget *widget, const char *markup) {
  struct rotated_label *info = rotated_label_get(widget);

  if (!info)
    return;

  g_free(info->markup);
  info->markup = g_strdup(markup);

  rotated_label_measure(widget);
}
