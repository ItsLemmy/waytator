#include "window-private.h"

#include "editor-utils.h"
#include "stroke.h"

#include <math.h>

#define SWASH_MIN_ZOOM 0.10
#define SWASH_MAX_ZOOM 8.00
#define SWASH_TOUCH_TAP_MAX_DISTANCE 24.0
#define SWASH_TOUCH_TAP_MAX_DURATION_US (700 * G_TIME_SPAN_MILLISECOND)
#define SWASH_CROP_DRAG_LEFT (1u << 0)
#define SWASH_CROP_DRAG_RIGHT (1u << 1)
#define SWASH_CROP_DRAG_TOP (1u << 2)
#define SWASH_CROP_DRAG_BOTTOM (1u << 3)
#define SWASH_CROP_DRAG_MOVE (1u << 4)
#define SWASH_CROP_DRAG_NEW (1u << 5)

typedef struct {
  double x;
  double y;
} SwashTouchTapPoint;

static void swash_window_text_editing_cancel(SwashWindow *self);
void swash_window_text_editing_commit(SwashWindow *self);

static gboolean
swash_window_add_sampled_point(SwashWindow *self,
                               SwashStroke *stroke,
                               double       x,
                               double       y)
{
  const guint len = stroke->points->len;
  const double zoom = MAX(swash_window_get_effective_zoom(self), SWASH_MIN_ZOOM);
  const double minimum_distance = 2.5 / zoom;
  const double simplify_tolerance = 0.75 / zoom;
  const SwashPoint *last;

  if (len == 0) {
    swash_stroke_add_point(stroke, x, y);
    return TRUE;
  }

  last = &g_array_index(stroke->points, SwashPoint, len - 1);
  {
    const SwashPoint next = { x, y };

    if (!swash_point_is_far_enough(last, &next, minimum_distance))
      return FALSE;
  }

  if (len >= 2) {
    const SwashPoint *anchor = &g_array_index(stroke->points, SwashPoint, len - 2);
    const SwashPoint next = { x, y };

    if (swash_point_can_simplify(anchor, last, &next, simplify_tolerance)) {
      g_array_index(stroke->points, SwashPoint, len - 1) = next;
      return TRUE;
    }
  }

  swash_stroke_add_point(stroke, x, y);
  return TRUE;
}

static void
swash_window_get_viewport_size(SwashWindow *self,
                                  double         *width,
                                  double         *height)
{
  GtkAdjustment *hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);

  *width = gtk_adjustment_get_page_size(hadjustment);
  *height = gtk_adjustment_get_page_size(vadjustment);

  if (*width <= 0)
    *width = gtk_widget_get_width(GTK_WIDGET(self->canvas_scroller));

  if (*height <= 0)
    *height = gtk_widget_get_height(GTK_WIDGET(self->canvas_scroller));
}

static gboolean
swash_window_cancel_current_interaction(SwashWindow *self)
{
  GPtrArray *strokes;

  if (self->text_editing) {
    swash_window_text_editing_cancel(self);
    return TRUE;
  }

  if (!self->drawing)
    return FALSE;

  self->drawing = FALSE;
  swash_window_update_active_stroke_overlay(self);

  if (self->active_tool == SWASH_TOOL_PAN)
    return TRUE;

  if (self->active_tool == SWASH_TOOL_CROP) {
    swash_window_cancel_pending_crop(self);
    return TRUE;
  }

  if (self->active_touch_draw_sequence != NULL) {
    self->cancelled_touch_draw_sequence = self->active_touch_draw_sequence;
    self->active_touch_draw_sequence = NULL;
  }

  strokes = swash_window_strokes(self);
  if (self->current_stroke != NULL && strokes != NULL)
    g_ptr_array_remove(strokes, self->current_stroke);

  self->current_stroke = NULL;
  swash_window_reset_active_stroke_nodes(self);
  if (!self->interaction_has_undo_step) {
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    return TRUE;
  }

  swash_document_discard_undo_step(self->document);
  swash_window_refresh_document_state(self);
  swash_window_update_history_buttons(self);
  return TRUE;
}

static void
swash_window_cancel_touch_tool_gestures(SwashWindow *self)
{
  if (self->touch_pan_gesture != NULL)
    gtk_gesture_set_state(self->touch_pan_gesture, GTK_EVENT_SEQUENCE_DENIED);

  if (self->crop_gesture != NULL)
    gtk_gesture_set_state(self->crop_gesture, GTK_EVENT_SEQUENCE_DENIED);

  if (self->draw_gesture != NULL)
    gtk_gesture_set_state(self->draw_gesture, GTK_EVENT_SEQUENCE_DENIED);
}

static void
swash_window_reset_touch_tap(SwashWindow *self)
{
  self->touch_tap_candidate = FALSE;
  self->touch_tap_cancelled = FALSE;
  self->touch_tap_max_points = 0;
  self->touch_tap_started_at = 0;
  g_hash_table_remove_all(self->touch_tap_points);
}

static void
swash_window_cancel_touch_tap(SwashWindow *self,
                                 const char     *reason)
{
  (void) reason;

  if (!self->touch_tap_candidate || self->touch_tap_cancelled)
    return;

  self->touch_tap_cancelled = TRUE;
}

static void
swash_window_update_touch_tap_motion(SwashWindow  *self,
                                        GdkEventSequence *sequence,
                                        GdkEvent         *event)
{
  SwashTouchTapPoint *point;
  double x;
  double y;

  if (!self->touch_tap_candidate || self->touch_tap_cancelled || sequence == NULL)
    return;

  point = g_hash_table_lookup(self->touch_tap_points, sequence);
  if (point == NULL || !gdk_event_get_position(event, &x, &y))
    return;

  if (hypot(x - point->x, y - point->y) > SWASH_TOUCH_TAP_MAX_DISTANCE)
    swash_window_cancel_touch_tap(self, "after movement threshold");
}

static void
swash_window_finish_touch_tap(SwashWindow *self)
{
  const gint64 duration = g_get_monotonic_time() - self->touch_tap_started_at;

  if (!self->touch_tap_candidate)
    return;

  if (!self->touch_tap_cancelled && duration <= SWASH_TOUCH_TAP_MAX_DURATION_US) {
    if (self->touch_tap_max_points == 2) {
      gtk_widget_activate_action(GTK_WIDGET(self), "win.undo", NULL);
    } else if (self->touch_tap_max_points == 3) {
      gtk_widget_activate_action(GTK_WIDGET(self), "win.redo", NULL);
    }
  }

  swash_window_reset_touch_tap(self);
}

