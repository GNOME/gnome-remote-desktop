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

#include <linux/dma-buf.h>
#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "grd-pipewire-utils.h"
#include "grd-vnc-cursor.h"

enum
{
  CLOSED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct _GrdVncFrame
{
  void *data;
  rfbCursorPtr rfb_cursor;
  gboolean cursor_moved;
  int cursor_x;
  int cursor_y;
} GrdVncFrame;

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

  struct pw_stream *pipewire_stream;
  struct spa_hook pipewire_stream_listener;

  uint32_t src_node_id;

  struct spa_video_info_raw spa_format;
};

G_DEFINE_TYPE (GrdVncPipeWireStream, grd_vnc_pipewire_stream,
               G_TYPE_OBJECT)

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
  uint8_t params_buffer[1024];
  struct spa_pod_builder pod_builder;
  int width;
  int height;
  const struct spa_pod *params[3];

  if (!format || id != SPA_PARAM_Format)
    return;

  spa_format_video_raw_parse (format, &stream->spa_format);

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));

  width = stream->spa_format.size.width;
  height = stream->spa_format.size.height;

  grd_session_vnc_queue_resize_framebuffer (stream->session, width, height);

  params[0] = spa_pod_builder_add_object (
    &pod_builder,
    SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int (8, 1, 8),
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

static int
do_render (struct spa_loop *loop,
           bool             async,
           uint32_t         seq,
           const void      *data,
           size_t           size,
           void            *user_data)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (user_data);
  GrdVncFrame *frame;

  g_mutex_lock (&stream->frame_mutex);
  frame = g_steal_pointer (&stream->pending_frame);
  g_mutex_unlock (&stream->frame_mutex);

  if (!frame)
    return 0;

  if (grd_session_vnc_is_client_gone (stream->session))
    {
      g_free (frame->data);
      g_clear_pointer (&frame->rfb_cursor, rfbFreeCursor);
      g_free (frame);
      return 0;
    }

  if (frame->rfb_cursor)
    grd_session_vnc_set_cursor (stream->session, frame->rfb_cursor);

  if (frame->cursor_moved)
    {
      grd_session_vnc_move_cursor (stream->session,
                                   frame->cursor_x,
                                   frame->cursor_y);
    }

  if (frame->data)
    grd_session_vnc_take_buffer (stream->session, frame->data);
  else
    grd_session_vnc_flush (stream->session);

  g_free (frame);

  return 0;
}

static GrdVncFrame *
process_buffer (GrdVncPipeWireStream *stream,
                struct spa_buffer    *buffer)
{
  size_t size;
  uint8_t *map;
  void *src_data;
  struct spa_meta_cursor *spa_meta_cursor;
  g_autofree GrdVncFrame *frame = NULL;

  frame = g_new0 (GrdVncFrame, 1);

  if (buffer->datas[0].chunk->size == 0)
    {
      map = NULL;
      src_data = NULL;
    }
  else if (buffer->datas[0].type == SPA_DATA_MemFd)
    {
      size = buffer->datas[0].maxsize + buffer->datas[0].mapoffset;
      map = mmap (NULL, size, PROT_READ, MAP_PRIVATE, buffer->datas[0].fd, 0);
      if (map == MAP_FAILED)
        {
          g_warning ("Failed to mmap buffer: %s", g_strerror (errno));
          return NULL;
        }
      src_data = SPA_MEMBER (map, buffer->datas[0].mapoffset, uint8_t);
    }
  else if (buffer->datas[0].type == SPA_DATA_DmaBuf)
    {
      int fd;

      fd = buffer->datas[0].fd;
      size = buffer->datas[0].maxsize + buffer->datas[0].mapoffset;

      map = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (map == MAP_FAILED)
        {
          g_warning ("Failed to mmap DMA buffer: %s", g_strerror (errno));
          return NULL;
        }
      grd_sync_dma_buf (fd, DMA_BUF_SYNC_START);

      src_data = SPA_MEMBER (map, buffer->datas[0].mapoffset, uint8_t);
    }
  else if (buffer->datas[0].type == SPA_DATA_MemPtr)
    {
      size = buffer->datas[0].maxsize + buffer->datas[0].mapoffset;
      map = NULL;
      src_data = buffer->datas[0].data;
    }
  else
    {
      return NULL;
    }

  if (src_data)
    {
      int src_stride;
      int dst_stride;
      int width;
      int height;
      int y;

      height = stream->spa_format.size.height;
      width = stream->spa_format.size.width;
      src_stride = buffer->datas[0].chunk->stride;
      dst_stride = grd_session_vnc_get_stride_for_width (stream->session,
                                                         width);

      frame->data = g_malloc (height * dst_stride);
      for (y = 0; y < height; y++)
        {
          memcpy (((uint8_t *) frame->data) + y * dst_stride,
                  ((uint8_t *) src_data) + y * src_stride,
                  width * 4);
        }
    }

  if (map)
    {
      if (buffer->datas[0].type == SPA_DATA_DmaBuf)
        grd_sync_dma_buf (buffer->datas[0].fd, DMA_BUF_SYNC_END);
      munmap (map, size);
    }

