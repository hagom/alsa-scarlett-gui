// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <gtk/gtk.h>
#include <math.h>
#include <string.h>

#include "config-helpers.h"
#include "custom-names.h"
#include "stringhelper.h"
#include "window-autogain.h"
#include "window-helper.h"

// how a row's state is presented: the colour given to its message, or
// none for a run that produced no result
enum autogain_severity {
  AUTOGAIN_NEUTRAL,
  AUTOGAIN_LISTENING,
  AUTOGAIN_SUCCESS,
  AUTOGAIN_WARNING,
  AUTOGAIN_ERROR
};

/* The exit status item list differs between the scarlett2 4th Gen,
 * Vocaster, and FCP devices, so the outcome is looked up by item name.
 * A name that isn't listed here is displayed as-is, uncoloured.
 */
static const struct {
  const char            *item;
  enum autogain_severity severity;
  const char            *text;
} autogain_statuses[] = {
  { "Success",          AUTOGAIN_SUCCESS, "Gain set" },
  { "SuccessDRover",    AUTOGAIN_WARNING,
    "Gain set, but the signal’s dynamic range was wider than the target" },
  { "DRover",           AUTOGAIN_WARNING,
    "Gain set, but the signal’s dynamic range was wider than the target" },
  { "WarnMinGainLimit", AUTOGAIN_WARNING,
    "Signal too loud; gain set to the minimum" },
  { "MinGainLimit",     AUTOGAIN_WARNING,
    "Signal too loud; gain set to the minimum" },
  { "WarnMinCap",       AUTOGAIN_WARNING,
    "Signal too loud; gain set to the minimum" },
  { "WarnMaxCap",       AUTOGAIN_WARNING,
    "Signal too quiet; gain set to the maximum" },
  { "FailMaxGainLimit", AUTOGAIN_ERROR,
    "Signal too quiet; the maximum gain wasn’t enough" },
  { "MaxGainLimit",     AUTOGAIN_ERROR,
    "Signal too quiet; the maximum gain wasn’t enough" },
  { "FailDRunder",      AUTOGAIN_ERROR,
    "The signal’s dynamic range was too small" },
  { "DRunder",          AUTOGAIN_ERROR,
    "The signal’s dynamic range was too small" },
  { "FailClipped",      AUTOGAIN_ERROR, "The signal clipped" },
  { "Clipped",          AUTOGAIN_ERROR, "The signal clipped" },
  { "FailPG",           AUTOGAIN_ERROR, "Couldn’t set a usable level" },
  { "FailRange",        AUTOGAIN_ERROR, "Couldn’t set a usable level" },
  { "Failed",           AUTOGAIN_ERROR, "Couldn’t set a usable level" },
  { "Cancelled",        AUTOGAIN_NEUTRAL, "Cancelled" },
  { "Invalid",          AUTOGAIN_NEUTRAL, "No result" },
  { "Root",             AUTOGAIN_NEUTRAL, "No result" },
  { "Unknown",          AUTOGAIN_NEUTRAL, "No result" },
  { "Stopped",          AUTOGAIN_NEUTRAL, "No result" },
};

// how often the channels are re-read while a run is in progress
#define AUTOGAIN_POLL_MS 200

static const char *autogain_severity_css[] = {
  NULL, "autogain-listening", "autogain-success", "autogain-warning",
  "autogain-error"
};

struct autogain_window;

// one input channel's progress and outcome
struct autogain_row {
  struct autogain_window *w;
  struct alsa_elem       *switch_elem;
  struct alsa_elem       *status_elem;
  struct alsa_elem       *gain_elem;
  struct routing_src     *src;         // for the channel's display name
  int                     lr_num;
  GtkWidget              *box;
  GtkWidget              *name_label;
  GtkWidget              *spinner;
  GtkWidget              *status_label;
  GtkWidget              *gain_label;
  int                     running;
};

struct autogain_window {
  struct alsa_card *card;
  GtkWidget        *window;
  GtkWidget        *instruction;
  GtkWidget        *hint;
  GtkWidget        *cancel_button;
  GtkWidget        *ok_button;
  GPtrArray        *rows;   // struct autogain_row *, owned
  guint             poll_timer;
};