static gboolean
swash_window_touch_event(GtkEventControllerLegacy *controller,
                            GdkEvent                 *event,
                            gpointer                  user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  GdkEventSequence *sequence;
  double x;
  double y;

  (void) controller;

  if (event == NULL)
    return FALSE;

  sequence = gdk_event_get_event_sequence(event);

  switch (gdk_event_get_event_type(event)) {
  case GDK_TOUCH_BEGIN:
    if (g_hash_table_size(self->active_touch_sequences) == 0) {
      swash_window_reset_touch_tap(self);
      self->touch_tap_candidate = TRUE;
      self->touch_tap_started_at = g_get_monotonic_time();
    }

    if (sequence != NULL)
      g_hash_table_add(self->active_touch_sequences, sequence);

    self->touch_tap_max_points = MAX(self->touch_tap_max_points,
                                     g_hash_table_size(self->active_touch_sequences));
    if (sequence != NULL && gdk_event_get_position(event, &x, &y)) {
      SwashTouchTapPoint *point = g_new0(SwashTouchTapPoint, 1);

      point->x = x;
      point->y = y;
      g_hash_table_insert(self->touch_tap_points, sequence, point);
    }

    if (g_hash_table_size(self->active_touch_sequences) > 3)
      swash_window_cancel_touch_tap(self, "after more than 3 fingers");

    if (g_hash_table_size(self->active_touch_sequences) >= 2) {
      swash_window_cancel_current_interaction(self);
      swash_window_cancel_touch_tool_gestures(self);
    }
    break;
  case GDK_TOUCH_UPDATE:
    swash_window_update_touch_tap_motion(self, sequence, event);
    break;
  case GDK_TOUCH_END:
    swash_window_update_touch_tap_motion(self, sequence, event);

    if (sequence != NULL) {
      g_hash_table_remove(self->active_touch_sequences, sequence);
      g_hash_table_remove(self->touch_tap_points, sequence);
    }

    if (g_hash_table_size(self->active_touch_sequences) == 0)
      swash_window_finish_touch_tap(self);
    break;
  case GDK_TOUCH_CANCEL:
    swash_window_cancel_touch_tap(self, "after touch cancel");

    if (sequence != NULL)
      g_hash_table_remove(self->active_touch_sequences, sequence);

    if (sequence != NULL)
      g_hash_table_remove(self->touch_tap_points, sequence);

    if (g_hash_table_size(self->active_touch_sequences) == 0)
      swash_window_reset_touch_tap(self);
    break;
  default:
    break;
  }

  return FALSE;
}

static void
swash_window_record_interaction_undo_step(SwashWindow *self)
{
  if (self->interaction_has_undo_step)
    return;

  swash_window_record_undo_step(self);
  self->interaction_has_undo_step = TRUE;
}

static void
swash_window_begin_draw_stroke(SwashWindow *self)
{
  if (self->active_tool == SWASH_TOOL_ERASER || self->current_stroke != NULL)
    return;

  swash_window_record_interaction_undo_step(self);
  self->current_stroke = swash_stroke_new(self->active_tool,
                                             self->tool_widths[self->active_tool],
                                             &self->tool_colors[self->active_tool],
                                             &self->tool_fill_colors[self->active_tool],
                                             self->blur_type);
  swash_stroke_add_point(self->current_stroke, self->last_draw_x, self->last_draw_y);

  if (swash_tool_is_shape(self->active_tool))
    swash_stroke_add_point(self->current_stroke, self->last_draw_x, self->last_draw_y);

  if (self->active_tool == SWASH_TOOL_NUMBERING) {
    GPtrArray *strokes = swash_window_strokes(self);
    int count = 0;

    for (guint i = 0; i < strokes->len; i++) {
      SwashStroke *s = g_ptr_array_index(strokes, i);

      if (s->tool == SWASH_TOOL_NUMBERING)
        count++;
    }

    self->current_stroke->text = g_strdup_printf("%d", count + 1);
  }

  g_ptr_array_add(swash_window_strokes(self), self->current_stroke);
  swash_window_reset_save_button(self);
  swash_window_update_active_stroke_overlay(self);
}

static gboolean
swash_window_get_zoom_gesture_center(SwashWindow *self,
                                        GtkGesture      *gesture,
                                        double          *surface_x,
                                        double          *surface_y,
                                        double          *viewport_x,
                                        double          *viewport_y)
{
  graphene_point_t surface_point;
  graphene_point_t viewport_point;
  double local_surface_x;
  double local_surface_y;
  double *actual_surface_x = surface_x != NULL ? surface_x : &local_surface_x;
  double *actual_surface_y = surface_y != NULL ? surface_y : &local_surface_y;

  if (!gtk_gesture_get_bounding_box_center(gesture, actual_surface_x, actual_surface_y))
    return FALSE;

  surface_point.x = (float) *actual_surface_x;
  surface_point.y = (float) *actual_surface_y;
  if (!gtk_widget_compute_point(self->canvas_surface,
                                GTK_WIDGET(self->canvas_scroller),
                                &surface_point,
                                &viewport_point))
    return FALSE;

  *viewport_x = viewport_point.x;
  *viewport_y = viewport_point.y;
  return TRUE;
}

static void
swash_window_apply_pinch_gesture(SwashWindow   *self,
                                    GtkGestureZoom   *gesture,
                                    double            scale)
{
  GtkAdjustment *hadjustment;
  GtkAdjustment *vadjustment;
  double viewport_x;
  double viewport_y;
  double viewport_width;
  double viewport_height;
  const int image_width = gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture));
  const int image_height = gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture));
  const double zoom = CLAMP(self->pinch_start_zoom * scale, SWASH_MIN_ZOOM, SWASH_MAX_ZOOM);
  const double display_width = image_width * zoom;
  const double display_height = image_height * zoom;
  double widget_width;
  double widget_height;
  double display_x;
  double display_y;

  if (self->texture == NULL)
    return;

  if (!swash_window_get_zoom_gesture_center(self,
                                              GTK_GESTURE(gesture),
                                              NULL,
                                              NULL,
                                              &viewport_x,
                                              &viewport_y))
    swash_window_get_viewport_center(self, &viewport_x, &viewport_y);

  self->fit_mode = FALSE;
  self->zoom = zoom;
  swash_window_apply_zoom_mode(self);
  swash_window_update_zoom_label(self);

  swash_window_get_viewport_size(self, &viewport_width, &viewport_height);
  widget_width = MAX(viewport_width, display_width);
  widget_height = MAX(viewport_height, display_height);
  display_x = MAX(0.0, (widget_width - display_width) / 2.0);
  display_y = MAX(0.0, (widget_height - display_height) / 2.0);

  hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);
  swash_window_set_adjustment_clamped(hadjustment,
                                         display_x + self->pinch_anchor_rel_x * display_width - viewport_x);
  swash_window_set_adjustment_clamped(vadjustment,
                                         display_y + self->pinch_anchor_rel_y * display_height - viewport_y);
}

