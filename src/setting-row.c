// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config-helpers.h"
#include "gtkhelper.h"
#include "setting-row.h"

GtkWidget *make_settings_grid(void) {
  GtkWidget *grid = gtk_grid_new();

  gtk_widget_set_margin(grid, 20);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 10);

  return grid;
}

GtkWidget *add_setting_row(
  GtkWidget  *grid,
  int        *grid_y,
  const char *label,
  GtkWidget  *control,
  const char *description
) {
  if (*grid_y) {
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 10);
    gtk_widget_set_margin_bottom(sep, 10);
    gtk_grid_attach(GTK_GRID(grid), sep, 0, (*grid_y)++, 2, 1);
  }

  /* Heading and control in one box in one row, rather than a row each:
   * a taller description would otherwise stretch the control's row and
   * leave the control floating in the middle of it.
   */
  GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_valign(label_box, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(label_box), config_bold_label(label));

  gtk_widget_set_halign(control, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(label_box), control);

  gtk_grid_attach(GTK_GRID(grid), label_box, 0, *grid_y, 1, 1);

  /* A box rather than the label alone, so that a caller can append its
   * own content below the description.
   */
  GtkWidget *desc_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_box_append(
    GTK_BOX(desc_box), config_wrapped_help_label(description, 70)
  );
  gtk_grid_attach(GTK_GRID(grid), desc_box, 1, *grid_y, 1, 1);

  (*grid_y)++;

  return desc_box;
}