// The card's element named by the channel prefix and a control name
static struct alsa_elem *get_prefixed_elem(
  GPtrArray  *elems,
  const char *prefix,
  const char *control
) {
  char *name = g_strconcat(prefix, control, NULL);
  struct alsa_elem *elem = get_elem_by_name(elems, name);

  g_free(name);
  return elem;
}

// The status element's current item name, or NULL if it has none
static char *get_status_item(struct alsa_elem *status_elem) {
  if (!status_elem)
    return NULL;

  long value = alsa_get_elem_value(status_elem);
  if (value < 0 || value >= alsa_get_item_count(status_elem))
    return NULL;

  return alsa_get_item_name(status_elem, value);
}

/* The switch is set while the channel's run is in progress and the
 * device clears it when the run ends, so it says what is running. The
 * device sets it on every channel it has ganged into the run, not just
 * the one the run was asked for.
 */
static int row_is_running(struct autogain_row *row) {
  return alsa_get_elem_value(row->switch_elem) != 0;
}

// Look up the outcome to display, defaulting to the raw item name
static void get_row_outcome(
  struct autogain_row     *row,
  char                   **text,
  enum autogain_severity  *severity
) {
  *text = NULL;
  *severity = AUTOGAIN_NEUTRAL;

  char *item = get_status_item(row->status_elem);
  if (!item)
    return;

  for (int i = 0; i < (int)G_N_ELEMENTS(autogain_statuses); i++) {
    if (strcmp(item, autogain_statuses[i].item) != 0)
      continue;

    *text = g_strdup(autogain_statuses[i].text);
    *severity = autogain_statuses[i].severity;
    g_free(item);
    return;
  }

  *text = item;
}

/* The gain the run settled on, which is worth more to the reader than
 * the outcome on its own. A run that failed has had the gain put back
 * to what it was, so there is nothing of its own to report.
 */
static void set_row_gain(
  struct autogain_row    *row,
  enum autogain_severity  severity
) {
  int settled = severity == AUTOGAIN_SUCCESS || severity == AUTOGAIN_WARNING;

  gtk_widget_set_visible(row->gain_label, settled && row->gain_elem);
  if (!settled || !row->gain_elem)
    return;

  // input gains step in whole dB; formatted as the gain widgets do
  int db = round(alsa_get_elem_dB(
    row->gain_elem, alsa_get_elem_value(row->gain_elem)
  ));

  char *text = g_strdup_printf(
    "%s%ddB", db < 0 ? "−" : db > 0 ? "+" : "", abs(db)
  );

  gtk_label_set_text(GTK_LABEL(row->gain_label), text);
  g_free(text);
}

static void set_row_status(
  struct autogain_row    *row,
  const char             *text,
  enum autogain_severity  severity,
  int                     spinning
) {
  gtk_label_set_text(GTK_LABEL(row->status_label), text);

  for (int i = 0; i < (int)G_N_ELEMENTS(autogain_severity_css); i++)
    if (autogain_severity_css[i])
      gtk_widget_remove_css_class(row->status_label, autogain_severity_css[i]);

  if (autogain_severity_css[severity])
    gtk_widget_add_css_class(row->status_label, autogain_severity_css[severity]);

  gtk_widget_set_visible(row->spinner, spinning);
  gtk_spinner_set_spinning(GTK_SPINNER(row->spinner), spinning);
}

static void set_row_result(struct autogain_row *row) {
  char *text;
  enum autogain_severity severity;

  get_row_outcome(row, &text, &severity);
  set_row_status(row, text ? text : "", severity, FALSE);
  set_row_gain(row, severity);
  g_free(text);
}

