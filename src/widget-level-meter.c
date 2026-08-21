// SPDX-FileCopyrightText: 2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <math.h>

#include "glow.h"
#include "peak-tracker.h"
#include "widget-level-meter.h"

#define BAR_WIDTH   14

// corner radius of the bar and of its background trough
#define BAR_RADIUS  4
#define NAT_HEIGHT  180

// tick marks beside the scale labels, and the gap between the two
#define TICK_LENGTH 3
#define LABEL_GAP   2

// the smallest vertical gap left between two scale labels
#define LABEL_MIN_GAP 2

// spacing of the diagonal hatching drawn on a meter with no level
#define HATCH_SPACING 5

// scale label font size relative to the widget's font
#define LABEL_FONT_SCALE 0.7

#define DB_MIN -60.0
#define DB_MAX 0.0

/* The scale is linear down to -30 dB. Below that each labelled
 * interval gets the same space regardless of how many dB it spans, at
 * two thirds of the space a 6 dB interval gets above -30 dB. That
 * leaves seven intervals of 3/21 above and three of 2/21 below.
 *
 * Each entry is a level and its position on the scale, 0 at the bottom
 * and 1 at the top.
 */
static const double scale_positions[][2] = {
  { -60, 0.0      },
  { -48, 2.0 / 21 },
  { -36, 4.0 / 21 },
  { -30, 6.0 / 21 },
  {   0, 1.0      }
};

// the levels labelled on the scale, top to bottom
static const double label_levels[] = {
  0, -6, -12, -18, -24, -30, -36, -48, -60
};

// The peak marker overhangs the bar slightly, so the snapshot is taken
// over a rect this much larger on every side.
#define BLEED 1

struct _GtkLevelMeter {
  GtkWidget parent_instance;

  double level;

  // a scale draws the dB labels and no bar
  gboolean is_scale;

  // the level can't be determined, so the meter is hatched
  gboolean unknown;

  struct peak_tracker peak;

  PangoLayout *layout;
};

G_DEFINE_TYPE(GtkLevelMeter, gtk_level_meter, GTK_TYPE_WIDGET)

static double db_to_pos(double db) {
  int count = G_N_ELEMENTS(scale_positions);

  if (db <= scale_positions[0][0])
    return scale_positions[0][1];

  for (int i = 0; i < count - 1; i++) {
    double from = scale_positions[i][0];
    double to = scale_positions[i + 1][0];

    if (db > to)
      continue;

    double frac = (db - from) / (to - from);

    return scale_positions[i][1] +
           frac * (scale_positions[i + 1][1] - scale_positions[i][1]);
  }

  return scale_positions[count - 1][1];
}

// dB to y coordinate; the inset at the top and bottom leaves room for
// half a label at each end of the scale
static double db_to_y(double db, double inset, double usable) {
  return inset + (1.0 - db_to_pos(db)) * usable;
}

// scale labels, with a proper minus sign on the negative ones
static void format_db_label(char *s, int size, double db) {
  snprintf(s, size, "%s%.0f", db < 0 ? "−" : "", fabs(db));
}

// the label font is needed for the scale labels and, in both modes, for
// the vertical inset that keeps the topmost and bottommost labels from
// being clipped
static PangoLayout *get_layout(GtkLevelMeter *meter) {
  if (meter->layout)
    return meter->layout;

  PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(meter));
  PangoFontDescription *desc = pango_font_description_copy(
    pango_context_get_font_description(context)
  );

  pango_font_description_set_size(
    desc, pango_font_description_get_size(desc) * LABEL_FONT_SCALE
  );

  meter->layout = pango_layout_new(context);
  pango_layout_set_font_description(meter->layout, desc);
  pango_font_description_free(desc);

  return meter->layout;
}

static void get_label_size(GtkLevelMeter *meter, int *width, int *height) {
  PangoLayout *layout = get_layout(meter);

  // the widest label is the one furthest from zero
  char s[20];

  format_db_label(s, sizeof(s), DB_MIN);
  pango_layout_set_text(layout, s, -1);

  pango_layout_get_pixel_size(layout, width, height);
}

// The closest two labels come together on the scale, as a fraction of
// the scale's height. The meter is never shorter than the height at
// which that gap fits a label, so every label is always drawn.
static double min_label_gap_frac(void) {
  double min = 1.0;

  for (int i = 1; i < G_N_ELEMENTS(label_levels); i++) {
    double gap =
      db_to_pos(label_levels[i - 1]) - db_to_pos(label_levels[i]);

    if (gap < min)
      min = gap;
  }

  return min;
}

static void rounded_rect(
  cairo_t *cr,
  double   x,
  double   y,
  double   w,
  double   h,
  double   r
) {
  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;

  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0,         M_PI / 2);
  cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2,  M_PI);
  cairo_arc(cr, x + r,     y + r,     r, M_PI,      3 * M_PI / 2);
  cairo_close_path(cr);
}

