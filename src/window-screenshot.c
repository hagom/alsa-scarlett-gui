// SPDX-FileCopyrightText: 2022-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "window-screenshot.h"

// How long to wait for one frame, and how long to keep asking for
// frames while the widget draws nothing.
#define FRAME_WAIT_MS     200
#define SNAPSHOT_WAIT_US  G_USEC_PER_SEC

// Set the flag pointed to, for a one-shot idle or timeout.
static gboolean flag_set(gpointer flag) {
  *(gboolean *)flag = TRUE;
  return G_SOURCE_REMOVE;
}

// As above, for a one-shot frame clock tick.
static gboolean frame_flag_set(
  GtkWidget     *widget,
  GdkFrameClock *clock,
  gpointer       flag
) {
  *(gboolean *)flag = TRUE;
  return G_SOURCE_REMOVE;
}

// Yield until a low-priority idle runs, so GTK's higher-priority layout
// and paint work for any pending change (e.g. a tab switch) has completed
// before we capture.
static void wait_for_idle(void) {
  gboolean seen = FALSE;
  g_idle_add_full(G_PRIORITY_LOW, flag_set, &seen, NULL);
  while (!seen)
    g_main_context_iteration(NULL, TRUE);
}

/* Redraw widget and yield until its frame clock has run a frame, or
 * FRAME_WAIT_MS passes. Both sources are one-shot and remove
 * themselves, so only the one that did not fire is removed here.
 */
static void wait_for_frame(GtkWidget *widget) {
  gboolean drawn = FALSE, expired = FALSE;

  guint tick = gtk_widget_add_tick_callback(
    widget, frame_flag_set, &drawn, NULL
  );
  guint timeout = g_timeout_add(FRAME_WAIT_MS, flag_set, &expired);

  gtk_widget_queue_draw(widget);

  while (!drawn && !expired)
    g_main_context_iteration(NULL, TRUE);

  if (!drawn)
    gtk_widget_remove_tick_callback(widget, tick);
  if (!expired)
    g_source_remove(timeout);
}

// Focus and raise the toplevel, then let GTK settle, so every capture is
// of a focused window (consistent decorations) and the window's frame
// clock is running when we snapshot.
static void present_and_settle(GtkWindow *window) {
  if (window)
    gtk_window_present(window);
  wait_for_idle();
}

/* One attempt at the widget's contents, or NULL if it drew nothing.
 *
 * Snapshot via a widget paintable so the capture includes the CSS
 * background and border of the widget itself, which the snapshot vfunc
 * draws for its children but not for the widget it is called on. A
 * freshly created paintable holds no cached frame, so this renders the
 * widget's current state.
 */
static GskRenderNode *snapshot_widget(
  GtkWidget *widget,
  int        width,
  int        height,
  int        scale
) {
  GtkSnapshot *snapshot = gtk_snapshot_new();

  if (scale != 1)
    gtk_snapshot_scale(snapshot, scale, scale);

  GdkPaintable *paintable = gtk_widget_paintable_new(widget);

  gdk_paintable_snapshot(paintable, snapshot, width, height);
  g_object_unref(paintable);

  return gtk_snapshot_free_to_node(snapshot);
}

// Hand the caller the reason a capture failed, taking ownership of
// message so every failure exit is a single line.
static gboolean fail(char **reason, char *message) {
  if (reason)
    *reason = message;
  else
    g_free(message);

  return FALSE;
}

// Render a widget to a PNG file entirely within the process, without any
// compositor screenshot API. This works on Wayland and does not require
// the window to already be unobscured.
gboolean screenshot_widget_to_png(
  GtkWidget   *widget,
  const char  *filename,
  char       **reason
) {
  if (reason)
    *reason = NULL;

  if (!widget)
    return fail(reason, g_strdup("no widget"));

  GtkRoot *root = gtk_widget_get_root(widget);
  GtkWindow *window = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;

  present_and_settle(window);

  if (!gtk_widget_get_mapped(widget))
    return fail(reason, g_strdup_printf(
      "not mapped (visible %s, realized %s)",
      gtk_widget_get_visible(widget) ? "yes" : "no",
      gtk_widget_get_realized(widget) ? "yes" : "no"
    ));

  int width = gtk_widget_get_width(widget);
  int height = gtk_widget_get_height(widget);
  if (width <= 0 || height <= 0)
    return fail(reason, g_strdup_printf("size is %dx%d", width, height));

  GtkNative *native = gtk_widget_get_native(widget);
  if (!native)
    return fail(reason, g_strdup("no native surface"));

  GskRenderer *renderer = gtk_native_get_renderer(native);
  if (!renderer)
    return fail(reason, g_strdup("no renderer"));

  // Render at the widget's scale factor so HiDPI output is crisp
  int scale = gtk_widget_get_scale_factor(widget);

  /* A window opened or presented a moment ago sometimes draws nothing on
   * the first attempt even though it is mapped, sized and not suspended,
   * so keep asking for a frame and trying again for up to
   * SNAPSHOT_WAIT_US rather than failing a documentation run at random.
   */
  gint64 deadline = g_get_monotonic_time() + SNAPSHOT_WAIT_US;
  GskRenderNode *node = snapshot_widget(widget, width, height, scale);

  while (!node && g_get_monotonic_time() < deadline) {
    wait_for_frame(widget);
    node = snapshot_widget(widget, width, height, scale);
  }

  if (!node)
    return fail(reason, g_strdup_printf(
      "nothing drawn at %dx%d scale %d, window %s",
      width, height, scale,
      !window                         ? "is not a toplevel" :
      gtk_window_is_suspended(window) ? "is suspended (minimised or "
                                        "completely covered)"
                                      : "is not suspended"
    ));

  graphene_rect_t bounds =
    GRAPHENE_RECT_INIT(0, 0, width * scale, height * scale);
  GdkTexture *texture = gsk_renderer_render_texture(renderer, node, &bounds);
  gsk_render_node_unref(node);

  GBytes *png = gdk_texture_save_to_png_bytes(texture);
  g_object_unref(texture);

  if (!png)
    return fail(reason, g_strdup("cannot encode PNG"));

  gsize size;
  const char *data = g_bytes_get_data(png, &size);

  GError *error = NULL;
  gboolean ok = g_file_set_contents(filename, data, size, &error);
  g_bytes_unref(png);

  if (!ok) {
    char *why = g_strdup_printf("cannot write file: %s", error->message);
    g_error_free(error);
    return fail(reason, why);
  }

  return TRUE;
}
