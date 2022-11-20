/*
 * Copyright (C) 2018 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 */

#include "config.h"

#include "grd-vnc-pipewire-stream.h"

#include <drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/param/format-utils.h>
#include <spa/utils/result.h>
#include <sys/mman.h>

#include "grd-context.h"
#include "grd-egl-thread.h"
#include "grd-pipewire-utils.h"
#include "grd-vnc-cursor.h"

enum
{
  CLOSED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct _GrdVncFrame GrdVncFrame;

typedef void (* GrdVncFrameReadyCallback) (GrdVncPipeWireStream *stream,
                                           GrdVncFrame          *frame,
                                           gboolean              success,
                                           gpointer              user_data);

struct _GrdVncFrame
{
  gatomicrefcount refcount;

  void *data;
  rfbCursorPtr rfb_cursor;
  gboolean cursor_moved;
  int cursor_x;
  int cursor_y;

  GrdVncPipeWireStream *stream;
  GrdVncFrameReadyCallback callback;
  gpointer callback_user_data;
};

struct _GrdVncPipeWireStream
{
  GObject parent;

  GrdSessionVnc *session;

  GSource *pipewire_source;
  struct pw_context *pipewire_context;
  struct pw_core *pipewire_core;

  struct spa_hook pipewire_core_listener;

  GMutex frame_mutex;
  GrdVncFrame *pending_frame;
  GSource *pending_frame_source;

  struct pw_stream *pipewire_stream;
  struct spa_hook pipewire_stream_listener;

  uint32_t src_node_id;

  struct spa_video_info_raw spa_format;
};

G_DEFINE_TYPE (GrdVncPipeWireStream, grd_vnc_pipewire_stream,
               G_TYPE_OBJECT)

static void grd_vnc_frame_unref (GrdVncFrame *frame);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdVncFrame, grd_vnc_frame_unref)

static gboolean
pipewire_loop_source_prepare (GSource *base,
                              int     *timeout)
{
  *timeout = -1;
  return FALSE;
}

static gboolean
pipewire_loop_source_dispatch (GSource     *source,
                               GSourceFunc  callback,
                               gpointer     user_data)
{
  GrdPipeWireSource *pipewire_source = (GrdPipeWireSource *) source;
  int result;

  result = pw_loop_iterate (pipewire_source->pipewire_loop, 0);
  if (result < 0)
    g_warning ("pipewire_loop_iterate failed: %s", spa_strerror (result));

  return TRUE;
}

static void
pipewire_loop_source_finalize (GSource *source)
{
  GrdPipeWireSource *pipewire_source = (GrdPipeWireSource *) source;

  pw_loop_leave (pipewire_source->pipewire_loop);
  pw_loop_destroy (pipewire_source->pipewire_loop);
}

static GSourceFuncs pipewire_source_funcs =
{
  pipewire_loop_source_prepare,
  NULL,
  pipewire_loop_source_dispatch,
  pipewire_loop_source_finalize
};

static GrdPipeWireSource *
create_pipewire_source (void)
{
  GrdPipeWireSource *pipewire_source;

  pipewire_source =
    (GrdPipeWireSource *) g_source_new (&pipewire_source_funcs,
                                        sizeof (GrdPipeWireSource));
  pipewire_source->pipewire_loop = pw_loop_new (NULL);
  if (!pipewire_source->pipewire_loop)
    {
      g_source_destroy ((GSource *) pipewire_source);
      return NULL;
    }

  g_source_add_unix_fd (&pipewire_source->base,
                        pw_loop_get_fd (pipewire_source->pipewire_loop),
                        G_IO_IN | G_IO_ERR);

  pw_loop_enter (pipewire_source->pipewire_loop);
  g_source_attach (&pipewire_source->base, NULL);

  return pipewire_source;
}

static void
on_stream_state_changed (void                 *user_data,
                         enum pw_stream_state  old,
                         enum pw_stream_state  state,
                         const char           *error)
{
  g_debug ("Pipewire stream state changed from %s to %s",
           pw_stream_state_as_string (old),
           pw_stream_state_as_string (state));

  switch (state)
    {
    case PW_STREAM_STATE_ERROR:
      g_warning ("PipeWire stream error: %s", error);
      break;
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_STREAMING:
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_CONNECTING:
      break;
    }
}

