#include "export.h"

#include "render.h"

static const char *
swash_export_format_from_path(const char *path)
{
  const char *dot = strrchr(path, '.');

  if (dot == NULL)
    return "png";

  dot++;
  if (g_ascii_strcasecmp(dot, "jpg") == 0 || g_ascii_strcasecmp(dot, "jpeg") == 0)
    return "jpeg";
  if (g_ascii_strcasecmp(dot, "png") == 0)
    return "png";
  if (g_ascii_strcasecmp(dot, "webp") == 0)
    return "webp";
  if (g_ascii_strcasecmp(dot, "bmp") == 0)
    return "bmp";
  if (g_ascii_strcasecmp(dot, "tif") == 0 || g_ascii_strcasecmp(dot, "tiff") == 0)
    return "tiff";

  return "png";
}

static const char *
swash_export_copy_mime_type(const char *format)
{
  return g_strcmp0(format, "jpeg") == 0 ? "image/jpeg" : "image/png";
}

/* Clipboard copies are encoded on the main thread so that the selection is
 * claimed while the window still has keyboard focus, so they trade a little
 * size for latency. gdk-pixbuf switches PNG filtering strategy above level 2,
 * which costs an order of magnitude in time (4K photo content: 62 ms at level
 * 2 versus 815 ms at level 9) for under 1% in size. Saved files keep level 9,
 * where the encode is off the interaction path. */
#define SWASH_EXPORT_PNG_COMPRESSION "9"
#define SWASH_EXPORT_COPY_PNG_COMPRESSION "2"

static void
swash_export_options(const char  *format,
                        char       **option_keys,
                        char       **option_values,
                        gboolean     for_copy)
{
  option_keys[0] = NULL;
  option_values[0] = NULL;

  if (g_strcmp0(format, "jpeg") == 0) {
    option_keys[0] = "quality";
    option_values[0] = "92";
    return;
  }

  if (g_strcmp0(format, "png") == 0) {
    option_keys[0] = "compression";
    option_values[0] = for_copy ? SWASH_EXPORT_COPY_PNG_COMPRESSION
                                : SWASH_EXPORT_PNG_COMPRESSION;
  }
}

static guchar
swash_export_unpremultiply(guchar color,
                           guchar alpha)
{
  if (alpha == 0)
    return 0;

  return MIN(255, (color * 255 + alpha / 2) / alpha);
}

static GdkPixbuf *
swash_export_pixbuf_from_surface(cairo_surface_t *surface)
{
  GdkPixbuf *pixbuf;
  const guchar *src_pixels;
  guchar *dst_pixels;
  int width;
  int height;
  int src_stride;
  int y;

  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS
      || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE
      || cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)
    return NULL;

  width = cairo_image_surface_get_width(surface);
  height = cairo_image_surface_get_height(surface);
  src_stride = cairo_image_surface_get_stride(surface);
  pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
  if (pixbuf == NULL)
    return NULL;

  cairo_surface_flush(surface);
  src_pixels = cairo_image_surface_get_data(surface);
  dst_pixels = gdk_pixbuf_get_pixels(pixbuf);

  for (y = 0; y < height; y++) {
    const guint32 *src = (const guint32 *) (src_pixels + y * src_stride);
    guchar *dst = dst_pixels + y * gdk_pixbuf_get_rowstride(pixbuf);
    int x;

    for (x = 0; x < width; x++) {
      const guint32 pixel = src[x];
      const guchar alpha = pixel >> 24;

      /* Screenshots are opaque almost everywhere, and premultiplied values
       * are already the final ones at full alpha: skipping the division there
       * takes this loop from 44 ms to 11 ms on a 12 MP image. */
      if (alpha == 0xff) {
        dst[x * 4] = (pixel >> 16) & 0xff;
        dst[x * 4 + 1] = (pixel >> 8) & 0xff;
        dst[x * 4 + 2] = pixel & 0xff;
        dst[x * 4 + 3] = 0xff;
        continue;
      }

      dst[x * 4] = swash_export_unpremultiply((pixel >> 16) & 0xff, alpha);
      dst[x * 4 + 1] = swash_export_unpremultiply((pixel >> 8) & 0xff, alpha);
      dst[x * 4 + 2] = swash_export_unpremultiply(pixel & 0xff, alpha);
      dst[x * 4 + 3] = alpha;
    }
  }

  return pixbuf;
}

