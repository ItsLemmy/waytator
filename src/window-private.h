#pragma once

#include "document.h"
#include "types.h"
#include "window.h"

typedef enum {
  SWASH_SHORTCUT_OPEN,
  SWASH_SHORTCUT_SAVE,
  SWASH_SHORTCUT_COPY,
  SWASH_SHORTCUT_UNDO,
  SWASH_SHORTCUT_REDO,
  SWASH_SHORTCUT_ZOOM_IN,
  SWASH_SHORTCUT_ZOOM_OUT,
  SWASH_SHORTCUT_ZOOM_FIT,
  SWASH_SHORTCUT_ROTATE,
  SWASH_SHORTCUT_FLIP_HORIZONTAL,
  SWASH_SHORTCUT_FLIP_VERTICAL,
  SWASH_SHORTCUT_PREFERENCES,
  SWASH_SHORTCUT_QUIT,
  SWASH_SHORTCUT_SIZE_DECREASE,
  SWASH_SHORTCUT_SIZE_INCREASE,
  SWASH_SHORTCUT_TOOL_MOVE,
  SWASH_SHORTCUT_TOOL_BRUSH,
  SWASH_SHORTCUT_TOOL_ARROW,
  SWASH_SHORTCUT_TOOL_RECTANGLE,
  SWASH_SHORTCUT_TOOL_CIRCLE,
  SWASH_SHORTCUT_TOOL_LINE,
  SWASH_SHORTCUT_TOOL_HIGHLIGHTER,
  SWASH_SHORTCUT_TOOL_TEXT,
  SWASH_SHORTCUT_TOOL_NUMBERING,
  SWASH_SHORTCUT_TOOL_BLUR,
  SWASH_SHORTCUT_TOOL_ERASER,
  SWASH_SHORTCUT_TOOL_CROP,
  SWASH_SHORTCUT_TOOL_PAN,
  SWASH_SHORTCUT_TOOL_OCR,
  SWASH_SHORTCUT_COUNT,
} SwashShortcut;

struct _SwashWindow {
  AdwApplicationWindow parent_instance;

  GtkStack *canvas_stack;
  GtkScrolledWindow *canvas_scroller;
  GtkWidget *empty_page;
  GtkWidget *canvas_surface;
  GtkPicture *picture;
  GtkDrawingArea *drawing_area;
  GtkWidget *active_stroke_overlay;
  GtkGesture *touch_pan_gesture;
  GtkGesture *crop_gesture;
  GtkGesture *draw_gesture;
  GtkGesture *zoom_gesture;
  GtkEventController *touch_legacy_controller;
  GtkFixed *ocr_overlay;
  AdwBottomSheet *ocr_panel_bottom_sheet;
  GtkWidget *ocr_panel;
  GtkWidget *ocr_panel_toggle_container;
  GtkToggleButton *ocr_panel_toggle_button;
  GtkButton *ocr_panel_close_button;
  GtkStack *ocr_panel_stack;
  GtkStackSwitcher *ocr_panel_tabs;
  GtkScrolledWindow *ocr_selected_page;
  GtkScrolledWindow *ocr_all_page;
  GtkTextView *ocr_selected_text_view;
  GtkTextView *ocr_all_text_view;
  GtkWidget *start_actions;
  GtkWidget *start_window_controls_pill;
  GtkWindowControls *start_window_controls;
  GListModel *start_window_controls_children;
  GtkWidget *open_actions;
  GtkButton *open_button;
  GtkButton *empty_open_button;
  GtkWidget *file_group;
  GtkMenuButton *file_button;
  GtkLabel *file_label;
  GtkLabel *zoom_label;
  GtkWidget *tool_group;
  GtkToggleButton *pan_tool_button;
  GtkToggleButton *crop_tool_button;
  GtkToggleButton *brush_tool_button;
  GtkToggleButton *highlighter_tool_button;
  GtkToggleButton *eraser_tool_button;
  GtkToggleButton *rectangle_tool_button;
  GtkToggleButton *circle_tool_button;
  GtkToggleButton *line_tool_button;
  GtkToggleButton *arrow_tool_button;
  GtkToggleButton *ocr_tool_button;
  GtkToggleButton *text_tool_button;
  GtkToggleButton *blur_tool_button;
  GtkToggleButton *numbering_tool_button;
  GtkToggleButton *move_tool_button;
  GtkButton *rotate_counter_clockwise_button;
  GtkButton *flip_horizontal_button;
  GtkButton *flip_vertical_button;
  GtkWidget *history_actions;
  GtkButton *undo_button;
  GtkButton *redo_button;
  GtkWidget *document_actions;
  GtkMenuButton *save_button;
  GtkStack *save_icon_stack;
  GtkImage *save_default_icon;
  GtkImage *save_working_icon;
  GtkImage *save_success_icon;
  GtkButton *copy_button;
  GtkMenuButton *app_menu_button;
  GtkWindowControls *end_window_controls;
  GListModel *end_window_controls_children;
  GtkStack *copy_icon_stack;
  GtkImage *copy_default_icon;
  GtkImage *copy_success_icon;
  GtkWidget *zoom_group;
  GtkButton *zoom_out_button;
  GtkButton *zoom_in_button;
  GtkButton *fit_zoom_button;
  GtkWidget *settings_group;
  GtkWidget *crop_controls;
  GtkLabel *crop_dimensions_label;
  GtkButton *crop_cancel_button;
  GtkButton *crop_apply_button;
  GtkColorDialogButton *color_button;
  GtkColorDialogButton *fill_color_button;
  GtkWidget *size_segment_box;
  GtkToggleButton *small_size_button;
  GtkToggleButton *medium_size_button;
  GtkToggleButton *large_size_button;
  GtkLabel *size_value_label;
  GtkDropDown *blur_type_dropdown;
  GtkCssProvider *window_css_provider;
  GtkCssProvider *widget_css_provider;
  char *shortcut_accels[SWASH_SHORTCUT_COUNT];