static void
on_stream_param_changed (void                 *user_data,
                         uint32_t              id,
                         const struct spa_pod *format)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (user_data);
  GrdSession *session = GRD_SESSION (stream->session);
  GrdContext *context = grd_session_get_context (session);
  uint8_t params_buffer[1024];
  struct spa_pod_builder pod_builder;
  int width;
  int height;
  enum spa_data_type allowed_buffer_types;
  const struct spa_pod *params[3];

  if (grd_session_vnc_is_client_gone (stream->session))
    return;

  if (!format || id != SPA_PARAM_Format)
    return;

  spa_format_video_raw_parse (format, &stream->spa_format);

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));

  width = stream->spa_format.size.width;
  height = stream->spa_format.size.height;

  grd_session_vnc_queue_resize_framebuffer (stream->session, width, height);

  allowed_buffer_types = 1 << SPA_DATA_MemFd;
  if (grd_context_get_egl_thread (context))
    allowed_buffer_types |= 1 << SPA_DATA_DmaBuf;

  params[0] = spa_pod_builder_add_object (
    &pod_builder,
    SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int (8, 1, 8),
    SPA_PARAM_BUFFERS_dataType, SPA_POD_Int (allowed_buffer_types),
    0);

  params[1] = spa_pod_builder_add_object (
    &pod_builder,
    SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
    SPA_PARAM_META_type, SPA_POD_Id (SPA_META_Header),
    SPA_PARAM_META_size, SPA_POD_Int (sizeof (struct spa_meta_header)),
    0);

  params[2] = spa_pod_builder_add_object(
    &pod_builder,
    SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
    SPA_PARAM_META_type, SPA_POD_Id (SPA_META_Cursor),
    SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int (CURSOR_META_SIZE (384, 384),
                                                   CURSOR_META_SIZE (1,1),
                                                   CURSOR_META_SIZE (384, 384)),
    0);

  pw_stream_update_params (stream->pipewire_stream,
                           params, G_N_ELEMENTS (params));
}

static GrdVncFrame *
grd_vnc_frame_new (GrdVncPipeWireStream     *stream,
                   GrdVncFrameReadyCallback  callback,
                   gpointer                  callback_user_data)
{
  GrdVncFrame *frame;

  frame = g_new0 (GrdVncFrame, 1);

  g_atomic_ref_count_init (&frame->refcount);
  frame->stream = stream;
  frame->callback = callback;
  frame->callback_user_data = callback_user_data;

  return frame;
}

static GrdVncFrame *
grd_vnc_frame_ref (GrdVncFrame *frame)
{
  g_atomic_ref_count_inc (&frame->refcount);
  return frame;
}

static void
grd_vnc_frame_unref (GrdVncFrame *frame)
{
  if (g_atomic_ref_count_dec (&frame->refcount))
    {
      g_free (frame->data);
      g_clear_pointer (&frame->rfb_cursor, rfbFreeCursor);
      g_free (frame);
    }
}

static gboolean
do_render (gpointer user_data)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (user_data);
  GrdVncFrame *frame;

  g_mutex_lock (&stream->frame_mutex);
  frame = g_steal_pointer (&stream->pending_frame);
  g_mutex_unlock (&stream->frame_mutex);

  if (!frame)
    return G_SOURCE_CONTINUE;

  if (grd_session_vnc_is_client_gone (stream->session))
    {
      grd_vnc_frame_unref (frame);
      return G_SOURCE_CONTINUE;
    }

  if (frame->rfb_cursor)
    {
      grd_session_vnc_set_cursor (stream->session,
                                  g_steal_pointer (&frame->rfb_cursor));
    }

  if (frame->cursor_moved)
    {
      grd_session_vnc_move_cursor (stream->session,
                                   frame->cursor_x,
                                   frame->cursor_y);
    }

  if (frame->data)
    {
      grd_session_vnc_take_buffer (stream->session,
                                   g_steal_pointer (&frame->data));
    }
  else
    {
      grd_session_vnc_flush (stream->session);
    }

  grd_vnc_frame_unref (frame);

  return G_SOURCE_CONTINUE;
}