// Show the "what to check" hint if any channel's run ended badly
static void update_autogain_hint(struct autogain_window *w) {
  int failed = 0;

  for (int i = 0; i < w->rows->len; i++) {
    struct autogain_row *row = g_ptr_array_index(w->rows, i);

    if (!gtk_widget_get_visible(row->box) || row->running)
      continue;

    char *text;
    enum autogain_severity severity;
    get_row_outcome(row, &text, &severity);
    g_free(text);

    if (severity == AUTOGAIN_ERROR)
      failed = 1;
  }

  gtk_widget_set_visible(w->hint, failed);
}

static int any_row_running(struct autogain_window *w) {
  for (int i = 0; i < w->rows->len; i++) {
    struct autogain_row *row = g_ptr_array_index(w->rows, i);

    if (row->running)
      return 1;
  }

  return 0;
}

// Clear the previous run's rows so only the new run is listed
static void reset_autogain_rows(struct autogain_window *w) {
  for (int i = 0; i < w->rows->len; i++) {
    struct autogain_row *row = g_ptr_array_index(w->rows, i);

    gtk_widget_set_visible(row->box, FALSE);
  }

  gtk_widget_set_visible(w->hint, FALSE);
}

/* Bring every row into line with the device. A run is started on the
 * channel that was asked for, but the device decides which channels it
 * covers, so all of them are re-read rather than just the one whose
 * control changed. A poll repeats this while any channel is running,
 * so that channels the device brought into the run appear as they
 * start rather than when they finish.
 */
static void update_autogain_rows(struct autogain_window *w) {
  int starting = 0;
  for (int i = 0; i < w->rows->len; i++) {
    struct autogain_row *row = g_ptr_array_index(w->rows, i);

    if (row_is_running(row) && !row->running)
      starting = 1;
  }

  // a run beginning with nothing else in progress starts a new listing
  if (starting && !any_row_running(w))
    reset_autogain_rows(w);

  for (int i = 0; i < w->rows->len; i++) {
    struct autogain_row *row = g_ptr_array_index(w->rows, i);
    int running = row_is_running(row);

    if (running == row->running)
      continue;

    row->running = running;

    if (running) {
      set_row_status(row, "Listening…", AUTOGAIN_LISTENING, TRUE);
      set_row_gain(row, AUTOGAIN_NEUTRAL);
    } else {
      set_row_result(row);
    }

    gtk_widget_set_visible(row->box, TRUE);
  }

  int running = any_row_running(w);

  update_autogain_hint(w);

  // Cancel while a run is in progress, OK once none is
  gtk_widget_set_visible(w->cancel_button, running);
  gtk_widget_set_visible(w->ok_button, !running);

  // there is nothing to perform for once the run is over
  if (running)
    gtk_widget_remove_css_class(w->instruction, "autogain-idle");
  else
    gtk_widget_add_css_class(w->instruction, "autogain-idle");

  if (starting && !gtk_widget_get_visible(w->window))
    gtk_window_present(GTK_WINDOW(w->window));
}

static gboolean autogain_poll(void *user_data) {
  struct autogain_window *w = user_data;

  update_autogain_rows(w);

  if (any_row_running(w))
    return G_SOURCE_CONTINUE;

  w->poll_timer = 0;
  return G_SOURCE_REMOVE;
}

static void autogain_switch_updated(struct alsa_elem *elem, void *private) {
  struct autogain_window *w = private;

  update_autogain_rows(w);

  if (any_row_running(w) && !w->poll_timer)
    w->poll_timer = g_timeout_add(AUTOGAIN_POLL_MS, autogain_poll, w);
}

/* The device clears the switch and sets the exit status together, but
 * their ALSA controls are updated one after the other, so the status
 * can arrive after the switch has already ended the run. The gain
 * arrives on a notification of its own, later again. Either landing
 * late re-reads the outcome of a run already reported.
 */
static void autogain_result_updated(struct alsa_elem *elem, void *private) {
  struct autogain_row *row = private;

  if (row->running || !gtk_widget_get_visible(row->box))
    return;

  set_row_result(row);
  update_autogain_hint(row->w);
}