static gboolean
swash_window_global_key_pressed(GtkEventControllerKey *controller,
                                   guint                  keyval,
                                   guint                  keycode,
                                   GdkModifierType        state,
                                   gpointer               user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  GtkWidget *focus;

  (void) controller;
  (void) keycode;

  focus = gtk_root_get_focus(GTK_ROOT(self));
  if (focus != NULL && GTK_IS_EDITABLE(focus))
    return FALSE;

  if (keyval == GDK_KEY_Escape
      && (state & gtk_accelerator_get_default_mod_mask()) == 0) {
    if (swash_window_cancel_current_interaction(self))
      return TRUE;

    if (adw_bottom_sheet_get_open(self->ocr_panel_bottom_sheet)) {
      gtk_widget_activate_action(GTK_WIDGET(self), "win.dismiss", NULL);
      return TRUE;
    }

    if (self->active_tool == SWASH_TOOL_CROP && self->crop_selection_active) {
      swash_window_cancel_pending_crop(self);
      return TRUE;
    }

    if (self->esc_closes_window) {
      gtk_widget_activate_action(GTK_WIDGET(self), "win.close-window", NULL);
      return TRUE;
    }
  }

  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
      && (state & gtk_accelerator_get_default_mod_mask()) == 0
      && self->active_tool == SWASH_TOOL_CROP
      && self->crop_selection_active) {
    swash_window_apply_pending_crop(self);
    return TRUE;
  }

  if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_COPY, keyval, state)) {
    gtk_widget_activate_action(GTK_WIDGET(self), "win.copy-buffer", NULL);
    return TRUE;
  }

  if (self->texture != NULL && !self->drawing) {
    if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_SIZE_DECREASE, keyval, state)) {
      swash_window_adjust_tool_size(self, -1.0);
      return TRUE;
    }
    if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_SIZE_INCREASE, keyval, state)) {
      swash_window_adjust_tool_size(self, 1.0);
      return TRUE;
    }
  }

  if (!self->drawing) {
    GtkToggleButton *target = NULL;

    if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_BRUSH, keyval, state))
      target = self->brush_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_ARROW, keyval, state))
      target = self->arrow_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_RECTANGLE, keyval, state))
      target = self->rectangle_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_CIRCLE, keyval, state))
      target = self->circle_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_LINE, keyval, state))
      target = self->line_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_HIGHLIGHTER, keyval, state))
      target = self->highlighter_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_TEXT, keyval, state))
      target = self->text_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_NUMBERING, keyval, state))
      target = self->numbering_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_BLUR, keyval, state))
      target = self->blur_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_ERASER, keyval, state))
      target = self->eraser_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_CROP, keyval, state))
      target = self->crop_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_PAN, keyval, state))
      target = self->pan_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_OCR, keyval, state))
      target = self->ocr_tool_button;
    else if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_TOOL_MOVE, keyval, state))
      target = self->move_tool_button;

    if (target != NULL && self->texture != NULL) {
      gtk_toggle_button_set_active(target, TRUE);
      return TRUE;
    }
  }

  return FALSE;
}

#define SWASH_ZOOM_STEP 1.15

static gboolean
swash_tool_uses_angle_snapping(SwashTool tool)
{
  return tool == SWASH_TOOL_LINE || tool == SWASH_TOOL_ARROW;
}

static gboolean
swash_tool_uses_aspect_ratio_snapping(SwashTool tool)
{
  return tool == SWASH_TOOL_RECTANGLE || tool == SWASH_TOOL_CIRCLE;
}

static gboolean
swash_window_snap_modifier_active(GtkGestureDrag *gesture,
                                     SwashWindow *self)
{
  const GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

  return self->angle_snap_modifiers != 0
      && (state & self->angle_snap_modifiers) == self->angle_snap_modifiers;
}

static gboolean
swash_window_event_is_touch(GtkEventController *controller)
{
  GdkDevice *device = gtk_event_controller_get_current_event_device(controller);

  return device != NULL && gdk_device_get_source(device) == GDK_SOURCE_TOUCHSCREEN;
}

static void
swash_window_maybe_snap_shape_endpoint(GtkGestureDrag *gesture,
                                          SwashWindow *self,
                                          double         *x,
                                          double         *y)
{
  if (self->current_stroke == NULL
      || self->current_stroke->points->len == 0
      || x == NULL
      || y == NULL)
    return;

  if (!swash_window_snap_modifier_active(gesture, self))
    return;

  {
    const SwashPoint *start = &g_array_index(self->current_stroke->points, SwashPoint, 0);
    const double dx = *x - start->x;
    const double dy = *y - start->y;

    if (fabs(dx) < 0.0001 && fabs(dy) < 0.0001)
      return;

    if (swash_tool_uses_angle_snapping(self->active_tool)) {
      const double distance = hypot(dx, dy);
      const double snapped_angle = round(atan2(dy, dx) / (G_PI / 4.0)) * (G_PI / 4.0);

      *x = start->x + cos(snapped_angle) * distance;
      *y = start->y + sin(snapped_angle) * distance;
      return;
    }

    if (swash_tool_uses_aspect_ratio_snapping(self->active_tool)) {
      const double side = MAX(fabs(dx), fabs(dy));

      *x = start->x + (dx < 0.0 ? -side : side);
      *y = start->y + (dy < 0.0 ? -side : side);
    }
  }
}

static gboolean
swash_window_erase_strokes(SwashWindow *self,
                               double          x0,
                              double          y0,
                              double          x1,
                              double          y1)
{
  const double radius = self->tool_widths[SWASH_TOOL_ERASER] / 2.0;
  GPtrArray *strokes = swash_window_strokes(self);
  gboolean removed_stroke = FALSE;
  guint i;

  if (strokes == NULL)
    return FALSE;

  for (i = strokes->len; i > 0; i--) {
    SwashStroke *stroke = g_ptr_array_index(strokes, i - 1);

    if (swash_stroke_intersects_segment(stroke, x0, y0, x1, y1, radius)) {
      swash_window_record_interaction_undo_step(self);
      g_ptr_array_remove_index(strokes, i - 1);
      removed_stroke = TRUE;
    }
  }

  if (removed_stroke) {
    swash_strokes_renumber(strokes);
    swash_document_annotations_changed(self->document);
    swash_window_reset_save_button(self);
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  }

  return removed_stroke;
}

static void
swash_window_zoom_in_action(GtkWidget  *widget,
                               const char *action_name,
                               GVariant   *parameter)
{
  SwashWindow *self = SWASH_WINDOW(widget);
  double center_x;
  double center_y;
  double zoom;

  (void) action_name;
  (void) parameter;

  if (!swash_window_get_pointer_viewport_position(self, &center_x, &center_y))
    swash_window_get_viewport_center(self, &center_x, &center_y);
  zoom = swash_window_get_effective_zoom(self);
  swash_window_set_zoom_at(self, zoom * SWASH_ZOOM_STEP, center_x, center_y);
}