static void
process_mouse_pointer_bitmap (GrdVncPipeWireStream *stream,
                              struct spa_buffer    *buffer,
                              GrdVncFrame          *frame)
{
  struct spa_meta_cursor *spa_meta_cursor;
  struct spa_meta_bitmap *spa_meta_bitmap;
  GrdPixelFormat format;

  spa_meta_cursor = spa_buffer_find_meta_data (buffer, SPA_META_Cursor,
                                               sizeof *spa_meta_cursor);

  g_assert (spa_meta_cursor);
  g_assert (spa_meta_cursor_is_valid (spa_meta_cursor));
  g_assert (spa_meta_cursor->bitmap_offset);

  spa_meta_bitmap = SPA_MEMBER (spa_meta_cursor,
                                spa_meta_cursor->bitmap_offset,
                                struct spa_meta_bitmap);

  if (spa_meta_bitmap &&
      spa_meta_bitmap->size.width > 0 &&
      spa_meta_bitmap->size.height > 0 &&
      grd_spa_pixel_format_to_grd_pixel_format (spa_meta_bitmap->format,
                                                &format))
    {
      uint8_t *buf;
      rfbCursorPtr rfb_cursor;

      buf = SPA_MEMBER (spa_meta_bitmap, spa_meta_bitmap->offset, uint8_t);
      rfb_cursor = grd_vnc_create_cursor (spa_meta_bitmap->size.width,
                                          spa_meta_bitmap->size.height,
                                          spa_meta_bitmap->stride,
                                          format,
                                          buf);
      rfb_cursor->xhot = spa_meta_cursor->hotspot.x;
      rfb_cursor->yhot = spa_meta_cursor->hotspot.y;

      frame->rfb_cursor = rfb_cursor;
    }
  else if (spa_meta_bitmap)
    {
      frame->rfb_cursor = grd_vnc_create_empty_cursor (1, 1);
    }
}

static void
copy_frame_data (GrdVncFrame *frame,
                 uint8_t     *src_data,
                 int          width,
                 int          height,
                 int          dst_stride,
                 int          src_stride,
                 int          bpp)
{
  int y;

  frame->data = g_malloc (height * dst_stride);
  for (y = 0; y < height; y++)
    {
      memcpy (((uint8_t *) frame->data) + y * dst_stride,
              ((uint8_t *) src_data) + y * src_stride,
              width * bpp);
    }
}

static void
on_dma_buf_downloaded (gboolean success,
                       gpointer user_data)
{
  GrdVncFrame *frame = user_data;

  frame->callback (frame->stream,
                   frame,
                   success,
                   frame->callback_user_data);
}