static GdkPixbuf *
swash_export_prepare_pixbuf_for_format(GdkPixbuf  *pixbuf,
                                          const char *format)
{
  GdkPixbuf *flattened;
  const guchar *src_pixels;
  guchar *dst_pixels;
  const int width = gdk_pixbuf_get_width(pixbuf);
  const int height = gdk_pixbuf_get_height(pixbuf);
  const int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
  int y;

  if (g_strcmp0(format, "jpeg") != 0 || !gdk_pixbuf_get_has_alpha(pixbuf))
    return g_object_ref(pixbuf);

  flattened = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
  if (flattened == NULL)
    return NULL;

  src_pixels = gdk_pixbuf_get_pixels(pixbuf);
  dst_pixels = gdk_pixbuf_get_pixels(flattened);

  for (y = 0; y < height; y++) {
    const guchar *src = src_pixels + y * src_stride;
    guchar *dst = dst_pixels + y * gdk_pixbuf_get_rowstride(flattened);
    int x;

    for (x = 0; x < width; x++) {
      dst[x * 3] = src[x * 4];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 2];
    }
  }
  return flattened;
}

SwashExportRequest *
swash_export_request_new(GdkTexture              *texture,
                            GPtrArray               *strokes,
                            SwashExportKind       kind,
                            GFile                   *file,
                             const char              *copy_format,
                             SwashStrokeCopyFunc   copy_stroke,
                             GDestroyNotify           stroke_free,
                             gboolean                 allow_marker_overlap,
                             SwashStrokeRenderFunc render_stroke,
                             guint                    image_generation,
                             GError                 **error)
{
  SwashExportRequest *request;
  const int width = texture != NULL ? gdk_texture_get_width(texture) : 0;
  const int height = texture != NULL ? gdk_texture_get_height(texture) : 0;
  guint i;

  if (width <= 0 || height <= 0) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "Could not render the current image");
    return NULL;
  }

  request = g_new0(SwashExportRequest, 1);
  request->kind = kind;
  request->width = width;
  request->height = height;
  request->stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
  request->pixels = g_malloc(request->stride * height);
  request->strokes = g_ptr_array_new_with_free_func(stroke_free);
  request->allow_marker_overlap = allow_marker_overlap;
  request->render_stroke = render_stroke;
  request->image_generation = image_generation;

  gdk_texture_download(texture, request->pixels, request->stride);

  if (strokes != NULL) {
    for (i = 0; i < strokes->len; i++)
      g_ptr_array_add(request->strokes, copy_stroke(g_ptr_array_index(strokes, i)));
  }

  if (file != NULL)
    request->file = g_object_ref(file);

  if (kind == SWASH_EXPORT_COPY)
    request->copy_format = g_strdup(copy_format != NULL ? copy_format : "png");

  return request;
}

void
swash_export_request_free(SwashExportRequest *request)
{
  if (request == NULL)
    return;

  g_clear_pointer(&request->pixels, g_free);
  g_clear_pointer(&request->strokes, g_ptr_array_unref);
  g_clear_object(&request->file);
  g_clear_pointer(&request->copy_format, g_free);
  g_free(request);
}

void
swash_copy_result_free(SwashCopyResult *result)
{
  if (result == NULL)
    return;

  g_clear_pointer(&result->bytes, g_bytes_unref);
  g_clear_object(&result->texture);
  g_free(result);
}

static cairo_surface_t *
swash_export_render_surface(SwashExportRequest *request)
{
  cairo_surface_t *surface;
  cairo_t *cr;

  surface = cairo_image_surface_create_for_data(request->pixels,
                                                CAIRO_FORMAT_ARGB32,
                                                request->width,
                                                request->height,
                                                request->stride);
  cr = cairo_create(surface);
  swash_render_strokes(cr,
                          request->strokes,
                          surface,
                          request->allow_marker_overlap,
                          request->render_stroke,
                          request->image_generation);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  return surface;
}