static void
swash_window_zoom_out_action(GtkWidget  *widget,
                                const char *action_name,
                                GVariant   *parameter)
{
  SwashWindow *self = SWASH_WINDOW(widget);
  double center_x;
  double center_y;
  double zoom;

  (void) action_name;
  (void) parameter;

  if (!swash_window_get_pointer_viewport_position(self, &center_x, &center_y))
    swash_window_get_viewport_center(self, &center_x, &center_y);
  zoom = swash_window_get_effective_zoom(self);
  swash_window_set_zoom_at(self, zoom / SWASH_ZOOM_STEP, center_x, center_y);
}

static void
swash_window_zoom_fit_action(GtkWidget  *widget,
                                const char *action_name,
                                GVariant   *parameter)
{
  (void) action_name;
  (void) parameter;

  swash_window_queue_fit_zoom(SWASH_WINDOW(widget));
}

static void
swash_window_drag_begin(GtkGestureDrag *gesture,
                           double          start_x,
                           double          start_y,
                           gpointer        user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  GtkAdjustment *hadjustment;
  GtkAdjustment *vadjustment;

  (void) start_x;
  (void) start_y;

  if (self->texture == NULL)
    return;

  if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_MIDDLE)
    return;

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);
  self->drag_start_hvalue = gtk_adjustment_get_value(hadjustment);
  self->drag_start_vvalue = gtk_adjustment_get_value(vadjustment);
}

static void
swash_window_drag_update(GtkGestureDrag *gesture,
                            double          offset_x,
                            double          offset_y,
                            gpointer        user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  GtkAdjustment *hadjustment;
  GtkAdjustment *vadjustment;

  (void) gesture;

  if (self->texture == NULL)
    return;

  if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_MIDDLE)
    return;

  hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);

  swash_window_set_adjustment_clamped(hadjustment, self->drag_start_hvalue - offset_x);
  swash_window_set_adjustment_clamped(vadjustment, self->drag_start_vvalue - offset_y);
}

static void
swash_window_begin_pan(SwashWindow *self)
{
  GtkAdjustment *hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);

  self->drag_start_hvalue = gtk_adjustment_get_value(hadjustment);
  self->drag_start_vvalue = gtk_adjustment_get_value(vadjustment);
}

static void
swash_window_update_pan(SwashWindow *self,
                           double          offset_x,
                           double          offset_y)
{
  GtkAdjustment *hadjustment = gtk_scrolled_window_get_hadjustment(self->canvas_scroller);
  GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(self->canvas_scroller);

  swash_window_set_adjustment_clamped(hadjustment, self->drag_start_hvalue - offset_x);
  swash_window_set_adjustment_clamped(vadjustment, self->drag_start_vvalue - offset_y);
}

static void
swash_window_pan_begin(GtkGestureDrag *gesture,
                          double          start_x,
                          double          start_y,
                          gpointer        user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);

  (void) start_x;
  (void) start_y;

  if (self->texture == NULL || self->active_tool != SWASH_TOOL_PAN)
    return;

  if (!swash_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture)))
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  self->drawing = TRUE;
  self->interaction_has_undo_step = FALSE;
  swash_window_begin_pan(self);
}

static void
swash_window_pan_update(GtkGestureDrag *gesture,
                           double          offset_x,
                           double          offset_y,
                           gpointer        user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);

  (void) gesture;

  if (self->texture == NULL || self->active_tool != SWASH_TOOL_PAN || !self->drawing)
    return;

  swash_window_update_pan(self, offset_x, offset_y);
}

static void
swash_window_pan_end(GtkGestureDrag *gesture,
                        double          offset_x,
                        double          offset_y,
                        gpointer        user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);

  (void) gesture;
  (void) offset_x;
  (void) offset_y;

  if (self->active_tool == SWASH_TOOL_PAN)
    self->drawing = FALSE;

  self->interaction_has_undo_step = FALSE;
}

static void
swash_window_normalize_crop_selection(SwashWindow *self)
{
  swash_crop_rect_normalize(&self->crop_start_x,
                            &self->crop_start_y,
                            &self->crop_end_x,
                            &self->crop_end_y);
}

void
swash_window_update_crop_controls(SwashWindow *self)
{
  const gboolean visible = self->texture != NULL
                        && self->active_tool == SWASH_TOOL_CROP;
  int width = 0;
  int height = 0;
  g_autofree char *dimensions = NULL;

  if (self->crop_selection_active)
    swash_crop_rect_dimensions(self->crop_start_x,
                               self->crop_start_y,
                               self->crop_end_x,
                               self->crop_end_y,
                               &width,
                               &height);
  dimensions = g_strdup_printf("%d × %d px", width, height);
  gtk_widget_set_visible(self->crop_controls, visible);
  gtk_label_set_text(self->crop_dimensions_label, dimensions);
  gtk_widget_set_sensitive(GTK_WIDGET(self->crop_apply_button),
                           self->crop_selection_active && width > 0 && height > 0);
}