static void
process_frame_data (GrdVncPipeWireStream *stream,
                    struct spa_buffer    *buffer,
                    GrdVncFrame          *frame)
{
  GrdVncFrameReadyCallback callback = frame->callback;
  gpointer user_data = frame->callback_user_data;
  int dst_stride;
  uint32_t drm_format;
  int bpp;
  int width;
  int height;

  g_assert (buffer->datas[0].chunk->size > 0);

  height = stream->spa_format.size.height;
  width = stream->spa_format.size.width;
  dst_stride = grd_session_vnc_get_stride_for_width (stream->session,
                                                     width);
  grd_get_spa_format_details (stream->spa_format.format,
                              &drm_format, &bpp);

  if (buffer->datas[0].type == SPA_DATA_MemFd)
    {
      size_t size;
      uint8_t *map;
      int src_stride;
      uint8_t *src_data;

      size = buffer->datas[0].maxsize + buffer->datas[0].mapoffset;
      map = mmap (NULL, size, PROT_READ, MAP_PRIVATE, buffer->datas[0].fd, 0);
      if (map == MAP_FAILED)
        {
          g_warning ("Failed to mmap buffer: %s", g_strerror (errno));
          callback (stream, NULL, FALSE, user_data);
          return;
        }

      src_data = SPA_MEMBER (map, buffer->datas[0].mapoffset, uint8_t);
      src_stride = buffer->datas[0].chunk->stride;
      copy_frame_data (frame, src_data,
                       width, height,
                       dst_stride, src_stride,
                       bpp);

      munmap (map, size);

      callback (stream, g_steal_pointer (&frame), TRUE, user_data);
    }
  else if (buffer->datas[0].type == SPA_DATA_DmaBuf)
    {
      GrdSession *session = GRD_SESSION (stream->session);
      GrdContext *context = grd_session_get_context (session);
      GrdEglThread *egl_thread = grd_context_get_egl_thread (context);
      int row_width;
      int *fds;
      uint32_t *offsets;
      uint32_t *strides;
      uint64_t *modifiers = NULL;
      uint32_t n_planes;
      unsigned int i;
      uint8_t *dst_data;

      row_width = dst_stride / bpp;

      n_planes = buffer->n_datas;
      fds = g_alloca (sizeof (int) * n_planes);
      offsets = g_alloca (sizeof (uint32_t) * n_planes);
      strides = g_alloca (sizeof (uint32_t) * n_planes);
      if (stream->spa_format.modifier != DRM_FORMAT_MOD_INVALID)
        modifiers = g_alloca (sizeof (uint64_t) * n_planes);

      for (i = 0; i < n_planes; i++)
        {
          fds[i] = buffer->datas[i].fd;
          offsets[i] = buffer->datas[i].chunk->offset;
          strides[i] = buffer->datas[i].chunk->stride;
          if (modifiers)
            modifiers[i] = stream->spa_format.modifier;
        }
      dst_data = g_malloc0 (height * dst_stride);

      frame->data = dst_data;
      grd_egl_thread_download (egl_thread,
                               0, 0, 0,
                               NULL, NULL, NULL,
                               dst_data,
                               row_width,
                               drm_format,
                               width, height,
                               n_planes,
                               fds,
                               strides,
                               offsets,
                               modifiers,
                               on_dma_buf_downloaded,
                               grd_vnc_frame_ref (g_steal_pointer (&frame)),
                               (GDestroyNotify) grd_vnc_frame_unref);
    }
  else if (buffer->datas[0].type == SPA_DATA_MemPtr)
    {
      uint8_t *src_data;
      int src_stride;

      src_data = buffer->datas[0].data;

      src_stride = buffer->datas[0].chunk->stride;
      copy_frame_data (frame, src_data,
                       width, height,
                       dst_stride, src_stride,
                       bpp);

      callback (stream, g_steal_pointer (&frame), TRUE, user_data);
    }
  else
    {
      g_assert_not_reached ();
    }
}