// The colour gradient running up the bar, fading between the band
// colours as the dials do, so a level is the same colour on both.
static cairo_pattern_t *make_gradient(double y_bottom, double y_top) {
  cairo_pattern_t *grad = cairo_pattern_create_linear(0, y_bottom, 0, y_top);

  // the ends of the visible range, plus every breakpoint between them
  double r, g, b;

  level_band_colour(DB_MIN, &r, &g, &b);
  cairo_pattern_add_color_stop_rgb(grad, 0, r, g, b);

  for (int i = 0; i < LEVEL_BAND_COUNT; i++) {
    double db = level_band_breakpoints[i];

    if (db <= DB_MIN || db >= DB_MAX)
      continue;

    const double *colours = &level_band_colours[i * 3];

    cairo_pattern_add_color_stop_rgb(
      grad, db_to_pos(db), colours[0], colours[1], colours[2]
    );
  }

  level_band_colour(DB_MAX, &r, &g, &b);
  cairo_pattern_add_color_stop_rgb(grad, 1, r, g, b);

  return grad;
}

static void draw_scale(
  GtkLevelMeter *meter,
  cairo_t       *cr,
  int            width,
  double         inset,
  double         usable
) {
  PangoLayout *layout = get_layout(meter);

  cairo_set_source_rgba(cr, 1, 1, 1, 0.55);

  for (int i = 0; i < G_N_ELEMENTS(label_levels); i++) {
    double y = db_to_y(label_levels[i], inset, usable);

    char s[20];

    format_db_label(s, sizeof(s), label_levels[i]);
    pango_layout_set_text(layout, s, -1);

    int w, h;

    pango_layout_get_pixel_size(layout, &w, &h);

    cairo_move_to(cr, width - TICK_LENGTH - LABEL_GAP - w, y - h / 2.0);
    pango_cairo_show_layout(cr, layout);

    cairo_rectangle(cr, width - TICK_LENGTH, y - 0.5, TICK_LENGTH, 1);
    cairo_fill(cr);
  }
}

// the coloured bar, from the bottom of the scale up to the level
static void draw_level(
  GtkLevelMeter *meter,
  cairo_t       *cr,
  double         x,
  double         y_bottom,
  double         inset,
  double         usable
) {
  double y_level = db_to_y(meter->level, inset, usable);
  cairo_pattern_t *grad = make_gradient(
    y_bottom, db_to_y(DB_MAX, inset, usable)
  );

  cairo_save(cr);
  rounded_rect(cr, x, y_level, BAR_WIDTH, y_bottom - y_level, BAR_RADIUS);
  cairo_clip(cr);
  cairo_set_source(cr, grad);
  cairo_paint(cr);
  cairo_restore(cr);

  cairo_pattern_destroy(grad);
}

// the peak-hold marker, which stays put while the peak is held even
// once the signal itself has gone
static void draw_peak(
  GtkLevelMeter *meter,
  cairo_t       *cr,
  double         x,
  double         inset,
  double         usable
) {
  double y = db_to_y(meter->peak.peak, inset, usable);
  double r, g, b;

  level_band_colour(MIN(meter->peak.peak, DB_MAX), &r, &g, &b);

  cairo_set_source_rgba(cr, r, g, b, 0.4);
  cairo_rectangle(cr, x - 1, y - 2, BAR_WIDTH + 2, 4);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, r, g, b);
  cairo_rectangle(cr, x, y - 1, BAR_WIDTH, 2);
  cairo_fill(cr);
}

// diagonal hatching filling the trough of a meter with no level to show
static void draw_hatching(
  cairo_t *cr,
  double   x,
  double   y_top,
  double   y_bottom
) {
  cairo_save(cr);
  rounded_rect(cr, x, y_top, BAR_WIDTH, y_bottom - y_top, BAR_RADIUS);
  cairo_clip(cr);

  cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
  cairo_set_line_width(cr, 1);

  for (double y = y_top + HATCH_SPACING;
       y < y_bottom + BAR_WIDTH;
       y += HATCH_SPACING) {
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + BAR_WIDTH, y - BAR_WIDTH);
  }

  cairo_stroke(cr);
  cairo_restore(cr);
}

static void draw_bar(
  GtkLevelMeter *meter,
  cairo_t       *cr,
  int            width,
  double         inset,
  double         usable
) {
  double x = (width - BAR_WIDTH) / 2.0;
  double y_top = db_to_y(DB_MAX, inset, usable);
  double y_bottom = db_to_y(DB_MIN, inset, usable);

  // trough, spanning the scale and no more; the label inset above and
  // below it is not part of the scale
  cairo_set_source_rgb(cr, 0.11, 0.11, 0.11);
  rounded_rect(cr, x, y_top, BAR_WIDTH, y_bottom - y_top, BAR_RADIUS);
  cairo_fill(cr);

  if (meter->unknown) {
    draw_hatching(cr, x, y_top, y_bottom);
    return;
  }

  // tick marks lining up with the scale labels
  cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
  for (int i = 0; i < G_N_ELEMENTS(label_levels); i++) {
    double y = db_to_y(label_levels[i], inset, usable);

    cairo_rectangle(cr, x + 2, y - 0.5, BAR_WIDTH - 4, 1);
    cairo_fill(cr);
  }

  if (meter->level > DB_MIN)
    draw_level(meter, cr, x, y_bottom, inset, usable);

  if (meter->peak.hold_ms && meter->peak.peak > DB_MIN)
    draw_peak(meter, cr, x, inset, usable);
}