void
swash_window_cancel_pending_crop(SwashWindow *self)
{
  self->drawing = FALSE;
  self->crop_selection_active = FALSE;
  self->crop_drag_mode = 0;
  self->crop_start_x = 0.0;
  self->crop_start_y = 0.0;
  self->crop_end_x = 0.0;
  self->crop_end_y = 0.0;
  swash_window_update_crop_controls(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

void
swash_window_apply_pending_crop(SwashWindow *self)
{
  int left;
  int top;
  int right;
  int bottom;

  if (!self->crop_selection_active || self->texture == NULL)
    return;

  swash_window_normalize_crop_selection(self);
  left = CLAMP((int) floor(self->crop_start_x), 0, gdk_texture_get_width(self->texture) - 1);
  top = CLAMP((int) floor(self->crop_start_y), 0, gdk_texture_get_height(self->texture) - 1);
  right = CLAMP((int) ceil(self->crop_end_x), left + 1, gdk_texture_get_width(self->texture));
  bottom = CLAMP((int) ceil(self->crop_end_y), top + 1, gdk_texture_get_height(self->texture));
  swash_window_cancel_pending_crop(self);
  swash_window_apply_crop(self, left, top, right - left, bottom - top);
  swash_window_update_crop_controls(self);
}

static guint
swash_window_crop_hit_test(SwashWindow *self,
                              double          x,
                              double          y)
{
  const double tolerance = 10.0 / MAX(swash_window_get_effective_zoom(self), 0.01);
  guint mode = 0;

  if (!self->crop_selection_active)
    return SWASH_CROP_DRAG_NEW;

  if (fabs(x - self->crop_start_x) <= tolerance)
    mode |= SWASH_CROP_DRAG_LEFT;
  else if (fabs(x - self->crop_end_x) <= tolerance)
    mode |= SWASH_CROP_DRAG_RIGHT;

  if (fabs(y - self->crop_start_y) <= tolerance)
    mode |= SWASH_CROP_DRAG_TOP;
  else if (fabs(y - self->crop_end_y) <= tolerance)
    mode |= SWASH_CROP_DRAG_BOTTOM;

  if (mode != 0
      && x >= self->crop_start_x - tolerance
      && x <= self->crop_end_x + tolerance
      && y >= self->crop_start_y - tolerance
      && y <= self->crop_end_y + tolerance)
    return mode;

  if (x >= self->crop_start_x && x <= self->crop_end_x
      && y >= self->crop_start_y && y <= self->crop_end_y)
    return SWASH_CROP_DRAG_MOVE;

  return SWASH_CROP_DRAG_NEW;
}

static void
swash_window_crop_begin(GtkGestureDrag *gesture,
                           double          start_x,
                           double          start_y,
                           gpointer        user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);

  if (self->texture == NULL || self->active_tool != SWASH_TOOL_CROP)
    return;

  if (!swash_window_get_image_point(self,
                                    start_x,
                                    start_y,
                                    FALSE,
                                    &self->crop_drag_x,
                                    &self->crop_drag_y))
    return;

  if (!swash_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture)))
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  self->drawing = TRUE;
  self->interaction_has_undo_step = FALSE;
  self->crop_drag_mode = swash_window_crop_hit_test(self,
                                                   self->crop_drag_x,
                                                   self->crop_drag_y);
  self->crop_original_left = self->crop_start_x;
  self->crop_original_top = self->crop_start_y;
  self->crop_original_right = self->crop_end_x;
  self->crop_original_bottom = self->crop_end_y;
  if (self->crop_drag_mode == SWASH_CROP_DRAG_NEW) {
    self->crop_selection_active = TRUE;
    self->crop_start_x = self->crop_drag_x;
    self->crop_start_y = self->crop_drag_y;
    self->crop_end_x = self->crop_drag_x;
    self->crop_end_y = self->crop_drag_y;
  }
  swash_window_update_crop_controls(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
swash_window_crop_update(GtkGestureDrag *gesture,
                            double          offset_x,
                            double          offset_y,
                            gpointer        user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  double current_x;
  double current_y;
  int image_width;
  int image_height;

  if (self->texture == NULL || self->active_tool != SWASH_TOOL_CROP || !self->drawing)
    return;

  image_width = gdk_texture_get_width(self->texture);
  image_height = gdk_texture_get_height(self->texture);

  double start_x;
  double start_y;

  gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
  if (!swash_window_get_image_point(self,
                                    start_x + offset_x,
                                    start_y + offset_y,
                                    TRUE,
                                    &current_x,
                                    &current_y))
    return;

  if (self->crop_drag_mode == SWASH_CROP_DRAG_NEW) {
    self->crop_end_x = current_x;
    self->crop_end_y = current_y;
  } else if (self->crop_drag_mode == SWASH_CROP_DRAG_MOVE) {
    const double width = self->crop_original_right - self->crop_original_left;
    const double height = self->crop_original_bottom - self->crop_original_top;
    const double left = CLAMP(self->crop_original_left + current_x - self->crop_drag_x,
                              0.0,
                              MAX(0.0, image_width - width));
    const double top = CLAMP(self->crop_original_top + current_y - self->crop_drag_y,
                             0.0,
                             MAX(0.0, image_height - height));

    self->crop_start_x = left;
    self->crop_start_y = top;
    self->crop_end_x = left + width;
    self->crop_end_y = top + height;
  } else {
    if (self->crop_drag_mode & SWASH_CROP_DRAG_LEFT)
      self->crop_start_x = MIN(current_x, self->crop_original_right - 1.0);
    if (self->crop_drag_mode & SWASH_CROP_DRAG_RIGHT)
      self->crop_end_x = MAX(current_x, self->crop_original_left + 1.0);
    if (self->crop_drag_mode & SWASH_CROP_DRAG_TOP)
      self->crop_start_y = MIN(current_y, self->crop_original_bottom - 1.0);
    if (self->crop_drag_mode & SWASH_CROP_DRAG_BOTTOM)
      self->crop_end_y = MAX(current_y, self->crop_original_top + 1.0);
  }

  swash_window_update_crop_controls(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
swash_window_crop_end(GtkGestureDrag *gesture,
                         double          offset_x,
                         double          offset_y,
                         gpointer        user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);

  (void) gesture;
  (void) offset_x;
  (void) offset_y;

  if (self->active_tool != SWASH_TOOL_CROP || !self->drawing) {
    self->drawing = FALSE;
    return;
  }

  self->drawing = FALSE;

  swash_window_normalize_crop_selection(self);
  self->crop_selection_active = self->crop_end_x - self->crop_start_x >= 1.0
                             && self->crop_end_y - self->crop_start_y >= 1.0;
  if (!self->crop_selection_active)
    swash_window_cancel_pending_crop(self);
  self->crop_drag_mode = 0;
  self->interaction_has_undo_step = FALSE;
  swash_window_update_crop_controls(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
swash_window_text_editing_stop(SwashWindow *self)
{
  self->text_editing = FALSE;
  self->text_cursor_visible = FALSE;
  if (self->text_cursor_blink_id != 0) {
    g_source_remove(self->text_cursor_blink_id);
    self->text_cursor_blink_id = 0;
  }
  if (self->text_input_hidden != NULL) {
    const gboolean had_focus = gtk_widget_has_focus(self->text_input_hidden);

    gtk_widget_set_visible(self->text_input_hidden, FALSE);
    /* Focus has to leave the hidden entry explicitly: while it holds focus,
     * key events are routed to it and window shortcuts such as copy never
     * reach the canvas. */
    if (had_focus)
      gtk_widget_grab_focus(GTK_WIDGET(self->drawing_area));
  }
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

void
swash_window_text_editing_commit(SwashWindow *self)
{
  if (!self->text_editing)
    return;

  if (self->current_stroke != NULL
      && (self->current_stroke->text == NULL || *self->current_stroke->text == '\0')) {
    GPtrArray *strokes = swash_window_strokes(self);

    if (strokes != NULL)
      g_ptr_array_remove(strokes, self->current_stroke);

    self->current_stroke = NULL;
    swash_document_discard_undo_step(self->document);
    swash_window_refresh_document_state(self);
    swash_window_update_history_buttons(self);
  } else {
    swash_document_annotations_changed(self->document);
    swash_window_maybe_auto_copy_latest_change(self);
    self->current_stroke = NULL;
  }

  swash_window_text_editing_stop(self);
}

static void
swash_window_text_editing_cancel(SwashWindow *self)
{
  GPtrArray *strokes;

  if (!self->text_editing)
    return;

  strokes = swash_window_strokes(self);
  if (self->current_stroke != NULL && strokes != NULL)
    g_ptr_array_remove(strokes, self->current_stroke);

  self->current_stroke = NULL;
  swash_window_reset_active_stroke_nodes(self);
  swash_document_discard_undo_step(self->document);
  swash_window_refresh_document_state(self);
  swash_window_update_history_buttons(self);
  swash_window_text_editing_stop(self);
}

static gboolean
swash_window_text_cursor_blink(gpointer user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);

  if (!self->text_editing) {
    self->text_cursor_blink_id = 0;
    return G_SOURCE_REMOVE;
  }

  self->text_cursor_visible = !self->text_cursor_visible;
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  return G_SOURCE_CONTINUE;
}

static void
swash_window_text_input_changed(GtkEditable *editable,
                                   gpointer     user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  const char *text;

  if (!self->text_editing || self->current_stroke == NULL)
    return;

  text = gtk_editable_get_text(editable);
  g_free(self->current_stroke->text);
  self->current_stroke->text = (text != NULL && *text != '\0') ? g_strdup(text) : NULL;
  self->text_cursor_visible = TRUE;
  if (self->text_cursor_blink_id != 0)
    g_source_remove(self->text_cursor_blink_id);
  self->text_cursor_blink_id = g_timeout_add(530, swash_window_text_cursor_blink, self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
swash_window_text_input_focus_lost(GtkEventControllerFocus *controller,
                                      gpointer                 user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);

  (void) controller;

  if (self->text_editing)
    swash_window_text_editing_commit(self);
}

static gboolean
swash_window_text_input_key_pressed(GtkEventControllerKey *controller,
                                       guint                  keyval,
                                       guint                  keycode,
                                       GdkModifierType        state,
                                       gpointer               user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);

  (void) controller;
  (void) keycode;

  if (!self->text_editing)
    return FALSE;

  /* The hidden entry consumes every key while text is being typed, so the
   * copy shortcut is handled here as well; the annotation in progress is
   * committed first so that it is part of what gets copied. */
  if (swash_window_shortcut_matches(self, SWASH_SHORTCUT_COPY, keyval, state)) {
    swash_window_text_editing_commit(self);
    gtk_widget_activate_action(GTK_WIDGET(self), "win.copy-buffer", NULL);
    return TRUE;
  }

  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    swash_window_text_editing_commit(self);
    return TRUE;
  }

  if (keyval == GDK_KEY_Escape) {
    swash_window_text_editing_cancel(self);
    return TRUE;
  }

  return FALSE;
}

static void
swash_window_draw_begin(GtkGestureDrag *gesture,
                           double          start_x,
                           double          start_y,
                           gpointer        user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  const gboolean is_touch = swash_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture));

  if (self->text_editing) {
    swash_window_text_editing_commit(self);
    return;
  }

  if (self->texture == NULL)
    return;

  if (swash_tool_is_non_drawing(self->active_tool))
    return;

  if (!swash_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture)))
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  if (!swash_window_get_image_point(self,
                                       start_x,
                                       start_y,
                                       FALSE,
                                       &self->last_draw_x,
                                       &self->last_draw_y))
    return;

  self->drawing = TRUE;
  self->interaction_has_undo_step = FALSE;

  if (self->active_tool == SWASH_TOOL_MOVE) {
    GPtrArray *strokes = swash_window_strokes(self);

    self->selected_stroke = NULL;
    self->move_stroke_moved = FALSE;
    if (strokes != NULL) {
      for (int i = (int) strokes->len - 1; i >= 0; i--) {
        SwashStroke *stroke = g_ptr_array_index(strokes, i);

        if (swash_stroke_hit_test(stroke, self->last_draw_x, self->last_draw_y, 8.0)) {
          self->selected_stroke = stroke;
          self->move_start_x = self->last_draw_x;
          self->move_start_y = self->last_draw_y;
          swash_window_record_undo_step(self);
          self->interaction_has_undo_step = TRUE;
          break;
        }
      }
    }
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    return;
  }
  if (is_touch) {
    self->active_touch_draw_sequence = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
    self->cancelled_touch_draw_sequence = NULL;
  } else {
    self->active_touch_draw_sequence = NULL;
    self->cancelled_touch_draw_sequence = NULL;
  }

  if (is_touch)
    return;

  swash_window_begin_draw_stroke(self);

  if (self->active_tool == SWASH_TOOL_ERASER) {
    self->pointer_x = self->last_draw_x;
    self->pointer_y = self->last_draw_y;
    swash_window_erase_strokes(self,
                                  self->last_draw_x,
                                  self->last_draw_y,
                                  self->last_draw_x,
                                  self->last_draw_y);
    return;
  }
}