static gboolean
pending_frame_source_dispatch (GSource     *source,
                               GSourceFunc  callback,
                               gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs pending_frame_source_funcs =
{
  .dispatch = pending_frame_source_dispatch,
};

static void
on_frame_ready (GrdVncPipeWireStream *stream,
                GrdVncFrame          *frame,
                gboolean              success,
                gpointer              user_data)
{
  GrdVncFrame *pending_frame;
  struct pw_buffer *buffer = user_data;

  g_assert (frame);

  if (!success)
    goto out;

  g_mutex_lock (&stream->frame_mutex);

  pending_frame = g_steal_pointer (&stream->pending_frame);
  if (pending_frame)
    {
      if (!frame->data && pending_frame->data)
        frame->data = g_steal_pointer (&pending_frame->data);
      if (!frame->rfb_cursor && pending_frame->rfb_cursor)
        frame->rfb_cursor = g_steal_pointer (&pending_frame->rfb_cursor);
      if (!frame->cursor_moved && pending_frame->cursor_moved)
        {
          frame->cursor_x = pending_frame->cursor_x;
          frame->cursor_y = pending_frame->cursor_y;
          frame->cursor_moved = TRUE;
        }

      grd_vnc_frame_unref (pending_frame);
    }

  stream->pending_frame = g_steal_pointer (&frame);

  g_mutex_unlock (&stream->frame_mutex);

out:
  if (buffer)
    pw_stream_queue_buffer (stream->pipewire_stream, buffer);

  g_source_set_ready_time (stream->pending_frame_source, 0);

  g_clear_pointer (&frame, grd_vnc_frame_unref);
}

static void
maybe_consume_pointer_position (struct pw_buffer *buffer,
                                gboolean         *cursor_moved,
                                int              *cursor_x,
                                int              *cursor_y)
{
  struct spa_meta_cursor *spa_meta_cursor;

  spa_meta_cursor = spa_buffer_find_meta_data (buffer->buffer, SPA_META_Cursor,
                                               sizeof *spa_meta_cursor);
  if (spa_meta_cursor && spa_meta_cursor_is_valid (spa_meta_cursor))
    {
      *cursor_x = spa_meta_cursor->position.x;
      *cursor_y = spa_meta_cursor->position.y;
      *cursor_moved = TRUE;
    }
}

static void
on_stream_process (void *user_data)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (user_data);
  g_autoptr (GrdVncFrame) frame = NULL;
  struct pw_buffer *last_pointer_buffer = NULL;
  struct pw_buffer *last_frame_buffer = NULL;
  struct pw_buffer *next_buffer;
  gboolean cursor_moved = FALSE;
  int cursor_x = 0;
  int cursor_y = 0;

  while ((next_buffer = pw_stream_dequeue_buffer (stream->pipewire_stream)))
    {
      maybe_consume_pointer_position (next_buffer, &cursor_moved,
                                      &cursor_x, &cursor_y);

      if (grd_pipewire_buffer_has_pointer_bitmap (next_buffer))
        {
          if (last_pointer_buffer == last_frame_buffer)
            last_pointer_buffer = NULL;

          if (last_pointer_buffer)
            pw_stream_queue_buffer (stream->pipewire_stream, last_pointer_buffer);
          last_pointer_buffer = next_buffer;
        }
      if (grd_pipewire_buffer_has_frame_data (next_buffer))
        {
          if (last_pointer_buffer == last_frame_buffer)
            last_frame_buffer = NULL;

          if (last_frame_buffer)
            pw_stream_queue_buffer (stream->pipewire_stream, last_frame_buffer);
          last_frame_buffer = next_buffer;
        }

      if (next_buffer != last_pointer_buffer &&
          next_buffer != last_frame_buffer)
        pw_stream_queue_buffer (stream->pipewire_stream, next_buffer);
    }
  if (!last_pointer_buffer && !last_frame_buffer && !cursor_moved)
    return;

  frame = grd_vnc_frame_new (stream, on_frame_ready, last_frame_buffer);
  frame->cursor_moved = cursor_moved;
  frame->cursor_x = cursor_x;
  frame->cursor_y = cursor_y;

  if (last_pointer_buffer)
    {
      process_mouse_pointer_bitmap (stream, last_pointer_buffer->buffer, frame);
      if (last_pointer_buffer != last_frame_buffer)
        pw_stream_queue_buffer (stream->pipewire_stream, last_pointer_buffer);
    }

  if (!last_frame_buffer)
    {
      GrdVncFrameReadyCallback callback = frame->callback;
      gpointer callback_user_data = frame->callback_user_data;

      callback (stream, g_steal_pointer (&frame), TRUE, callback_user_data);
      return;
    }

  process_frame_data (stream, last_frame_buffer->buffer,
                      g_steal_pointer (&frame));
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_stream_state_changed,
  .param_changed = on_stream_param_changed,
  .process = on_stream_process,
};

static void
add_common_format_params (struct spa_pod_builder *pod_builder,
                          enum spa_video_format   spa_format)
{
  struct spa_rectangle min_rect;
  struct spa_rectangle max_rect;
  struct spa_fraction min_framerate;
  struct spa_fraction max_framerate;

  min_rect = SPA_RECTANGLE (1, 1);
  max_rect = SPA_RECTANGLE (INT32_MAX, INT32_MAX);
  min_framerate = SPA_FRACTION (1, 1);
  max_framerate = SPA_FRACTION (30, 1);

  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_mediaType,
                       SPA_POD_Id (SPA_MEDIA_TYPE_video), 0);
  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_mediaSubtype,
                       SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw), 0);
  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_VIDEO_format,
                       SPA_POD_Id (spa_format), 0);
  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_VIDEO_size,
                       SPA_POD_CHOICE_RANGE_Rectangle (&min_rect,
                                                       &min_rect,
                                                       &max_rect), 0);
  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_VIDEO_framerate,
                       SPA_POD_Fraction (&SPA_FRACTION (0, 1)), 0);
  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_VIDEO_maxFramerate,
                       SPA_POD_CHOICE_RANGE_Fraction (&min_framerate,
                                                      &min_framerate,
                                                      &max_framerate), 0);
}