// Keep the results in step with a channel's name being changed
static void autogain_name_updated(struct alsa_elem *elem, void *private) {
  struct autogain_row *row = private;

  char *name = row->src
    ? get_src_display_name_formatted(row->src)
    : g_strdup_printf("Input %d", row->lr_num);

  char *markup = g_markup_printf_escaped("<b>%s</b>", name);
  gtk_label_set_markup(GTK_LABEL(row->name_label), markup);
  g_free(markup);
  g_free(name);
}

static struct autogain_row *create_autogain_row(
  struct autogain_window *w,
  struct alsa_elem       *switch_elem,
  GtkSizeGroup           *name_size_group
) {
  struct autogain_row *row = g_malloc0(sizeof(struct autogain_row));

  row->w = w;
  row->switch_elem = switch_elem;
  row->lr_num = get_num_from_string(switch_elem->name);
  row->src = get_analogue_input_src(w->card, row->lr_num);

  /* "<prefix> Autogain Capture Switch" names the channel's status and
   * gain controls too
   */
  char *prefix = g_strndup(
    switch_elem->name, strstr(switch_elem->name, "Autogain") - switch_elem->name
  );
  row->status_elem = get_prefixed_elem(
    w->card->elems, prefix, "Autogain Status Capture Enum"
  );
  row->gain_elem = get_prefixed_elem(
    w->card->elems, prefix, "Gain Capture Volume"
  );
  g_free(prefix);

  row->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

  row->name_label = gtk_label_new(NULL);
  gtk_widget_set_halign(row->name_label, GTK_ALIGN_START);
  gtk_size_group_add_widget(name_size_group, row->name_label);
  gtk_box_append(GTK_BOX(row->box), row->name_label);

  row->spinner = gtk_spinner_new();
  gtk_box_append(GTK_BOX(row->box), row->spinner);

  row->status_label = gtk_label_new("");
  gtk_widget_set_halign(row->status_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(row->box), row->status_label);

  row->gain_label = gtk_label_new("");
  gtk_widget_add_css_class(row->gain_label, "autogain-gain");
  gtk_widget_set_halign(row->gain_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand(row->gain_label, TRUE);
  gtk_widget_set_visible(row->gain_label, FALSE);
  gtk_box_append(GTK_BOX(row->box), row->gain_label);

  // a row is listed only once its channel has been through a run
  gtk_widget_set_visible(row->box, FALSE);

  autogain_name_updated(NULL, row);
  row->running = row_is_running(row);

  alsa_elem_add_callback(switch_elem, autogain_switch_updated, w, NULL);

  if (row->status_elem)
    alsa_elem_add_callback(
      row->status_elem, autogain_result_updated, row, NULL
    );
  if (row->gain_elem)
    alsa_elem_add_callback(
      row->gain_elem, autogain_result_updated, row, NULL
    );

  if (row->src && row->src->custom_name_elem)
    alsa_elem_add_callback(
      row->src->custom_name_elem, autogain_name_updated, row, NULL
    );

  return row;
}

// Putting the window away drops the results it was showing; it comes
// back when a run starts
static void hide_autogain_window(struct autogain_window *w) {
  reset_autogain_rows(w);
  gtk_widget_set_visible(w->window, FALSE);
}

static gboolean window_autogain_close_request(GtkWindow *window, gpointer data) {
  struct alsa_card *card = data;

  hide_autogain_window(card->autogain_data);

  return TRUE;
}

static void autogain_ok_clicked(GtkButton *button, gpointer data) {
  hide_autogain_window(data);
}

/* Clearing the switch on each running channel stops the run, which the
 * device answers by putting the gain back to what it was beforehand.
 * There is no result worth waiting around for, so the window goes too.
 */
static void autogain_cancel_clicked(GtkButton *button, gpointer data) {
  struct autogain_window *w = data;

  for (int i = 0; i < w->rows->len; i++) {
    struct autogain_row *row = g_ptr_array_index(w->rows, i);

    if (row->running)
      alsa_set_elem_value(row->switch_elem, 0);
  }

  hide_autogain_window(w);
}

static void create_autogain_buttons(struct autogain_window *w, GtkBox *content) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign(box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(box, 5);

  w->cancel_button = gtk_button_new_with_label("Cancel");
  g_signal_connect(
    w->cancel_button, "clicked", G_CALLBACK(autogain_cancel_clicked), w
  );
  gtk_widget_set_visible(w->cancel_button, FALSE);
  gtk_box_append(GTK_BOX(box), w->cancel_button);

  w->ok_button = gtk_button_new_with_label("OK");
  g_signal_connect(
    w->ok_button, "clicked", G_CALLBACK(autogain_ok_clicked), w
  );
  gtk_box_append(GTK_BOX(box), w->ok_button);

  gtk_box_append(content, box);
  gtk_window_set_default_widget(GTK_WINDOW(w->window), w->ok_button);
}

void create_autogain_window(struct alsa_card *card) {
  GPtrArray *switch_elems = g_ptr_array_new();

  for (int i = 0; i < card->elems->len; i++) {
    struct alsa_elem *elem = g_ptr_array_index(card->elems, i);

    if (!elem->card)
      continue;
    if (!strstr(elem->name, "Autogain Capture Switch"))
      continue;
    if (get_num_from_string(elem->name) < 1)
      continue;

    g_ptr_array_add(switch_elems, elem);
  }

  if (!switch_elems->len) {
    g_ptr_array_free(switch_elems, TRUE);
    return;
  }

  struct autogain_window *w = g_malloc0(sizeof(struct autogain_window));

  w->card = card;
  w->rows = g_ptr_array_new();
  card->autogain_data = w;

  w->window = create_subwindow(
    card, "Autogain", G_CALLBACK(window_autogain_close_request)
  );
  card->window_autogain = w->window;

  GtkWidget *top = gtk_frame_new(NULL);
  gtk_widget_add_css_class(top, "window-frame");

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class(content, "window-content");
  add_card_style_class(content, card);
  gtk_frame_set_child(GTK_FRAME(top), content);

  w->instruction = config_help_label(
    "Speak, sing, or play as you would while recording."
  );
  gtk_widget_add_css_class(w->instruction, "autogain-idle");
  gtk_box_append(GTK_BOX(content), w->instruction);

  GtkSizeGroup *name_size_group = gtk_size_group_new(
    GTK_SIZE_GROUP_HORIZONTAL
  );

  for (int i = 0; i < switch_elems->len; i++) {
    struct autogain_row *row = create_autogain_row(
      w, g_ptr_array_index(switch_elems, i), name_size_group
    );

    g_ptr_array_add(w->rows, row);
    gtk_box_append(GTK_BOX(content), row->box);
  }

  g_object_unref(name_size_group);
  g_ptr_array_free(switch_elems, TRUE);

  w->hint = config_wrapped_help_label(
    "Before running Autogain again, check that the input is connected, "
    "that 48V is on if you’re using a condenser microphone, and that "
    "there is sound on the input while Autogain runs.",
    50
  );
  gtk_widget_set_visible(w->hint, FALSE);
  gtk_box_append(GTK_BOX(content), w->hint);

  create_autogain_buttons(w, GTK_BOX(content));

  gtk_window_set_child(GTK_WINDOW(w->window), top);
}

void destroy_autogain_window(struct alsa_card *card) {
  struct autogain_window *w = card->autogain_data;

  if (!w)
    return;

  for (int i = 0; i < w->rows->len; i++) {
    struct autogain_row *row = g_ptr_array_index(w->rows, i);

    alsa_elem_remove_callbacks_by_data(row->switch_elem, w);
    if (row->status_elem)
      alsa_elem_remove_callbacks_by_data(row->status_elem, row);
    if (row->gain_elem)
      alsa_elem_remove_callbacks_by_data(row->gain_elem, row);

    if (row->src && row->src->custom_name_elem)
      alsa_elem_remove_callbacks_by_data(row->src->custom_name_elem, row);

    g_free(row);
  }
  g_ptr_array_free(w->rows, TRUE);

  if (w->poll_timer)
    g_source_remove(w->poll_timer);

  gtk_window_destroy(GTK_WINDOW(w->window));
  card->window_autogain = NULL;

  g_free(w);
  card->autogain_data = NULL;
}