static void level_meter_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  GtkLevelMeter *meter = GTK_LEVEL_METER(widget);

  int width = gtk_widget_get_width(widget);
  int height = gtk_widget_get_height(widget);

  if (width <= 0 || height <= 0)
    return;

  int label_w, label_h;

  get_label_size(meter, &label_w, &label_h);

  double inset = label_h / 2.0;
  double usable = height - label_h;

  if (usable <= 0) {
    inset = 0;
    usable = height;
  }

  cairo_t *cr = gtk_snapshot_append_cairo(
    snapshot,
    &GRAPHENE_RECT_INIT(
      -BLEED, -BLEED,
      width + BLEED * 2, height + BLEED * 2
    )
  );

  if (meter->is_scale)
    draw_scale(meter, cr, width, inset, usable);
  else
    draw_bar(meter, cr, width, inset, usable);

  cairo_destroy(cr);
}

static void level_meter_measure(
  GtkWidget      *widget,
  GtkOrientation  orientation,
  int             for_size,
  int            *minimum,
  int            *natural,
  int            *minimum_baseline,
  int            *natural_baseline
) {
  GtkLevelMeter *meter = GTK_LEVEL_METER(widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    if (meter->is_scale) {
      int label_w, label_h;

      get_label_size(meter, &label_w, &label_h);
      *minimum = *natural = label_w + TICK_LENGTH + LABEL_GAP;
    } else {
      *minimum = *natural = BAR_WIDTH;
    }
  } else {
    int label_w, label_h;

    get_label_size(meter, &label_w, &label_h);

    // the scale needs the height at which the closest pair of labels
    // still fits, plus half a label of inset at each end
    *minimum = (int)ceil(
      (label_h + LABEL_MIN_GAP) / min_label_gap_frac()
    ) + label_h;
    *natural = MAX(NAT_HEIGHT, *minimum);
  }

  *minimum_baseline = -1;
  *natural_baseline = -1;
}

static void level_meter_css_changed(
  GtkWidget         *widget,
  GtkCssStyleChange *change
) {
  GtkLevelMeter *meter = GTK_LEVEL_METER(widget);

  g_clear_object(&meter->layout);

  GTK_WIDGET_CLASS(gtk_level_meter_parent_class)->css_changed(widget, change);
}

static void level_meter_dispose(GObject *object) {
  GtkLevelMeter *meter = GTK_LEVEL_METER(object);

  g_clear_object(&meter->layout);

  G_OBJECT_CLASS(gtk_level_meter_parent_class)->dispose(object);
}

static void gtk_level_meter_class_init(GtkLevelMeterClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  G_OBJECT_CLASS(klass)->dispose = level_meter_dispose;

  widget_class->snapshot = level_meter_snapshot;
  widget_class->measure = level_meter_measure;
  widget_class->css_changed = level_meter_css_changed;

  gtk_widget_class_set_css_name(widget_class, "levelmeter");
}

static void gtk_level_meter_init(GtkLevelMeter *meter) {
  meter->level = -INFINITY;

  peak_tracker_init(&meter->peak);
}

GtkWidget *gtk_level_meter_new(void) {
  return g_object_new(GTK_TYPE_LEVEL_METER, NULL);
}

GtkWidget *gtk_level_meter_scale_new(void) {
  GtkWidget *widget = gtk_level_meter_new();

  GTK_LEVEL_METER(widget)->is_scale = TRUE;

  return widget;
}

void gtk_level_meter_set_peak_hold(GtkLevelMeter *meter, int peak_hold) {
  meter->peak.hold_ms = peak_hold;
}

void gtk_level_meter_set_unknown(GtkLevelMeter *meter, gboolean unknown) {
  if (meter->unknown == unknown)
    return;

  meter->unknown = unknown;

  // drop the held peak; it is not this port's any more
  int hold_ms = meter->peak.hold_ms;

  peak_tracker_init(&meter->peak);
  meter->peak.hold_ms = hold_ms;

  gtk_widget_queue_draw(GTK_WIDGET(meter));
}

void gtk_level_meter_set_level(GtkLevelMeter *meter, double level_db) {
  if (isnan(level_db))
    return;

  double old_peak = meter->peak.peak;

  if (meter->peak.hold_ms)
    peak_tracker_add(&meter->peak, level_db);

  if (meter->level == level_db && meter->peak.peak == old_peak)
    return;

  meter->level = level_db;

  gtk_widget_queue_draw(GTK_WIDGET(meter));
}

double gtk_level_meter_get_level(GtkLevelMeter *meter) {
  return meter->level;
}
