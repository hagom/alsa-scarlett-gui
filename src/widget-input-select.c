// SPDX-FileCopyrightText: 2023-2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "stringhelper.h"
#include "widget-input-select.h"
#include "window-geometry.h"

struct input_select {
  struct alsa_elem *elem;
  struct alsa_elem *hw_link_elem;   // device link: gangs the selection
  struct alsa_elem *gui_link_elem;  // GUI link: pair collapsed to one button
  GtkWidget        *button;
  int               line_num;
};

static void input_select_clicked(
  GtkWidget           *widget,
  struct input_select *data
) {
  int count = alsa_get_item_count(data->elem);

  // select the item that matches the line number that was clicked on
  for (int i = 0; i < count; i++) {
    const char *text = alsa_get_item_name(data->elem, i);
    int a, b;
    get_two_num_from_string(text, &a, &b);

    if ((b == -1 && a == data->line_num) ||
        (a <= data->line_num && b >= data->line_num)) {
      alsa_set_elem_value(data->elem, i);
      break;
    }
  }
}

// whether the current selection covers the given line number
static int selection_covers_line(struct alsa_elem *elem, int line_num) {
  int value = alsa_get_elem_value(elem);
  const char *text = alsa_get_item_name(elem, value);

  int a, b;
  get_two_num_from_string(text, &a, &b);

  return b == -1
           ? a == line_num
           : a <= line_num && b >= line_num;
}

static void input_select_updated(
  struct alsa_elem *elem,
  void             *private
) {
  struct input_select *data = private;
  int line_num = data->line_num;
  int is_writable = alsa_get_elem_writable(data->elem);

  int active = selection_covers_line(data->elem, line_num);
  int hw_linked =
    data->hw_link_elem && alsa_get_elem_value(data->hw_link_elem);
  int gui_linked =
    data->gui_link_elem && alsa_get_elem_value(data->gui_link_elem);

  // when the device gangs the pair, both channels light up whichever
  // of the two the underlying selection points at
  if (!active && hw_linked) {
    int partner = (line_num % 2) ? line_num + 1 : line_num - 1;

    active = selection_covers_line(data->elem, partner);
  }

  // the left button of a collapsed GUI-linked pair shows the pair range
  if (line_num % 2) {
    char s[30];

    if (gui_linked)
      snprintf(s, sizeof(s), "%d–%d", line_num, line_num + 1);
    else
      snprintf(s, sizeof(s), "%d", line_num);
    gtk_button_set_label(GTK_BUTTON(data->button), s);
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->button), active);
  gtk_widget_set_sensitive(data->button, !active && is_writable);
}

GtkWidget *make_input_select_alsa_elem(
  struct alsa_elem *elem,
  struct alsa_elem *hw_link_elem,
  struct alsa_elem *gui_link_elem,
  int               line_num
) {
  struct input_select *data = malloc(sizeof(struct input_select));
  data->elem = elem;
  data->hw_link_elem = hw_link_elem;
  data->gui_link_elem = gui_link_elem;
  data->button = gtk_toggle_button_new();
  data->line_num = line_num;

  gtk_widget_add_css_class(data->button, "input-select");

  char s[20];
  snprintf(s, 20, "%d", line_num);
  gtk_button_set_label(GTK_BUTTON(data->button), s);

  g_signal_connect(
    data->button, "clicked", G_CALLBACK(input_select_clicked), data
  );
  alsa_elem_add_callback(elem, input_select_updated, data, NULL);
  if (hw_link_elem)
    alsa_elem_add_callback(hw_link_elem, input_select_updated, data, NULL);
  if (gui_link_elem)
    alsa_elem_add_callback(gui_link_elem, input_select_updated, data, NULL);

  input_select_updated(elem, data);

  widget_doc_register(data->button, elem);

  return data->button;
}