SwashCopyResult *
swash_export_render_copy(SwashExportRequest *request,
                            GError            **error)
{
  g_autoptr(GdkPixbuf) pixbuf = NULL;
  g_autoptr(GdkPixbuf) encoded_pixbuf = NULL;
  g_autoptr(GBytes) texture_bytes = NULL;
  cairo_surface_t *surface;
  char *buffer = NULL;
  gsize length = 0;
  char *option_keys[] = { NULL, NULL };
  char *option_values[] = { NULL, NULL };
  SwashCopyResult *result;

  g_return_val_if_fail(request != NULL, NULL);
  g_return_val_if_fail(request->kind == SWASH_EXPORT_COPY, NULL);

  surface = swash_export_render_surface(request);
  pixbuf = swash_export_pixbuf_from_surface(surface);
  cairo_surface_destroy(surface);

  if (pixbuf == NULL) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "Could not encode the current image");
    return NULL;
  }

  result = g_new0(SwashCopyResult, 1);
  /* The rendered pixels are handed to the texture rather than copied: the
   * request is spent once the copy has been rendered. */
  texture_bytes = g_bytes_new_take(g_steal_pointer(&request->pixels),
                                   request->stride * request->height);
  result->texture = gdk_memory_texture_new(request->width,
                                           request->height,
                                           GDK_MEMORY_DEFAULT,
                                           texture_bytes,
                                           request->stride);

  swash_export_options(request->copy_format, option_keys, option_values, TRUE);
  encoded_pixbuf = swash_export_prepare_pixbuf_for_format(pixbuf, request->copy_format);
  if (encoded_pixbuf == NULL) {
    swash_copy_result_free(result);
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "Could not prepare the current image for export");
    return NULL;
  }

  if (!gdk_pixbuf_save_to_bufferv(encoded_pixbuf,
                                  &buffer,
                                  &length,
                                  request->copy_format,
                                  option_keys,
                                  option_values,
                                  error)) {
    swash_copy_result_free(result);
    return NULL;
  }

  result->mime_type = swash_export_copy_mime_type(request->copy_format);
  result->bytes = g_bytes_new_take(buffer, length);
  return result;
}

void
swash_export_run_task(GTask        *task,
                         gpointer      source_object,
                         gpointer      task_data,
                         GCancellable *cancellable)
{
  SwashExportRequest *request = task_data;
  cairo_surface_t *surface;

  (void) source_object;
  (void) cancellable;

  g_return_if_fail(request->kind == SWASH_EXPORT_SAVE);

  surface = swash_export_render_surface(request);

  g_autofree char *path = g_file_get_path(request->file);
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkPixbuf) pixbuf = NULL;
  g_autoptr(GdkPixbuf) encoded_pixbuf = NULL;
  char *option_keys[] = { NULL, NULL };
  char *option_values[] = { NULL, NULL };
  const char *format;

  if (path == NULL) {
    cairo_surface_destroy(surface);
    g_task_return_new_error(task,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "Saving to non-local files is not supported yet");
    return;
  }

  pixbuf = swash_export_pixbuf_from_surface(surface);
  cairo_surface_destroy(surface);

  if (pixbuf == NULL) {
    g_task_return_new_error(task,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Could not encode the current image");
    return;
  }

  format = swash_export_format_from_path(path);
  swash_export_options(format, option_keys, option_values, FALSE);
  encoded_pixbuf = swash_export_prepare_pixbuf_for_format(pixbuf, format);

  if (encoded_pixbuf == NULL) {
    g_task_return_new_error(task,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Could not prepare the current image for export");
    return;
  }

  if (option_keys[0] != NULL) {
    if (!gdk_pixbuf_savev(encoded_pixbuf,
                          path,
                          format,
                          option_keys,
                          option_values,
                          &error)) {
      g_task_return_error(task, g_steal_pointer(&error));
      return;
    }
  } else if (!gdk_pixbuf_save(encoded_pixbuf,
                              path,
                              format,
                              &error,
                              NULL)) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  g_task_return_boolean(task, TRUE);
}