static void
swash_window_draw_update(GtkGestureDrag *gesture,
                            double          offset_x,
                            double          offset_y,
                            gpointer        user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  GdkEventSequence *sequence = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
  double start_x;
  double start_y;
  double image_x;
  double image_y;

  if (!self->drawing || self->texture == NULL)
    return;

  if (swash_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture))
      && sequence != NULL
      && sequence == self->cancelled_touch_draw_sequence)
    return;

  if (swash_tool_is_non_drawing(self->active_tool))
    return;

  gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
  if (!swash_window_get_image_point(self,
                                       start_x + offset_x,
                                       start_y + offset_y,
                                       TRUE,
                                       &image_x,
                                       &image_y))
    return;

  if (self->active_tool == SWASH_TOOL_NUMBERING)
    return;

  if (self->active_tool == SWASH_TOOL_MOVE) {
    if (self->selected_stroke != NULL) {
      const double dx = image_x - self->move_start_x;
      const double dy = image_y - self->move_start_y;

      swash_stroke_offset(self->selected_stroke, dx, dy);
      /* The document is only marked changed once at drag end: bumping the
       * annotations generation here would rebuild the full annotation cache
       * on every motion event. */
      self->move_stroke_moved = TRUE;
      self->move_start_x = image_x;
      self->move_start_y = image_y;
      gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    }
    return;
  }

  if (self->active_tool == SWASH_TOOL_ERASER) {
    self->pointer_x = image_x;
    self->pointer_y = image_y;
    swash_window_erase_strokes(self,
                                  self->last_draw_x,
                                  self->last_draw_y,
                                  image_x,
                                  image_y);
  } else {
    swash_window_begin_draw_stroke(self);

    if (self->current_stroke == NULL)
      return;

    swash_window_maybe_snap_shape_endpoint(gesture, self, &image_x, &image_y);

    if (swash_tool_is_shape(self->active_tool)) {
      swash_stroke_set_last_point(self->current_stroke, image_x, image_y);
    } else {
      if (!swash_window_add_sampled_point(self, self->current_stroke, image_x, image_y)) {
        self->last_draw_x = image_x;
        self->last_draw_y = image_y;
        return;
      }
    }

    swash_window_update_active_stroke_overlay(self);
  }

  self->last_draw_x = image_x;
  self->last_draw_y = image_y;
}

