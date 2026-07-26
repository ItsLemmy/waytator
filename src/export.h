#pragma once

#include "types.h"

SwashExportRequest *swash_export_request_new(GdkTexture            *texture,
                                                   GPtrArray             *strokes,
                                                   SwashExportKind     kind,
                                                   GFile                 *file,
                                                   const char            *copy_format,
                                                   SwashStrokeCopyFunc copy_stroke,
                                                   GDestroyNotify         stroke_free,
                                                   gboolean               allow_marker_overlap,
                                                   SwashStrokeRenderFunc render_stroke,
                                                   guint                  image_generation,
                                                   GError               **error);
void swash_export_request_free(SwashExportRequest *request);

/* Renders a SWASH_EXPORT_COPY request on the calling thread and consumes its
 * pixel buffer. Clipboard claims run on the main thread so that the selection
 * is taken while the window still holds keyboard focus. */
SwashCopyResult *swash_export_render_copy(SwashExportRequest *request,
                                             GError            **error);

void swash_copy_result_free(SwashCopyResult *result);
void swash_export_run_task(GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
                              GCancellable *cancellable);
