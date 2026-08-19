// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string.h>

#include "autogain-hold.h"

#define AUTOGAIN_PAIR_CHANNELS 2

struct alsa_elem *get_autogain_hold_elem(struct alsa_card *card, int ch) {
  if (card->driver_type != DRIVER_TYPE_SOCKET)
    return NULL;

  char name[64];

  snprintf(name, sizeof(name), "Line In %d Autogain Capture Switch", ch);
  return get_elem_by_name(card->elems, name);
}

struct autogain_hold {
  GPtrArray        *autogain_elems;  // watched, not owned
  struct alsa_elem *elem;
  GtkWidget        *widget;
};

static void autogain_hold_updated(struct alsa_elem *elem, void *private) {
  struct autogain_hold *hold = private;
  int held = 0;

  for (int i = 0; i < hold->autogain_elems->len; i++)
    if (alsa_get_elem_value(g_ptr_array_index(hold->autogain_elems, i)))
      held = 1;

  gtk_widget_set_sensitive(hold->widget, !held);
}

static void autogain_hold_destroy(GtkWidget *widget, gpointer user_data) {
  struct autogain_hold *hold = user_data;

  for (int i = 0; i < hold->autogain_elems->len; i++)
    alsa_elem_remove_callbacks_by_data(
      g_ptr_array_index(hold->autogain_elems, i), hold
    );
  alsa_elem_remove_callbacks_by_data(hold->elem, hold);
  g_ptr_array_free(hold->autogain_elems, TRUE);
  g_free(hold);
}

// Hold widget while any of autogain_elems is set. Takes ownership of
// the array.
static void add_autogain_hold(
  GtkWidget        *widget,
  struct alsa_elem *elem,
  GPtrArray        *autogain_elems
) {
  if (!autogain_elems->len) {
    g_ptr_array_free(autogain_elems, TRUE);
    return;
  }

  struct autogain_hold *hold = g_malloc0(sizeof(*hold));

  hold->autogain_elems = autogain_elems;
  hold->elem = elem;
  hold->widget = widget;

  for (int i = 0; i < autogain_elems->len; i++)
    alsa_elem_add_callback(
      g_ptr_array_index(autogain_elems, i), autogain_hold_updated, hold, NULL
    );

  // also registered on the control's own element, after the widget's
  // own callback, so the hold wins when the widget re-asserts
  // sensitivity from writability
  alsa_elem_add_callback(elem, autogain_hold_updated, hold, NULL);
  g_signal_connect(widget, "destroy", G_CALLBACK(autogain_hold_destroy), hold);

  autogain_hold_updated(elem, hold);
}

void autogain_hold_pair_control(
  GtkWidget        *widget,
  struct alsa_card *card,
  int               ch,
  struct alsa_elem *elem
) {
  GPtrArray *autogain_elems = g_ptr_array_new();

  for (int i = 0; i < AUTOGAIN_PAIR_CHANNELS; i++) {
    struct alsa_elem *autogain_elem = get_autogain_hold_elem(card, ch + i);

    if (autogain_elem)
      g_ptr_array_add(autogain_elems, autogain_elem);
  }

  add_autogain_hold(widget, elem, autogain_elems);
}

void autogain_hold_gui_control(
  GtkWidget        *widget,
  struct alsa_card *card,
  struct alsa_elem *elem
) {
  GPtrArray *autogain_elems = g_ptr_array_new();

  for (int i = 0; i < card->elems->len; i++) {
    struct alsa_elem *autogain_elem = g_ptr_array_index(card->elems, i);

    if (autogain_elem->card &&
        strstr(autogain_elem->name, "Autogain Capture Switch"))
      g_ptr_array_add(autogain_elems, autogain_elem);
  }

  add_autogain_hold(widget, elem, autogain_elems);
}