static void
swash_window_draw_end(GtkGestureDrag *gesture,
                         double          offset_x,
                         double          offset_y,
                         gpointer        user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  const gboolean was_drawing = self->drawing;
  GdkEventSequence *sequence = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
  double start_x;
  double start_y;
  double image_x;
  double image_y;

  self->drawing = FALSE;
  swash_window_update_active_stroke_overlay(self);

  if (swash_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture))
      && sequence != NULL
      && sequence == self->active_touch_draw_sequence)
    self->active_touch_draw_sequence = NULL;

  if (!was_drawing)
    goto done;

  if (swash_window_event_is_touch(GTK_EVENT_CONTROLLER(gesture))
      && sequence != NULL
      && sequence == self->cancelled_touch_draw_sequence)
    goto done;

  if (self->texture == NULL)
    goto done;

  if (swash_tool_is_non_drawing(self->active_tool))
    goto done;

  if (self->active_tool == SWASH_TOOL_MOVE) {
    if (self->move_stroke_moved)
      swash_document_annotations_changed(self->document);
    if (self->selected_stroke != NULL && !self->interaction_has_undo_step) {
      swash_document_discard_undo_step(self->document);
      swash_window_update_history_buttons(self);
    }
    if (self->interaction_has_undo_step)
      swash_window_maybe_auto_copy_latest_change(self);
    self->selected_stroke = NULL;
    self->interaction_has_undo_step = FALSE;
    self->move_stroke_moved = FALSE;
    gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
    goto done;
  }

  if (self->active_tool == SWASH_TOOL_ERASER) {
    const gboolean erased_anything = self->interaction_has_undo_step;

    self->interaction_has_undo_step = FALSE;
    if (erased_anything)
      swash_window_maybe_auto_copy_latest_change(self);
    goto done;
  }

  if (!gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y))
    goto done;

  if (!swash_window_get_image_point(self,
                                       start_x + offset_x,
                                       start_y + offset_y,
                                       TRUE,
                                       &image_x,
                                       &image_y))
    goto done;

  if (self->current_stroke == NULL)
    swash_window_begin_draw_stroke(self);

  if (self->current_stroke == NULL)
    goto done;

  swash_window_maybe_snap_shape_endpoint(gesture, self, &image_x, &image_y);

  if (swash_tool_is_shape(self->active_tool))
    swash_stroke_set_last_point(self->current_stroke, image_x, image_y);
  else
    swash_stroke_add_point(self->current_stroke, image_x, image_y);

  if (self->active_tool == SWASH_TOOL_TEXT) {
    self->text_editing = TRUE;
    self->text_cursor_visible = TRUE;
    g_free(self->current_stroke->text);
    self->current_stroke->text = NULL;
    gtk_editable_set_text(GTK_EDITABLE(self->text_input_hidden), "");
    gtk_widget_set_visible(self->text_input_hidden, TRUE);
    gtk_widget_grab_focus(self->text_input_hidden);
    self->text_cursor_blink_id = g_timeout_add(530, swash_window_text_cursor_blink, self);
  }

  if (self->active_tool != SWASH_TOOL_TEXT) {
    swash_document_annotations_changed(self->document);
    swash_window_annotation_cache_append_stroke(self, self->current_stroke);
  }
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  if (self->active_tool != SWASH_TOOL_TEXT)
    swash_window_maybe_auto_copy_latest_change(self);

done:
  self->interaction_has_undo_step = FALSE;
  if (sequence != NULL && sequence == self->cancelled_touch_draw_sequence)
    self->cancelled_touch_draw_sequence = NULL;
  self->current_stroke = (self->active_tool == SWASH_TOOL_TEXT) ? self->current_stroke : NULL;
  swash_window_reset_active_stroke_nodes(self);
  swash_window_update_active_stroke_overlay(self);
}

static gboolean
swash_window_scroll_zoom(GtkEventControllerScroll *controller,
                            double                    dx,
                            double                    dy,
                            gpointer                  user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  GdkModifierType state;
  GdkEvent *event;
  double anchor_x;
  double anchor_y;
  const double zoom = swash_window_get_effective_zoom(self);

  (void) dx;

  if (self->texture == NULL)
    return FALSE;

  state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
  if ((state & GDK_CONTROL_MASK) != 0)
    return FALSE;

  event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
  if (event == NULL || !gdk_event_get_position(event, &anchor_x, &anchor_y)) {
    if (!swash_window_get_pointer_viewport_position(self, &anchor_x, &anchor_y))
      swash_window_get_viewport_center(self, &anchor_x, &anchor_y);
  }

  swash_window_set_zoom_at(self,
                              zoom * pow(SWASH_ZOOM_STEP, -dy),
                              anchor_x,
                              anchor_y);

  return TRUE;
}

static void
swash_window_zoom_gesture_begin(GtkGesture       *gesture,
                                   GdkEventSequence *sequence,
                                   gpointer          user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  double surface_x;
  double surface_y;
  double viewport_x;
  double viewport_y;

  (void) sequence;

  if (self->texture == NULL)
    return;

  swash_window_cancel_current_interaction(self);
  swash_window_cancel_touch_tool_gestures(self);
  gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
  self->pinch_start_zoom = swash_window_get_effective_zoom(self);
  if (!swash_window_get_zoom_gesture_center(self,
                                              gesture,
                                              &surface_x,
                                              &surface_y,
                                              &viewport_x,
                                              &viewport_y)) {
    swash_window_get_viewport_center(self,
                                        &viewport_x,
                                        &viewport_y);
    surface_x = viewport_x;
    surface_y = viewport_y;
  }

  self->pointer_widget_x = viewport_x;
  self->pointer_widget_y = viewport_y;

  {
    double display_x;
    double display_y;
    double display_width;
    double display_height;
    double widget_width = gtk_widget_get_width(self->canvas_surface);
    double widget_height = gtk_widget_get_height(self->canvas_surface);

    if (!swash_window_get_display_rect(self,
                                          widget_width,
                                          widget_height,
                                          &display_x,
                                          &display_y,
                                          &display_width,
                                          &display_height)) {
      self->pinch_anchor_rel_x = 0.5;
      self->pinch_anchor_rel_y = 0.5;
      return;
    }

    self->pinch_anchor_rel_x = display_width > 0.0
                             ? CLAMP((surface_x - display_x) / display_width, 0.0, 1.0)
                             : 0.5;
    self->pinch_anchor_rel_y = display_height > 0.0
                             ? CLAMP((surface_y - display_y) / display_height, 0.0, 1.0)
                             : 0.5;
  }
}

