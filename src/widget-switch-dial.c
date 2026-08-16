// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gtkdial.h"
#include "widget-boolean.h"
#include "widget-switch-dial.h"
#include "window-geometry.h"

// The level dial pops up as a floating child of the main window's
// GtkOverlay rather than a GtkPopover: a popover is a separate surface
// and its drag gestures are cancelled when the pointer leaves it.
struct switch_dial {
  struct alsa_elem *level_elem;
  GtkWidget        *dial;
  GtkWidget        *arrow;
  GtkWidget        *popup;
  GtkWidget        *overlay;
  GtkEventController *outside_click;
};

static void dial_changed(GtkWidget *dial, gpointer user_data) {
  struct switch_dial *data = user_data;

  long value = (long)gtk_dial_get_value(GTK_DIAL(dial));

  if (value != alsa_get_elem_value(data->level_elem))
    alsa_set_elem_value(data->level_elem, value);
}

static void level_updated(struct alsa_elem *elem, void *private) {
  struct switch_dial *data = private;

  gtk_widget_set_sensitive(data->dial, alsa_get_elem_writable(elem));
  gtk_dial_set_value(GTK_DIAL(data->dial), alsa_get_elem_value(elem));
}

static void popup_hide(struct switch_dial *data) {
  gtk_widget_set_visible(data->popup, FALSE);
}

static void popup_show(struct switch_dial *data) {
  graphene_rect_t bounds;

  if (!gtk_widget_compute_bounds(data->arrow, data->overlay, &bounds))
    return;

  gtk_widget_set_halign(data->popup, GTK_ALIGN_START);
  gtk_widget_set_valign(data->popup, GTK_ALIGN_START);
  gtk_widget_set_margin_start(data->popup, bounds.origin.x);
  gtk_widget_set_margin_top(
    data->popup, bounds.origin.y + bounds.size.height
  );
  gtk_widget_set_visible(data->popup, TRUE);
}

// dismiss the popup on a click anywhere else in the window
static void overlay_clicked(
  GtkGestureClick *gesture,
  int              n_press,
  double           x,
  double           y,
  gpointer         user_data
) {
  struct switch_dial *data = user_data;

  if (!gtk_widget_get_visible(data->popup))
    return;

  graphene_point_t point = GRAPHENE_POINT_INIT(x, y);
  graphene_rect_t bounds;

  if (gtk_widget_compute_bounds(data->popup, data->overlay, &bounds) &&
      graphene_rect_contains_point(&bounds, &point))
    return;

  // let the arrow's own handler do the toggle
  if (gtk_widget_compute_bounds(data->arrow, data->overlay, &bounds) &&
      graphene_rect_contains_point(&bounds, &point))
    return;

  popup_hide(data);
}

static gboolean popup_key_pressed(
  GtkEventControllerKey *controller,
  guint                  keyval,
  guint                  keycode,
  GdkModifierType        state,
  gpointer               user_data
) {
  struct switch_dial *data = user_data;

  if (keyval != GDK_KEY_Escape)
    return FALSE;

  popup_hide(data);
  return TRUE;
}

static void arrow_clicked(GtkWidget *button, gpointer user_data) {
  struct switch_dial *data = user_data;

  // attach to the main window's overlay on first use (the widget is
  // not rooted at construction time)
  if (!data->overlay) {
    GtkWidget *overlay = gtk_widget_get_ancestor(button, GTK_TYPE_OVERLAY);

    if (!overlay)
      return;

    data->overlay = overlay;
    g_object_add_weak_pointer(
      G_OBJECT(overlay), (gpointer *)&data->overlay
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), data->popup);

    data->outside_click = GTK_EVENT_CONTROLLER(gtk_gesture_click_new());
    gtk_event_controller_set_propagation_phase(
      data->outside_click, GTK_PHASE_CAPTURE
    );
    g_signal_connect(
      data->outside_click, "pressed", G_CALLBACK(overlay_clicked), data
    );
    gtk_widget_add_controller(overlay, data->outside_click);
  }

  if (gtk_widget_get_visible(data->popup))
    popup_hide(data);
  else
    popup_show(data);
}

static void switch_dial_destroy(GtkWidget *widget, gpointer user_data) {
  struct switch_dial *data = user_data;

  alsa_elem_remove_callbacks_by_data(data->level_elem, data);

  // the overlay outlives us unless the whole window is being torn
  // down, in which case the weak pointer has been cleared
  if (data->overlay) {
    gtk_widget_remove_controller(data->overlay, data->outside_click);
    gtk_overlay_remove_overlay(GTK_OVERLAY(data->overlay), data->popup);
    g_object_remove_weak_pointer(
      G_OBJECT(data->overlay), (gpointer *)&data->overlay
    );
  }

  g_object_unref(data->popup);
  g_free(data);
}

GtkWidget *make_switch_dial_alsa_elem(
  struct alsa_elem *switch_elem,
  struct alsa_elem *level_elem,
  const char       *label
) {
  struct switch_dial *data = g_malloc0(sizeof(struct switch_dial));
  data->level_elem = level_elem;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(box, "switch-dial");

  GtkWidget *button = make_boolean_alsa_elem(switch_elem, label, NULL);
  gtk_widget_set_hexpand(button, TRUE);
  gtk_box_append(GTK_BOX(box), button);

  data->arrow = gtk_button_new_from_icon_name("pan-down-symbolic");
  gtk_widget_add_css_class(data->arrow, "switch-dial-arrow");
  gtk_box_append(GTK_BOX(box), data->arrow);

  data->dial = gtk_dial_new_with_range(
    level_elem->min_val, level_elem->max_val, 1, 10
  );
  gtk_dial_set_show_value(GTK_DIAL(data->dial), TRUE);
  gtk_widget_set_size_request(data->dial, 60, 60);

  data->popup = g_object_ref_sink(gtk_frame_new(NULL));
  gtk_widget_add_css_class(data->popup, "switch-dial-popup");
  gtk_frame_set_child(GTK_FRAME(data->popup), data->dial);
  gtk_widget_set_visible(data->popup, FALSE);

  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(
    key, "key-pressed", G_CALLBACK(popup_key_pressed), data
  );
  gtk_widget_add_controller(data->popup, key);

  g_signal_connect(
    data->arrow, "clicked", G_CALLBACK(arrow_clicked), data
  );
  g_signal_connect(
    data->dial, "value-changed", G_CALLBACK(dial_changed), data
  );
  g_signal_connect(
    data->arrow, "destroy", G_CALLBACK(switch_dial_destroy), data
  );

  level_updated(level_elem, data);
  alsa_elem_add_callback(level_elem, level_updated, data, NULL);

  widget_doc_register(box, switch_elem);

  return box;
}