  GFile *current_file;
  char *source_name;
  GdkTexture *texture;
  cairo_surface_t *image_surface;
  cairo_surface_t *annotation_cache;
  int annotation_cache_width;
  int annotation_cache_height;
  guint annotation_cache_image_generation;
  guint annotation_cache_generation;
  gboolean annotation_cache_allow_marker_overlap;
  SwashStroke *annotation_cache_excluded;
  GPtrArray *active_stroke_nodes;
  guint active_stroke_baked_points;
  SwashStroke *active_stroke_baked_stroke;
  double active_stroke_bake_dx;
  double active_stroke_bake_dy;
  double active_stroke_bake_sx;
  double active_stroke_bake_sy;
  SwashDocument *document;
  SwashStroke *current_stroke;
  double zoom;
  gboolean fit_mode;
  SwashTool active_tool;
  gboolean drawing;
  guint copy_feedback_timeout_id;
  guint save_spinner_timeout_id;
  guint save_feedback_timeout_id;
  gint64 save_feedback_started_at;
  guint auto_copy_throttle_id;
  gint64 auto_copy_last_claim;
  gint64 clipboard_claim_cost;
  gboolean auto_copy_pending;
  guint ocr_generation;
  gboolean ocr_running;
  double last_draw_x;
  double last_draw_y;
  double drag_start_hvalue;
  double drag_start_vvalue;
  double pinch_start_zoom;
  double pinch_anchor_rel_x;
  double pinch_anchor_rel_y;
  double crop_start_x;
  double crop_start_y;
  double crop_end_x;
  double crop_end_y;
  double crop_drag_x;
  double crop_drag_y;
  double crop_original_left;
  double crop_original_top;
  double crop_original_right;
  double crop_original_bottom;
  guint crop_drag_mode;
  gboolean crop_selection_active;

  double pointer_x;
  double pointer_y;
  double pointer_widget_x;
  double pointer_widget_y;
  gboolean pointer_in;
  SwashWindowBackgroundMode window_background_mode;
  gboolean updating_ui;
  gboolean interaction_has_undo_step;
  SwashStroke *selected_stroke;
  double move_start_x;
  double move_start_y;
  gboolean move_stroke_moved;
  gboolean text_editing;
  GtkWidget *text_input_hidden;
  guint text_cursor_blink_id;
  gboolean text_cursor_visible;
  GdkEventSequence *active_touch_draw_sequence;
  GdkEventSequence *cancelled_touch_draw_sequence;
  GHashTable *active_touch_sequences;
  GHashTable *touch_tap_points;
  gboolean touch_tap_candidate;
  gboolean touch_tap_cancelled;
  guint touch_tap_max_points;
  gint64 touch_tap_started_at;
  gboolean esc_closes_window;
  gboolean close_after_copy;
  gboolean close_after_save;
  GdkModifierType angle_snap_modifiers;
  gboolean allow_highlighter_overlap;
  gboolean floating_controls_blur;
  gboolean auto_copy_latest_change;
  gboolean remember_tool_sizes;
  double window_background_opacity;
  double floating_controls_opacity;
  double tool_widths[SWASH_TOOL_MOVE + 1];
  GdkRGBA tool_colors[SWASH_TOOL_MOVE + 1];
  GdkRGBA tool_fill_colors[SWASH_TOOL_MOVE + 1];
  int blur_type;
  GPtrArray *ocr_lines;
  SwashOcrLine *selected_ocr_line;
  char *ocr_all_text;
};