static void
swash_window_zoom_gesture_update(GtkGesture *gesture,
                                    GdkEventSequence *sequence,
                                    gpointer          user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);
  (void) sequence;

  if (self->texture == NULL)
    return;

  gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
  swash_window_apply_pinch_gesture(self,
                                      GTK_GESTURE_ZOOM(gesture),
                                      gtk_gesture_zoom_get_scale_delta(GTK_GESTURE_ZOOM(gesture)));
}

static void
swash_window_pointer_enter(GtkEventControllerMotion *controller,
                              double                    x,
                              double                    y,
                              gpointer                  user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);

  (void) controller;

  self->pointer_in = TRUE;
  self->pointer_widget_x = x;
  self->pointer_widget_y = y;
}

static void
swash_window_pointer_leave(GtkEventControllerMotion *controller,
                              gpointer                  user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);

  (void) controller;

  self->pointer_in = FALSE;
}

static void
swash_window_pointer_motion(GtkEventControllerMotion *controller,
                               double                    x,
                               double                    y,
                               gpointer                  user_data)
{
  SwashWindow *self = SWASH_WINDOW(user_data);

  (void) controller;

  self->pointer_widget_x = x;
  self->pointer_widget_y = y;
  if (!swash_window_get_image_point(self, x, y, TRUE, &self->pointer_x, &self->pointer_y))
    return;
}

void
swash_window_install_canvas_actions(GtkWidgetClass *widget_class)
{
  swash_window_install_history_actions(widget_class);
  gtk_widget_class_install_action(widget_class, "win.zoom-in", NULL, swash_window_zoom_in_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-out", NULL, swash_window_zoom_out_action);
  gtk_widget_class_install_action(widget_class, "win.zoom-fit", NULL, swash_window_zoom_fit_action);
}

void
swash_window_setup_controllers(SwashWindow *self)
{
  GtkEventController *scroll;
  GtkEventController *keys;
  GtkGesture *drag;
  GtkGesture *pan_drag;
  GtkGesture *pan_drag_touch;
  GtkGesture *crop;
  GtkGesture *draw;
  GtkGesture *zoom;
  GtkEventController *legacy;
  GtkEventController *motion;

  drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_MIDDLE);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(swash_window_drag_begin), self);
  g_signal_connect(drag, "drag-update", G_CALLBACK(swash_window_drag_update), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_scroller), GTK_EVENT_CONTROLLER(drag));

  pan_drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pan_drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(pan_drag, "drag-begin", G_CALLBACK(swash_window_pan_begin), self);
  g_signal_connect(pan_drag, "drag-update", G_CALLBACK(swash_window_pan_update), self);
  g_signal_connect(pan_drag, "drag-end", G_CALLBACK(swash_window_pan_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_scroller), GTK_EVENT_CONTROLLER(pan_drag));

  pan_drag_touch = gtk_gesture_drag_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(pan_drag_touch), TRUE);
  g_signal_connect(pan_drag_touch, "drag-begin", G_CALLBACK(swash_window_pan_begin), self);
  g_signal_connect(pan_drag_touch, "drag-update", G_CALLBACK(swash_window_pan_update), self);
  g_signal_connect(pan_drag_touch, "drag-end", G_CALLBACK(swash_window_pan_end), self);
  gtk_widget_add_controller(self->canvas_surface, GTK_EVENT_CONTROLLER(pan_drag_touch));
  self->touch_pan_gesture = pan_drag_touch;

  crop = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(crop), GDK_BUTTON_PRIMARY);
  g_signal_connect(crop, "drag-begin", G_CALLBACK(swash_window_crop_begin), self);
  g_signal_connect(crop, "drag-update", G_CALLBACK(swash_window_crop_update), self);
  g_signal_connect(crop, "drag-end", G_CALLBACK(swash_window_crop_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self->drawing_area), GTK_EVENT_CONTROLLER(crop));
  self->crop_gesture = crop;

  draw = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(draw), GDK_BUTTON_PRIMARY);
  g_signal_connect(draw, "drag-begin", G_CALLBACK(swash_window_draw_begin), self);
  g_signal_connect(draw, "drag-update", G_CALLBACK(swash_window_draw_update), self);
  g_signal_connect(draw, "drag-end", G_CALLBACK(swash_window_draw_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self->drawing_area), GTK_EVENT_CONTROLLER(draw));
  self->draw_gesture = draw;

  zoom = gtk_gesture_zoom_new();
  g_signal_connect(zoom, "begin", G_CALLBACK(swash_window_zoom_gesture_begin), self);
  g_signal_connect(zoom, "update", G_CALLBACK(swash_window_zoom_gesture_update), self);
  gtk_widget_add_controller(self->canvas_surface, GTK_EVENT_CONTROLLER(zoom));
  self->zoom_gesture = zoom;

  legacy = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(legacy, GTK_PHASE_CAPTURE);
  g_signal_connect(legacy, "event", G_CALLBACK(swash_window_touch_event), self);
  gtk_widget_add_controller(self->canvas_surface, legacy);
  self->touch_legacy_controller = legacy;

  motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "enter", G_CALLBACK(swash_window_pointer_enter), self);
  g_signal_connect(motion, "leave", G_CALLBACK(swash_window_pointer_leave), self);
  g_signal_connect(motion, "motion", G_CALLBACK(swash_window_pointer_motion), self);
  gtk_widget_add_controller(GTK_WIDGET(self->drawing_area), motion);

  scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect(scroll, "scroll", G_CALLBACK(swash_window_scroll_zoom), self);
  gtk_widget_add_controller(GTK_WIDGET(self->canvas_scroller), scroll);

  keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(swash_window_global_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), keys);

  {
    GtkWidget *text_input = gtk_text_new();
    GtkEventController *text_keys = gtk_event_controller_key_new();

    GtkEventController *text_focus = gtk_event_controller_focus_new();

    gtk_widget_set_size_request(text_input, 1, 1);
    gtk_widget_set_opacity(text_input, 0.0);
    gtk_widget_set_visible(text_input, FALSE);
    gtk_widget_set_can_focus(text_input, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(self->canvas_surface), text_input);
    g_signal_connect(text_input, "changed", G_CALLBACK(swash_window_text_input_changed), self);
    gtk_event_controller_set_propagation_phase(text_keys, GTK_PHASE_CAPTURE);
    g_signal_connect(text_keys, "key-pressed", G_CALLBACK(swash_window_text_input_key_pressed), self);
    gtk_widget_add_controller(text_input, text_keys);
    g_signal_connect(text_focus, "leave", G_CALLBACK(swash_window_text_input_focus_lost), self);
    gtk_widget_add_controller(text_input, text_focus);
    self->text_input_hidden = text_input;
  }
}