  spa_meta_cursor = spa_buffer_find_meta_data (buffer, SPA_META_Cursor,
                                               sizeof *spa_meta_cursor);
  if (spa_meta_cursor && spa_meta_cursor_is_valid (spa_meta_cursor))
    {
      struct spa_meta_bitmap *spa_meta_bitmap;
      GrdPixelFormat format;

      if (spa_meta_cursor->bitmap_offset)
        {
          spa_meta_bitmap = SPA_MEMBER (spa_meta_cursor,
                                        spa_meta_cursor->bitmap_offset,
                                        struct spa_meta_bitmap);
        }
      else
        {
          spa_meta_bitmap = NULL;
        }

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

      frame->cursor_moved = TRUE;
      frame->cursor_x = spa_meta_cursor->position.x;
      frame->cursor_y = spa_meta_cursor->position.y;
    }

  return g_steal_pointer (&frame);
}

static void
on_stream_process (void *user_data)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (user_data);
  GrdPipeWireSource *pipewire_source =
    (GrdPipeWireSource *) stream->pipewire_source;
  struct pw_buffer *next_buffer;
  struct pw_buffer *buffer = NULL;
  GrdVncFrame *frame;

  next_buffer = pw_stream_dequeue_buffer (stream->pipewire_stream);
  while (next_buffer)
    {
      buffer = next_buffer;
      next_buffer = pw_stream_dequeue_buffer (stream->pipewire_stream);

      if (next_buffer)
        pw_stream_queue_buffer (stream->pipewire_stream, buffer);
    }
  if (!buffer)
    return;

  frame = process_buffer (stream, buffer->buffer);

  g_assert (frame);
  g_mutex_lock (&stream->frame_mutex);
  if (stream->pending_frame)
    {
      if (!frame->data && stream->pending_frame->data)
        frame->data = g_steal_pointer (&stream->pending_frame->data);
      if (!frame->rfb_cursor && stream->pending_frame->rfb_cursor)
        frame->rfb_cursor = g_steal_pointer (&stream->pending_frame->rfb_cursor);
      if (!frame->cursor_moved && stream->pending_frame->cursor_moved)
        {
          frame->cursor_x = stream->pending_frame->cursor_x;
          frame->cursor_y = stream->pending_frame->cursor_y;
          frame->cursor_moved = TRUE;
        }

      g_free (stream->pending_frame->data);
      g_clear_pointer (&stream->pending_frame, g_free);
    }
  stream->pending_frame = frame;
  g_mutex_unlock (&stream->frame_mutex);

  pw_stream_queue_buffer (stream->pipewire_stream, buffer);

  pw_loop_invoke (pipewire_source->pipewire_loop, do_render,
                  SPA_ID_INVALID, NULL, 0,
                  false, stream);
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_stream_state_changed,
  .param_changed = on_stream_param_changed,
  .process = on_stream_process,
};

static gboolean
connect_to_stream (GrdVncPipeWireStream  *stream,
                   GError               **error)
{
  struct pw_stream *pipewire_stream;
  uint8_t params_buffer[1024];
  struct spa_pod_builder pod_builder;
  struct spa_rectangle min_rect;
  struct spa_rectangle max_rect;
  struct spa_fraction min_framerate;
  struct spa_fraction max_framerate;
  const struct spa_pod *params[2];
  int ret;

  pipewire_stream = pw_stream_new (stream->pipewire_core,
                                   "grd-vnc-pipewire-stream",
                                   NULL);

  min_rect = SPA_RECTANGLE (1, 1);
  max_rect = SPA_RECTANGLE (INT32_MAX, INT32_MAX);
  min_framerate = SPA_FRACTION (1, 1);
  max_framerate = SPA_FRACTION (30, 1);

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));
  params[0] = spa_pod_builder_add_object (
    &pod_builder,
    SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
    SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_video),
    SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_VIDEO_format, SPA_POD_Id (SPA_VIDEO_FORMAT_BGRx),
    SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle (&min_rect,
                                                           &min_rect,
                                                           &max_rect),
    SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction (&SPA_FRACTION(0, 1)),
    SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction (&min_framerate,
                                                                  &min_framerate,
                                                                  &max_framerate),
    0);

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
                           params, 1);
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

  pw_core_add_listener (stream->pipewire_core,
                        &stream->pipewire_core_listener,
                        &core_events,
                        stream);

  if (!connect_to_stream (stream, error))
    return NULL;

  return g_steal_pointer (&stream);
}

static void
grd_vnc_pipewire_stream_finalize (GObject *object)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (object);

  /*
   * We can't clear stream->pipewire_stream before destroying it, as the data
   * thread in PipeWire might access the variable during destruction.
   */
  if (stream->pipewire_stream)
    pw_stream_destroy (stream->pipewire_stream);

  g_clear_pointer (&stream->pipewire_core, pw_core_disconnect);
  g_clear_pointer (&stream->pipewire_context, pw_context_destroy);
  if (stream->pipewire_source)
    {
      g_source_destroy (stream->pipewire_source);
      g_clear_pointer (&stream->pipewire_source, g_source_unref);
    }

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