GPtrArray *swash_window_strokes(SwashWindow *self);
void swash_window_update_ocr_overlay(SwashWindow *self);
void swash_window_maybe_start_ocr(SwashWindow *self);
void swash_window_update_ocr_panel(SwashWindow *self);
void swash_window_reset_save_button(SwashWindow *self);
void swash_window_update_history_buttons(SwashWindow *self);
void swash_window_clear_history(SwashWindow *self);
void swash_window_record_undo_step(SwashWindow *self);
void swash_window_maybe_auto_copy_latest_change(SwashWindow *self);
void swash_window_trigger_copy(SwashWindow *self,
                                  gboolean        user_initiated);
gboolean swash_window_shortcut_matches(SwashWindow   *self,
                                          SwashShortcut shortcut,
                                          guint          keyval,
                                          GdkModifierType state);
void swash_window_restore_strokes(SwashWindow *self,
                                     GPtrArray      *strokes);

gboolean swash_window_get_display_rect(SwashWindow *self,
                                          double          widget_width,
                                          double          widget_height,
                                          double         *display_x,
                                          double         *display_y,
                                          double         *display_width,
                                          double         *display_height);
gboolean swash_window_get_image_point(SwashWindow *self,
                                         double          widget_x,
                                         double          widget_y,
                                         gboolean        clamp_to_image,
                                         double         *image_x,
                                         double         *image_y);
void swash_window_set_adjustment_clamped(GtkAdjustment *adjustment,
                                            double         value);
gboolean swash_window_get_pointer_viewport_position(SwashWindow *self,
                                                       double         *x,
                                                       double         *y);
void swash_window_get_viewport_center(SwashWindow *self,
                                         double         *x,
                                         double         *y);
double swash_window_get_effective_zoom(SwashWindow *self);
void swash_window_apply_zoom_mode(SwashWindow *self);
void swash_window_update_zoom_label(SwashWindow *self);
void swash_window_update_picture_size(SwashWindow *self);
void swash_window_refresh_document_state(SwashWindow *self);
cairo_surface_t *swash_window_render_composited_surface(SwashWindow *self);
void swash_window_apply_crop(SwashWindow *self,
                                int             left,
                                int             top,
                                int             width,
                                int             height);
void swash_window_apply_pending_crop(SwashWindow *self);
void swash_window_cancel_pending_crop(SwashWindow *self);
void swash_window_update_crop_controls(SwashWindow *self);
void swash_window_set_zoom_at(SwashWindow *self,
                                 double          zoom,
                                 double          viewport_x,
                                 double          viewport_y);
void swash_window_update_size_controls(SwashWindow *self);
void swash_window_adjust_tool_size(SwashWindow *self,
                                      double          delta);
void swash_window_update_tool_ui(SwashWindow *self);
void swash_window_text_editing_commit(SwashWindow *self);
void swash_window_queue_fit_zoom(SwashWindow *self);
void swash_window_sync_state(SwashWindow *self);
void swash_window_install_history_actions(GtkWidgetClass *widget_class);
void swash_window_setup_tool_signals(SwashWindow *self);
void swash_window_install_canvas_actions(GtkWidgetClass *widget_class);
void swash_window_setup_controllers(SwashWindow *self);
void swash_window_setup_signals(SwashWindow *self);
void swash_window_drawing_area_draw(GtkDrawingArea *area,
                                       cairo_t        *cr,
                                       int             width,
                                       int             height,
                                       gpointer        user_data);
GtkWidget *swash_stroke_overlay_new(SwashWindow *self);
void swash_window_update_active_stroke_overlay(SwashWindow *self);
void swash_window_reset_active_stroke_nodes(SwashWindow *self);
void swash_window_annotation_cache_append_stroke(SwashWindow *self,
                                                 SwashStroke *stroke);
void swash_window_update_canvas_cursor(SwashWindow *self);