static gboolean
connect_to_stream (GrdVncPipeWireStream  *stream,
                   GError               **error)
{
  GrdSession *session = GRD_SESSION (stream->session);
  GrdContext *context = grd_session_get_context (session);
  struct pw_stream *pipewire_stream;
  uint8_t params_buffer[1024];
  struct spa_pod_builder pod_builder;
  const struct spa_pod *params[2];
  int ret;
  enum spa_video_format spa_format = SPA_VIDEO_FORMAT_BGRx;
  GrdEglThread *egl_thread;
  struct spa_pod_frame format_frame;
  gboolean need_fallback_format = FALSE;

  pipewire_stream = pw_stream_new (stream->pipewire_core,
                                   "grd-vnc-pipewire-stream",
                                   NULL);

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));

  spa_pod_builder_push_object (&pod_builder, &format_frame,
                               SPA_TYPE_OBJECT_Format,
                               SPA_PARAM_EnumFormat);
  add_common_format_params (&pod_builder, spa_format);

  egl_thread = grd_context_get_egl_thread (context);
  if (egl_thread)
    {
      uint32_t drm_format;
      int n_modifiers;
      g_autofree uint64_t *modifiers = NULL;

      grd_get_spa_format_details (spa_format, &drm_format, NULL);
      if (grd_egl_thread_get_modifiers_for_format (egl_thread, drm_format,
                                                   &n_modifiers,
                                                   &modifiers))
        {
          struct spa_pod_frame modifier_frame;
          int i;

          spa_pod_builder_prop (&pod_builder,
                                SPA_FORMAT_VIDEO_modifier,
                                (SPA_POD_PROP_FLAG_MANDATORY |
                                 SPA_POD_PROP_FLAG_DONT_FIXATE));

          spa_pod_builder_push_choice (&pod_builder, &modifier_frame,
                                       SPA_CHOICE_Enum, 0);
          spa_pod_builder_long (&pod_builder, modifiers[0]);

          for (i = 0; i < n_modifiers; i++)
            {
              uint64_t modifier = modifiers[i];

              spa_pod_builder_long (&pod_builder, modifier);
            }
          spa_pod_builder_long (&pod_builder, DRM_FORMAT_MOD_INVALID);
          spa_pod_builder_pop (&pod_builder, &modifier_frame);

          need_fallback_format = TRUE;
        }
    }

  params[0] = spa_pod_builder_pop (&pod_builder, &format_frame);

  if (need_fallback_format)
    {
      spa_pod_builder_push_object (&pod_builder, &format_frame,
                                   SPA_TYPE_OBJECT_Format,
                                   SPA_PARAM_EnumFormat);
      add_common_format_params (&pod_builder, spa_format);
      params[1] = spa_pod_builder_pop (&pod_builder, &format_frame);
    }

  stream->pipewire_stream = pipewire_stream;

  pw_stream_add_listener (pipewire_stream,
                          &stream->pipewire_stream_listener,
                          &stream_events,
                          stream);

  ret = pw_stream_connect (stream->pipewire_stream,
                           PW_DIRECTION_INPUT,
                           stream->src_node_id,
                           (PW_STREAM_FLAG_RT_PROCESS |
                            PW_STREAM_FLAG_AUTOCONNECT),
                           params, need_fallback_format ? 2 : 1);
  if (ret < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                           strerror (-ret));
      return FALSE;
    }

  return TRUE;
}

static void
on_core_error (void       *user_data,
               uint32_t    id,
               int         seq,
               int         res,
               const char *message)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (user_data);

  g_warning ("Pipewire core error: id:%u %s", id, message);

  if (id == PW_ID_CORE && res == -EPIPE)
    g_signal_emit (stream, signals[CLOSED], 0);
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .error = on_core_error,
};

