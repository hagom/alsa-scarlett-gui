// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "window-screenshot.h"

static gboolean idle_seen(gpointer seen) {
  *(gboolean *)seen = TRUE;
  return G_SOURCE_REMOVE;
}

// Yield until a low-priority idle runs, so GTK's higher-priority layout
// and paint work for any pending change (e.g. a tab switch) has completed
// before we capture.
static void wait_for_idle(void) {
  gboolean seen = FALSE;
  g_idle_add_full(G_PRIORITY_LOW, idle_seen, &seen, NULL);
  while (!seen)
    g_main_context_iteration(NULL, TRUE);
}

// Focus and raise the toplevel, then let GTK settle, so every capture is
// of a focused window (consistent decorations) and the window's frame
// clock is running when we snapshot.
static void present_and_settle(GtkWidget *widget) {
  GtkRoot *root = gtk_widget_get_root(widget);
  if (GTK_IS_WINDOW(root))
    gtk_window_present(GTK_WINDOW(root));
  wait_for_idle();
}

// Render a widget to a PNG file entirely within the process, without any
// compositor screenshot API. This works on Wayland and does not require
// the window to already be unobscured.
gboolean screenshot_widget_to_png(
  GtkWidget  *widget,
  const char *filename
) {
  if (!widget)
    return FALSE;

  present_and_settle(widget);

  if (!gtk_widget_get_mapped(widget))
    return FALSE;

  int width = gtk_widget_get_width(widget);
  int height = gtk_widget_get_height(widget);
  if (width <= 0 || height <= 0)
    return FALSE;

  GtkNative *native = gtk_widget_get_native(widget);
  if (!native)
    return FALSE;

  GskRenderer *renderer = gtk_native_get_renderer(native);
  if (!renderer)
    return FALSE;

  // Render at the widget's scale factor so HiDPI output is crisp
  int scale = gtk_widget_get_scale_factor(widget);

  GtkSnapshot *snapshot = gtk_snapshot_new();
  if (scale != 1)
    gtk_snapshot_scale(snapshot, scale, scale);

  // Build the render node directly from the widget's current state, so
  // the capture does not depend on a cached drawn frame.
  GtkWidgetClass *klass = GTK_WIDGET_GET_CLASS(widget);
  if (klass->snapshot)
    klass->snapshot(widget, snapshot);

  GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
  if (!node)
    return FALSE;

  graphene_rect_t bounds =
    GRAPHENE_RECT_INIT(0, 0, width * scale, height * scale);
  GdkTexture *texture = gsk_renderer_render_texture(renderer, node, &bounds);
  gsk_render_node_unref(node);

  gboolean ok = gdk_texture_save_to_png(texture, filename);
  g_object_unref(texture);

  return ok;
}