GrdVncPipeWireStream *
grd_vnc_pipewire_stream_new (GrdSessionVnc  *session_vnc,
                             uint32_t        src_node_id,
                             GError        **error)
{
  g_autoptr (GrdVncPipeWireStream) stream = NULL;
  GrdPipeWireSource *pipewire_source;
  GSource *source;

  grd_maybe_initialize_pipewire ();

  stream = g_object_new (GRD_TYPE_VNC_PIPEWIRE_STREAM, NULL);
  stream->session = session_vnc;
  stream->src_node_id = src_node_id;

  pipewire_source = create_pipewire_source ();
  if (!pipewire_source)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire source");
      return NULL;
    }
  stream->pipewire_source = (GSource *) pipewire_source;

  stream->pipewire_context = pw_context_new (pipewire_source->pipewire_loop,
                                             NULL, 0);
  if (!stream->pipewire_context)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create pipewire context");
      return NULL;
    }

  stream->pipewire_core = pw_context_connect (stream->pipewire_context, NULL, 0);
  if (!stream->pipewire_core)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to connect pipewire context");
      return NULL;
    }

  source = g_source_new (&pending_frame_source_funcs, sizeof (GSource));
  stream->pending_frame_source = source;
  g_source_set_callback (source, do_render, stream, NULL);
  g_source_attach (source, NULL);
  g_source_unref (source);

  pw_core_add_listener (stream->pipewire_core,
                        &stream->pipewire_core_listener,
                        &core_events,
                        stream);

  if (!connect_to_stream (stream, error))
    return NULL;

  return g_steal_pointer (&stream);
}

typedef struct
{
  GMutex mutex;
  GCond cond;
  gboolean done;
} SyncData;

static void
on_sync_done (gboolean success,
              gpointer user_data)
{
  SyncData *sync_data = user_data;

  g_mutex_lock (&sync_data->mutex);
  sync_data->done = TRUE;
  g_cond_signal (&sync_data->cond);
  g_mutex_unlock (&sync_data->mutex);
}

static void
grd_vnc_pipewire_stream_finalize (GObject *object)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (object);
  GrdSession *session = GRD_SESSION (stream->session);
  GrdContext *context = grd_session_get_context (session);
  GrdEglThread *egl_thread;

  /* Setting a PipeWire stream inactive will wait for the data thread to end */
  if (stream->pipewire_stream)
    pw_stream_set_active (stream->pipewire_stream, false);

  egl_thread = grd_context_get_egl_thread (context);
  if (egl_thread)
    {
      SyncData sync_data = {};

      g_mutex_init (&sync_data.mutex);
      g_cond_init (&sync_data.cond);

      grd_egl_thread_sync (egl_thread, on_sync_done, &sync_data, NULL);

      g_mutex_lock (&sync_data.mutex);
      while (!sync_data.done)
        g_cond_wait (&sync_data.cond, &sync_data.mutex);
      g_mutex_unlock (&sync_data.mutex);

      g_cond_clear (&sync_data.cond);
      g_mutex_clear (&sync_data.mutex);
    }

  g_clear_pointer (&stream->pipewire_stream, pw_stream_destroy);

  g_clear_pointer (&stream->pipewire_core, pw_core_disconnect);
  g_clear_pointer (&stream->pipewire_context, pw_context_destroy);
  g_clear_pointer (&stream->pending_frame_source, g_source_destroy);
  if (stream->pipewire_source)
    {
      g_source_destroy (stream->pipewire_source);
      g_clear_pointer (&stream->pipewire_source, g_source_unref);
    }

  g_clear_pointer (&stream->pending_frame, grd_vnc_frame_unref);

  g_mutex_clear (&stream->frame_mutex);

  G_OBJECT_CLASS (grd_vnc_pipewire_stream_parent_class)->finalize (object);
}

static void
grd_vnc_pipewire_stream_init (GrdVncPipeWireStream *stream)
{
  g_mutex_init (&stream->frame_mutex);
}

static void
grd_vnc_pipewire_stream_class_init (GrdVncPipeWireStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_vnc_pipewire_stream_finalize;

  signals[CLOSED] = g_signal_new ("closed",
                                  G_TYPE_FROM_CLASS (klass),
                                  G_SIGNAL_RUN_LAST,
                                  0,
                                  NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}
