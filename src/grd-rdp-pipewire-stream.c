/*
 * Copyright (C) 2020 Pascal Nowack
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
 */

#include "config.h"

#include "grd-rdp-pipewire-stream.h"

#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <sys/mman.h>

#include "grd-context.h"
#include "grd-egl-thread.h"
#include "grd-hwaccel-nvidia.h"
#include "grd-pipewire-utils.h"
#include "grd-rdp-buffer.h"
#include "grd-rdp-buffer-pool.h"
#include "grd-rdp-damage-detector.h"
#include "grd-rdp-surface.h"
#include "grd-utils.h"

#define DEFAULT_BUFFER_POOL_SIZE 5

enum
{
  ERROR,
  CLOSED,
  VIDEO_RESIZED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct _GrdRdpFrame GrdRdpFrame;

typedef void (* GrdRdpFrameReadyCallback) (GrdRdpPipeWireStream *stream,
                                           GrdRdpFrame          *frame,
                                           gboolean              success,
                                           gpointer              user_data);

typedef struct
{
  GMutex buffer_mutex;
  gboolean is_locked;
} BufferContext;

typedef struct
{
  GMutex stream_mutex;
  GrdRdpPipeWireStream *stream;
} StreamContext;

struct _GrdRdpFrame
{
  gatomicrefcount refcount;

  GrdRdpBuffer *buffer;

  gboolean has_map;
  size_t map_size;
  uint8_t *map;

  GrdRdpPipeWireStream *stream;
  GrdRdpFrameReadyCallback callback;
  gpointer callback_user_data;
};

typedef struct
{
  uint8_t *pointer_bitmap;
  uint16_t pointer_hotspot_x;
  uint16_t pointer_hotspot_y;
  uint16_t pointer_width;
  uint16_t pointer_height;
} RdpPointer;

typedef struct
{
  GrdRdpBuffer *rdp_buffer;
} UnmapBufferData;

typedef struct
{
  GrdRdpBuffer *rdp_buffer;
} AllocateBufferData;

typedef struct
{
  GrdRdpBuffer *rdp_buffer;
} RealizeBufferData;

typedef struct
{
  AllocateBufferData allocate;
  RealizeBufferData realize;

  GrdRdpBuffer *rdp_buffer;
} ImportBufferData;

struct _GrdRdpPipeWireStream
{
  GObject parent;

  GrdSessionRdp *session_rdp;
  GrdRdpSurface *rdp_surface;
  GrdEglThreadSlot egl_slot;

  GMutex dequeue_mutex;
  gboolean dequeuing_disallowed;

  GSource *pipewire_source;
  struct pw_context *pipewire_context;
  struct pw_core *pipewire_core;

  struct spa_hook pipewire_core_listener;

  struct pw_registry *pipewire_registry;
  struct spa_hook pipewire_registry_listener;

  GrdRdpBufferPool *buffer_pool;

  StreamContext *frame_context;
  StreamContext *pointer_context;

  GSource *frame_render_source;
  GMutex frame_mutex;
  GrdRdpFrame *pending_frame;

  GSource *pointer_render_source;
  GMutex pointer_mutex;
  RdpPointer *pending_pointer;

  struct pw_stream *pipewire_stream;
  struct spa_hook pipewire_stream_listener;

  GHashTable *pipewire_buffers;

  uint32_t src_node_id;

  struct spa_video_info_raw spa_format;
};

G_DEFINE_TYPE (GrdRdpPipeWireStream, grd_rdp_pipewire_stream,
               G_TYPE_OBJECT)

static void grd_rdp_frame_unref (GrdRdpFrame *frame);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdRdpFrame, grd_rdp_frame_unref)

static BufferContext *
buffer_context_new (void)
{
  BufferContext *buffer_context;

  buffer_context = g_new0 (BufferContext, 1);
  g_mutex_init (&buffer_context->buffer_mutex);

  return buffer_context;
}

static void
buffer_context_free (BufferContext *buffer_context)
{
  g_mutex_clear (&buffer_context->buffer_mutex);

  g_free (buffer_context);
}

static void
acquire_pipewire_buffer_lock (GrdRdpPipeWireStream *stream,
                              struct pw_buffer     *buffer)
{
  BufferContext *buffer_context = NULL;

  if (!g_hash_table_lookup_extended (stream->pipewire_buffers, buffer,
                                     NULL, (gpointer *) &buffer_context))
    g_assert_not_reached ();

  g_mutex_lock (&buffer_context->buffer_mutex);
  g_assert (!buffer_context->is_locked);
  buffer_context->is_locked = TRUE;
}

static void
maybe_release_pipewire_buffer_lock (GrdRdpPipeWireStream *stream,
                                    struct pw_buffer     *buffer)
{
  BufferContext *buffer_context = NULL;

  if (!g_hash_table_lookup_extended (stream->pipewire_buffers, buffer,
                                     NULL, (gpointer *) &buffer_context))
    g_assert_not_reached ();

  if (!buffer_context->is_locked)
    return;

  buffer_context->is_locked = FALSE;
  g_mutex_unlock (&buffer_context->buffer_mutex);
}

static StreamContext *
stream_context_new (GrdRdpPipeWireStream *stream)
{
  StreamContext *stream_context;

  stream_context = g_new0 (StreamContext, 1);
  stream_context->stream = stream;

  g_mutex_init (&stream_context->stream_mutex);

  return stream_context;
}

static void
stream_context_free (gpointer data)
{
  StreamContext *stream_context = data;

  g_mutex_clear (&stream_context->stream_mutex);

  g_free (stream_context);
}

static void
stream_context_release_stream (StreamContext *stream_context)
{
  g_mutex_lock (&stream_context->stream_mutex);
  stream_context->stream = NULL;
  g_mutex_unlock (&stream_context->stream_mutex);
}

static GrdRdpFrame *
grd_rdp_frame_new (GrdRdpPipeWireStream     *stream,
                   GrdRdpFrameReadyCallback  callback,
                   gpointer                  callback_user_data)
{
  GrdRdpFrame *frame;

  frame = g_new0 (GrdRdpFrame, 1);

  g_atomic_ref_count_init (&frame->refcount);
  frame->stream = stream;
  frame->callback = callback;
  frame->callback_user_data = callback_user_data;

  return frame;
}

static GrdRdpFrame *
grd_rdp_frame_ref (GrdRdpFrame *frame)
{
  g_atomic_ref_count_inc (&frame->refcount);
  return frame;
}

static void
grd_rdp_frame_unref (GrdRdpFrame *frame)
{
  if (g_atomic_ref_count_dec (&frame->refcount))
    {
      g_assert (!frame->has_map);

      g_clear_pointer (&frame->buffer, grd_rdp_buffer_release);
      g_free (frame);
    }
}

static void
rdp_pointer_free (RdpPointer *rdp_pointer)
{
  g_clear_pointer (&rdp_pointer->pointer_bitmap, g_free);
  g_free (rdp_pointer);
}

void
grd_rdp_pipewire_stream_resize (GrdRdpPipeWireStream *stream,
                                GrdRdpVirtualMonitor *virtual_monitor)
{
  struct spa_rectangle virtual_monitor_rect;
  uint8_t params_buffer[1024];
  struct spa_pod_builder pod_builder;
  const struct spa_pod *params[1];

  virtual_monitor_rect = SPA_RECTANGLE (virtual_monitor->width,
                                        virtual_monitor->height);

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));

  params[0] = spa_pod_builder_add_object (
    &pod_builder,
    SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
    SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle (&virtual_monitor_rect),
    0);

  pw_stream_update_params (stream->pipewire_stream,
                           params, G_N_ELEMENTS (params));
}

static gboolean
render_frame (gpointer user_data)
{
  StreamContext *stream_context = user_data;
  g_autoptr (GMutexLocker) locker = NULL;
  GrdRdpPipeWireStream *stream;
  GrdRdpSurface *rdp_surface;
  gboolean had_frame = FALSE;
  GrdRdpFrame *frame;

  locker = g_mutex_locker_new (&stream_context->stream_mutex);
  if (!stream_context->stream)
    return G_SOURCE_REMOVE;

  stream = stream_context->stream;
  rdp_surface = stream->rdp_surface;

  g_mutex_lock (&stream->frame_mutex);
  frame = g_steal_pointer (&stream->pending_frame);

  if (frame)
    {
      g_mutex_lock (&rdp_surface->surface_mutex);
      had_frame = !!rdp_surface->pending_framebuffer;
      g_clear_pointer (&rdp_surface->pending_framebuffer,
                       grd_rdp_buffer_release);

      rdp_surface->pending_framebuffer = g_steal_pointer (&frame->buffer);
      g_mutex_unlock (&rdp_surface->surface_mutex);
    }
  g_mutex_unlock (&stream->frame_mutex);

  if (!frame)
    return G_SOURCE_CONTINUE;

  grd_session_rdp_notify_frame (stream->session_rdp, had_frame);
  grd_session_rdp_maybe_encode_pending_frame (stream->session_rdp, rdp_surface);

  grd_rdp_frame_unref (frame);

  return G_SOURCE_CONTINUE;
}

static gboolean
render_mouse_pointer (gpointer user_data)
{
  StreamContext *stream_context = user_data;
  g_autoptr (GMutexLocker) locker = NULL;
  GrdRdpPipeWireStream *stream;
  RdpPointer *rdp_pointer;

  locker = g_mutex_locker_new (&stream_context->stream_mutex);
  if (!stream_context->stream)
    return G_SOURCE_REMOVE;

  stream = stream_context->stream;
  g_mutex_lock (&stream->pointer_mutex);
  if (!stream->pending_pointer)
    {
      g_mutex_unlock (&stream->pointer_mutex);
      return G_SOURCE_CONTINUE;
    }

  rdp_pointer = g_steal_pointer (&stream->pending_pointer);
  g_mutex_unlock (&stream->pointer_mutex);

  if (rdp_pointer->pointer_bitmap)
    {
      grd_session_rdp_update_pointer (stream->session_rdp,
                                      rdp_pointer->pointer_hotspot_x,
                                      rdp_pointer->pointer_hotspot_y,
                                      rdp_pointer->pointer_width,
                                      rdp_pointer->pointer_height,
                                      g_steal_pointer (&rdp_pointer->pointer_bitmap));
    }
  else
    {
      grd_session_rdp_hide_pointer (stream->session_rdp);
    }

  rdp_pointer_free (rdp_pointer);

  return G_SOURCE_CONTINUE;
}

static gboolean
render_source_dispatch (GSource     *source,
                        GSourceFunc  callback,
                        gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs render_source_funcs =
{
  .dispatch = render_source_dispatch,
};

static void
create_render_sources (GrdRdpPipeWireStream *stream,
                       GMainContext         *render_context)
{
  StreamContext *frame_context, *pointer_context;

  frame_context = stream_context_new (stream);
  stream->frame_context = frame_context;

  stream->frame_render_source = g_source_new (&render_source_funcs,
                                              sizeof (GSource));
  g_source_set_callback (stream->frame_render_source, render_frame,
                         frame_context, stream_context_free);
  g_source_set_ready_time (stream->frame_render_source, -1);
  g_source_attach (stream->frame_render_source, render_context);

  pointer_context = stream_context_new (stream);
  stream->pointer_context = pointer_context;

  stream->pointer_render_source = g_source_new (&render_source_funcs,
                                                sizeof (GSource));
  g_source_set_callback (stream->pointer_render_source, render_mouse_pointer,
                         pointer_context, stream_context_free);
  g_source_set_ready_time (stream->pointer_render_source, -1);
  g_source_attach (stream->pointer_render_source, render_context);
}

static void
on_stream_state_changed (void                 *user_data,
                         enum pw_stream_state  old,
                         enum pw_stream_state  state,
                         const char           *error)
{
  g_debug ("PipeWire stream state changed from %s to %s",
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
on_sync_complete (gboolean success,
                  gpointer user_data)
{
  GrdSyncPoint *sync_point = user_data;

  grd_sync_point_complete (sync_point, success);
}

static void
sync_egl_thread (GrdEglThread *egl_thread)
{
  GrdSyncPoint sync_point = {};

  grd_sync_point_init (&sync_point);
  grd_egl_thread_sync (egl_thread, on_sync_complete, &sync_point, NULL);

  grd_sync_point_wait_for_completion (&sync_point);
  grd_sync_point_clear (&sync_point);
}

static void
release_all_buffers (GrdRdpPipeWireStream *stream)
{
  g_mutex_lock (&stream->frame_mutex);
  if (stream->pending_frame)
    g_clear_pointer (&stream->pending_frame->buffer, grd_rdp_buffer_release);
  g_mutex_unlock (&stream->frame_mutex);

  g_mutex_lock (&stream->rdp_surface->surface_mutex);
  g_clear_pointer (&stream->rdp_surface->pending_framebuffer,
                   grd_rdp_buffer_release);
  g_mutex_unlock (&stream->rdp_surface->surface_mutex);
}

static void
on_stream_param_changed (void                 *user_data,
                         uint32_t              id,
                         const struct spa_pod *format)
{
  GrdRdpPipeWireStream *stream = GRD_RDP_PIPEWIRE_STREAM (user_data);
  GrdSession *session = GRD_SESSION (stream->session_rdp);
  GrdContext *context = grd_session_get_context (session);
  GrdEglThread *egl_thread = grd_context_get_egl_thread (context);
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint8_t params_buffer[1024];
  struct spa_pod_builder pod_builder;
  enum spa_data_type allowed_buffer_types;
  const struct spa_pod *params[3];

  if (!format || id != SPA_PARAM_Format)
    return;

  spa_format_video_raw_parse (format, &stream->spa_format);
  height = stream->spa_format.size.height;
  width = stream->spa_format.size.width;
  stride = grd_session_rdp_get_stride_for_width (stream->session_rdp, width);

  g_debug ("[RDP] Stream parameters changed. New surface size: [%u, %u]",
           width, height);

  if (egl_thread)
    sync_egl_thread (egl_thread);
  release_all_buffers (stream);
  grd_rdp_surface_reset (stream->rdp_surface);

  if (!grd_rdp_damage_detector_resize_surface (stream->rdp_surface->detector,
                                               width, height) ||
      !grd_rdp_buffer_pool_resize_buffers (stream->buffer_pool,
                                           width, height, stride))
    {
      grd_session_rdp_notify_error (
        stream->session_rdp, GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED);
      return;
    }

  grd_rdp_surface_set_size (stream->rdp_surface, width, height);
  g_signal_emit (stream, signals[VIDEO_RESIZED], 0, width, height);

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));

  allowed_buffer_types = 1 << SPA_DATA_MemFd;
  if (egl_thread)
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

  params[2] = spa_pod_builder_add_object (
    &pod_builder,
    SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
    SPA_PARAM_META_type, SPA_POD_Id (SPA_META_Cursor),
    SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int (CURSOR_META_SIZE (384, 384),
                                                   CURSOR_META_SIZE (1, 1),
                                                   CURSOR_META_SIZE (384, 384)),
    0);

  pw_stream_update_params (stream->pipewire_stream,
                           params, G_N_ELEMENTS (params));
}

static void
on_stream_add_buffer (void             *user_data,
                      struct pw_buffer *buffer)
{
  GrdRdpPipeWireStream *stream = user_data;

  g_hash_table_insert (stream->pipewire_buffers, buffer, buffer_context_new ());
}

static void
on_stream_remove_buffer (void             *user_data,
                         struct pw_buffer *buffer)
{
  GrdRdpPipeWireStream *stream = user_data;
  BufferContext *buffer_context = NULL;

  if (!g_hash_table_lookup_extended (stream->pipewire_buffers, buffer,
                                     NULL, (gpointer *) &buffer_context))
    g_assert_not_reached ();

  /* Ensure buffer is not locked any more */
  g_mutex_lock (&buffer_context->buffer_mutex);
  g_mutex_unlock (&buffer_context->buffer_mutex);

  g_hash_table_remove (stream->pipewire_buffers, buffer);
}

static void
process_mouse_pointer_bitmap (GrdRdpPipeWireStream *stream,
                              struct spa_buffer    *buffer)
{
  struct spa_meta_cursor *spa_meta_cursor;
  struct spa_meta_bitmap *spa_meta_bitmap;
  RdpPointer *rdp_pointer = NULL;
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

      buf = SPA_MEMBER (spa_meta_bitmap, spa_meta_bitmap->offset, uint8_t);

      rdp_pointer = g_new0 (RdpPointer, 1);
      rdp_pointer->pointer_bitmap =
        g_memdup2 (buf, spa_meta_bitmap->size.height * spa_meta_bitmap->stride);
      rdp_pointer->pointer_hotspot_x = spa_meta_cursor->hotspot.x;
      rdp_pointer->pointer_hotspot_y = spa_meta_cursor->hotspot.y;
      rdp_pointer->pointer_width = spa_meta_bitmap->size.width;
      rdp_pointer->pointer_height = spa_meta_bitmap->size.height;
    }
  else if (spa_meta_bitmap)
    {
      rdp_pointer = g_new0 (RdpPointer, 1);
    }

  if (rdp_pointer)
    {
      g_mutex_lock (&stream->pointer_mutex);
      g_clear_pointer (&stream->pending_pointer, rdp_pointer_free);

      stream->pending_pointer = rdp_pointer;
      g_mutex_unlock (&stream->pointer_mutex);

      g_source_set_ready_time (stream->pointer_render_source, 0);
    }
}

static void
on_frame_ready (GrdRdpPipeWireStream *stream,
                GrdRdpFrame          *frame,
                gboolean              success,
                gpointer              user_data)
{
  struct pw_buffer *buffer = user_data;

  g_assert (frame);
  g_assert (buffer);

  if (frame->has_map)
    {
      munmap (frame->map, frame->map_size);
      frame->has_map = FALSE;
    }

  if (!success)
    goto out;

  g_assert (frame->buffer);

  g_mutex_lock (&stream->frame_mutex);
  g_clear_pointer (&stream->pending_frame, grd_rdp_frame_unref);

  stream->pending_frame = g_steal_pointer (&frame);
  g_mutex_unlock (&stream->frame_mutex);

  g_source_set_ready_time (stream->frame_render_source, 0);
out:
  pw_stream_queue_buffer (stream->pipewire_stream, buffer);
  maybe_release_pipewire_buffer_lock (stream, buffer);

  g_clear_pointer (&frame, grd_rdp_frame_unref);
}

static void
copy_frame_data (GrdRdpFrame *frame,
                 uint8_t     *src_data,
                 int          width,
                 int          height,
                 int          dst_stride,
                 int          src_stride,
                 int          bpp)
{
  GrdRdpBuffer *buffer = frame->buffer;
  int y;

  for (y = 0; y < height; ++y)
    {
      memcpy (grd_rdp_buffer_get_local_data (buffer) + y * dst_stride,
              ((uint8_t *) src_data) + y * src_stride,
              width * 4);
    }
}

static gboolean
cuda_unmap_resource (gpointer user_data)
{
  UnmapBufferData *data = user_data;

  grd_rdp_buffer_unmap_cuda_resource (data->rdp_buffer);

  return TRUE;
}

static void
unmap_cuda_resources (GrdEglThread *egl_thread,
                      GrdRdpBuffer *rdp_buffer)
{
  UnmapBufferData *data;

  data = g_new0 (UnmapBufferData, 1);
  data->rdp_buffer = rdp_buffer;

  grd_egl_thread_run_custom_task (egl_thread,
                                  cuda_unmap_resource, data,
                                  NULL, data, g_free);
}

static gboolean
cuda_allocate_buffer (gpointer user_data,
                      uint32_t pbo)
{
  AllocateBufferData *data = user_data;
  GrdRdpBuffer *rdp_buffer = data->rdp_buffer;

  return grd_rdp_buffer_register_read_only_gl_buffer (rdp_buffer, pbo);
}

static gboolean
allocate_buffer (gpointer user_data,
                 uint32_t pbo)
{
  ImportBufferData *data = user_data;

  return cuda_allocate_buffer (&data->allocate, pbo);
}

static gboolean
cuda_map_resource (gpointer user_data)
{
  RealizeBufferData *data = user_data;
  GrdRdpBuffer *rdp_buffer = data->rdp_buffer;

  return grd_rdp_buffer_map_cuda_resource (rdp_buffer);
}

static gboolean
realize_buffer (gpointer user_data)
{
  ImportBufferData *data = user_data;

  return cuda_map_resource (&data->realize);
}

static void
on_framebuffer_ready (gboolean success,
                      gpointer user_data)
{
  GrdRdpFrame *frame = user_data;

  frame->callback (frame->stream,
                   frame,
                   success,
                   frame->callback_user_data);
}

static void
process_frame_data (GrdRdpPipeWireStream *stream,
                    struct pw_buffer     *pw_buffer)
{
  struct spa_buffer *buffer = pw_buffer->buffer;
  g_autoptr (GrdRdpFrame) frame = NULL;
  GrdRdpFrameReadyCallback callback;
  gpointer user_data;
  uint32_t drm_format;
  int bpp;
  int width;
  int height;
  int src_stride;
  int dst_stride;

  g_assert (buffer->datas[0].chunk->size > 0);

  height = stream->spa_format.size.height;
  width = stream->spa_format.size.width;
  src_stride = buffer->datas[0].chunk->stride;
  dst_stride = grd_session_rdp_get_stride_for_width (stream->session_rdp,
                                                     width);
  grd_get_spa_format_details (stream->spa_format.format,
                              &drm_format, &bpp);

  frame = grd_rdp_frame_new (stream, on_frame_ready, pw_buffer);
  callback = frame->callback;
  user_data = frame->callback_user_data;

  if (buffer->datas[0].type == SPA_DATA_MemFd)
    {
      GrdSession *session = GRD_SESSION (stream->session_rdp);
      GrdContext *context = grd_session_get_context (session);
      GrdEglThread *egl_thread = grd_context_get_egl_thread (context);
      GrdHwAccelNvidia *hwaccel_nvidia = stream->rdp_surface->hwaccel_nvidia;
      AllocateBufferData *allocate_buffer_data;
      RealizeBufferData *realize_buffer_data;
      GrdRdpBuffer *rdp_buffer;
      size_t size;
      uint8_t *map;
      void *src_data;
      uint32_t pbo;
      uint8_t *data_to_upload;

      size = buffer->datas[0].maxsize + buffer->datas[0].mapoffset;
      map = mmap (NULL, size, PROT_READ, MAP_PRIVATE, buffer->datas[0].fd, 0);
      if (map == MAP_FAILED)
        {
          g_warning ("Failed to mmap buffer: %s", g_strerror (errno));
          callback (stream, g_steal_pointer (&frame), FALSE, user_data);
          return;
        }
      src_data = SPA_MEMBER (map, buffer->datas[0].mapoffset, uint8_t);

      frame->buffer = grd_rdp_buffer_pool_acquire (stream->buffer_pool);
      if (!frame->buffer)
        {
          grd_session_rdp_notify_error (stream->session_rdp,
                                        GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED);
          callback (stream, g_steal_pointer (&frame), FALSE, user_data);
          return;
        }
      rdp_buffer = frame->buffer;
      pbo = grd_rdp_buffer_get_pbo (rdp_buffer);

      if (stream->rdp_surface->needs_no_local_data &&
          src_stride == dst_stride)
        {
          frame->map_size = size;
          frame->map = map;
          frame->has_map = TRUE;

          data_to_upload = src_data;
        }
      else
        {
          copy_frame_data (frame,
                           src_data,
                           width, height,
                           dst_stride,
                           src_stride,
                           bpp);

          munmap (map, size);

          data_to_upload = grd_rdp_buffer_get_local_data (rdp_buffer);
        }

      if (!hwaccel_nvidia)
        {
          callback (stream, g_steal_pointer (&frame), TRUE, user_data);
          return;
        }

      unmap_cuda_resources (egl_thread, rdp_buffer);

      allocate_buffer_data = g_new0 (AllocateBufferData, 1);
      allocate_buffer_data->rdp_buffer = rdp_buffer;

      realize_buffer_data = g_new0 (RealizeBufferData, 1);
      realize_buffer_data->rdp_buffer = rdp_buffer;

      acquire_pipewire_buffer_lock (stream, pw_buffer);
      grd_egl_thread_upload (egl_thread,
                             stream->egl_slot,
                             pbo,
                             height,
                             dst_stride,
                             data_to_upload,
                             cuda_allocate_buffer,
                             allocate_buffer_data,
                             g_free,
                             cuda_map_resource,
                             realize_buffer_data,
                             g_free,
                             on_framebuffer_ready,
                             grd_rdp_frame_ref (g_steal_pointer (&frame)),
                             (GDestroyNotify) grd_rdp_frame_unref);
    }
  else if (buffer->datas[0].type == SPA_DATA_DmaBuf)
    {
      GrdSession *session = GRD_SESSION (stream->session_rdp);
      GrdContext *context = grd_session_get_context (session);
      GrdEglThread *egl_thread = grd_context_get_egl_thread (context);
      GrdHwAccelNvidia *hwaccel_nvidia = stream->rdp_surface->hwaccel_nvidia;
      GrdEglThreadImportIface iface;
      ImportBufferData *import_buffer_data = NULL;
      GrdRdpBuffer *rdp_buffer;
      int row_width;
      int *fds;
      uint32_t *offsets;
      uint32_t *strides;
      uint64_t *modifiers = NULL;
      uint32_t n_planes;
      unsigned int i;
      uint8_t *dst_data = NULL;

      frame->buffer = grd_rdp_buffer_pool_acquire (stream->buffer_pool);
      if (!frame->buffer)
        {
          grd_session_rdp_notify_error (stream->session_rdp,
                                        GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED);
          callback (stream, g_steal_pointer (&frame), FALSE, user_data);
          return;
        }
      rdp_buffer = frame->buffer;

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

      if (!stream->rdp_surface->needs_no_local_data)
        dst_data = grd_rdp_buffer_get_local_data (frame->buffer);

      if (hwaccel_nvidia)
        {
          unmap_cuda_resources (egl_thread, rdp_buffer);

          iface.allocate = allocate_buffer;
          iface.realize = realize_buffer;

          import_buffer_data = g_new0 (ImportBufferData, 1);
          import_buffer_data->allocate.rdp_buffer = rdp_buffer;
          import_buffer_data->realize.rdp_buffer = rdp_buffer;

          import_buffer_data->rdp_buffer = rdp_buffer;
        }

      acquire_pipewire_buffer_lock (stream, pw_buffer);
      grd_egl_thread_download (egl_thread,
                               stream->egl_slot,
                               grd_rdp_buffer_get_pbo (rdp_buffer),
                               height,
                               dst_stride,
                               hwaccel_nvidia ? &iface : NULL,
                               import_buffer_data,
                               g_free,
                               dst_data,
                               row_width,
                               drm_format,
                               width, height,
                               n_planes,
                               fds,
                               strides,
                               offsets,
                               modifiers,
                               on_framebuffer_ready,
                               grd_rdp_frame_ref (g_steal_pointer (&frame)),
                               (GDestroyNotify) grd_rdp_frame_unref);
    }
  else
    {
      g_assert_not_reached ();
    }
}

static void
on_stream_process (void *user_data)
{
  GrdRdpPipeWireStream *stream = GRD_RDP_PIPEWIRE_STREAM (user_data);
  g_autoptr (GMutexLocker) locker = NULL;
  struct pw_buffer *last_pointer_buffer = NULL;
  struct pw_buffer *last_frame_buffer = NULL;
  struct pw_buffer *next_buffer;

  locker = g_mutex_locker_new (&stream->dequeue_mutex);
  if (stream->dequeuing_disallowed)
    return;

  while ((next_buffer = pw_stream_dequeue_buffer (stream->pipewire_stream)))
    {
      struct spa_meta_header *spa_meta_header;

      spa_meta_header = spa_buffer_find_meta_data (next_buffer->buffer,
                                                   SPA_META_Header,
                                                   sizeof (struct spa_meta_header));
      if (spa_meta_header &&
          spa_meta_header->flags & SPA_META_HEADER_FLAG_CORRUPTED)
        {
          pw_stream_queue_buffer (stream->pipewire_stream, next_buffer);
          continue;
        }

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
  if (!last_pointer_buffer && !last_frame_buffer)
    return;

  if (last_pointer_buffer)
    {
      process_mouse_pointer_bitmap (stream, last_pointer_buffer->buffer);
      if (last_pointer_buffer != last_frame_buffer)
        pw_stream_queue_buffer (stream->pipewire_stream, last_pointer_buffer);
    }
  if (!last_frame_buffer)
    return;

  process_frame_data (stream, last_frame_buffer);
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_stream_state_changed,
  .param_changed = on_stream_param_changed,
  .add_buffer = on_stream_add_buffer,
  .remove_buffer = on_stream_remove_buffer,
  .process = on_stream_process,
};

static void
add_common_format_params (struct spa_pod_builder     *pod_builder,
                          enum spa_video_format       spa_format,
                          const GrdRdpVirtualMonitor *virtual_monitor,
                          uint32_t                    refresh_rate)
{
  struct spa_rectangle min_rect;
  struct spa_rectangle max_rect;
  struct spa_fraction min_framerate;
  struct spa_fraction max_framerate;

  min_rect = SPA_RECTANGLE (1, 1);
  max_rect = SPA_RECTANGLE (INT32_MAX, INT32_MAX);
  min_framerate = SPA_FRACTION (1, 1);
  max_framerate = SPA_FRACTION (refresh_rate, 1);

  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_mediaType,
                       SPA_POD_Id (SPA_MEDIA_TYPE_video), 0);
  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_mediaSubtype,
                       SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw), 0);
  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_VIDEO_format,
                       SPA_POD_Id (spa_format), 0);

  if (virtual_monitor)
    {
      struct spa_rectangle virtual_monitor_rect;

      virtual_monitor_rect = SPA_RECTANGLE (virtual_monitor->width,
                                            virtual_monitor->height);
      spa_pod_builder_add (pod_builder,
                           SPA_FORMAT_VIDEO_size,
                           SPA_POD_Rectangle (&virtual_monitor_rect), 0);
    }
  else
    {
      spa_pod_builder_add (pod_builder,
                           SPA_FORMAT_VIDEO_size,
                           SPA_POD_CHOICE_RANGE_Rectangle (&min_rect,
                                                           &min_rect,
                                                           &max_rect), 0);
    }
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
connect_to_stream (GrdRdpPipeWireStream        *stream,
                   const GrdRdpVirtualMonitor  *virtual_monitor,
                   uint32_t                     refresh_rate,
                   GError                     **error)
{
  GrdSession *session = GRD_SESSION (stream->session_rdp);
  GrdContext *context = grd_session_get_context (session);
  struct pw_stream *pipewire_stream;
  uint8_t params_buffer[1024];
  struct spa_pod_builder pod_builder;
  const struct spa_pod *params[2];
  struct spa_pod_frame format_frame;
  GrdEglThread *egl_thread;
  enum spa_video_format spa_format = SPA_VIDEO_FORMAT_BGRx;
  gboolean need_fallback_format = FALSE;
  int ret;

  pipewire_stream = pw_stream_new (stream->pipewire_core,
                                   "grd-rdp-pipewire-stream",
                                   NULL);

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));

  spa_pod_builder_push_object (&pod_builder, &format_frame,
                               SPA_TYPE_OBJECT_Format,
                               SPA_PARAM_EnumFormat);
  add_common_format_params (&pod_builder, spa_format, virtual_monitor,
                            refresh_rate);

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
      add_common_format_params (&pod_builder, spa_format, virtual_monitor,
                                refresh_rate);
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
  GrdRdpPipeWireStream *stream = GRD_RDP_PIPEWIRE_STREAM (user_data);

  g_warning ("PipeWire core error: id:%u %s", id, message);

  if (id == PW_ID_CORE && res == -EPIPE)
    g_signal_emit (stream, signals[ERROR], 0);
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .error = on_core_error,
};

static void
registry_event_global_remove (void     *user_data,
                              uint32_t  id)
{
  GrdRdpPipeWireStream *stream = user_data;

  if (id != stream->src_node_id)
    return;

  g_debug ("[RDP] PipeWire stream with node id %u closed", id);
  g_signal_emit (stream, signals[CLOSED], 0);
}

static const struct pw_registry_events registry_events =
{
  .version = PW_VERSION_REGISTRY_EVENTS,
  .global_remove = registry_event_global_remove,
};

GrdRdpPipeWireStream *
grd_rdp_pipewire_stream_new (GrdSessionRdp               *session_rdp,
                             GrdHwAccelNvidia            *hwaccel_nvidia,
                             GMainContext                *render_context,
                             GrdRdpSurface               *rdp_surface,
                             const GrdRdpVirtualMonitor  *virtual_monitor,
                             uint32_t                     src_node_id,
                             GError                     **error)
{
  GrdSession *session = GRD_SESSION (session_rdp);
  GrdContext *context = grd_session_get_context (session);
  GrdEglThread *egl_thread = grd_context_get_egl_thread (context);
  g_autoptr (GrdRdpPipeWireStream) stream = NULL;
  GrdPipeWireSource *pipewire_source;

  stream = g_object_new (GRD_TYPE_RDP_PIPEWIRE_STREAM, NULL);
  stream->session_rdp = session_rdp;
  stream->rdp_surface = rdp_surface;
  stream->src_node_id = src_node_id;

  if (egl_thread && !hwaccel_nvidia)
    stream->egl_slot = grd_egl_thread_acquire_slot (egl_thread);

  pw_init (NULL, NULL);

  create_render_sources (stream, render_context);

  pipewire_source = grd_attached_pipewire_source_new ("RDP", error);
  if (!pipewire_source)
    return NULL;

  stream->pipewire_source = (GSource *) pipewire_source;

  stream->pipewire_context = pw_context_new (pipewire_source->pipewire_loop,
                                             NULL, 0);
  if (!stream->pipewire_context)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire context");
      return NULL;
    }

  stream->pipewire_core = pw_context_connect (stream->pipewire_context, NULL, 0);
  if (!stream->pipewire_core)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to connect PipeWire context");
      return NULL;
    }

  pw_core_add_listener (stream->pipewire_core,
                        &stream->pipewire_core_listener,
                        &core_events,
                        stream);

  stream->pipewire_registry = pw_core_get_registry (stream->pipewire_core,
                                                    PW_VERSION_REGISTRY, 0);
  if (!stream->pipewire_registry)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to retrieve PipeWire registry");
      return NULL;
    }

  pw_registry_add_listener (stream->pipewire_registry,
                            &stream->pipewire_registry_listener,
                            &registry_events, stream);

  if (!connect_to_stream (stream, virtual_monitor, rdp_surface->refresh_rate, error))
    return NULL;

  stream->buffer_pool = grd_rdp_buffer_pool_new (egl_thread,
                                                 hwaccel_nvidia,
                                                 rdp_surface->cuda_stream,
                                                 DEFAULT_BUFFER_POOL_SIZE);
  if (!stream->buffer_pool)
    return NULL;

  return g_steal_pointer (&stream);
}

static void
grd_rdp_pipewire_stream_finalize (GObject *object)
{
  GrdRdpPipeWireStream *stream = GRD_RDP_PIPEWIRE_STREAM (object);
  GrdSession *session = GRD_SESSION (stream->session_rdp);
  GrdContext *context = grd_session_get_context (session);
  GrdEglThread *egl_thread;

  g_mutex_lock (&stream->dequeue_mutex);
  stream->dequeuing_disallowed = TRUE;
  g_mutex_unlock (&stream->dequeue_mutex);

  egl_thread = grd_context_get_egl_thread (context);
  if (egl_thread && stream->pipewire_stream)
    sync_egl_thread (egl_thread);

  g_clear_pointer (&stream->pipewire_stream, pw_stream_destroy);

  if (stream->pipewire_registry)
    {
      spa_hook_remove (&stream->pipewire_registry_listener);
      pw_proxy_destroy ((struct pw_proxy *) stream->pipewire_registry);
      stream->pipewire_registry = NULL;
    }

  g_clear_pointer (&stream->pipewire_core, pw_core_disconnect);
  g_clear_pointer (&stream->pipewire_context, pw_context_destroy);
  if (stream->pipewire_source)
    {
      g_source_destroy (stream->pipewire_source);
      g_clear_pointer (&stream->pipewire_source, g_source_unref);
    }

  stream_context_release_stream (stream->pointer_context);
  stream_context_release_stream (stream->frame_context);

  if (stream->pointer_render_source)
    {
      g_source_destroy (stream->pointer_render_source);
      g_clear_pointer (&stream->pointer_render_source, g_source_unref);
    }
  if (stream->frame_render_source)
    {
      g_source_destroy (stream->frame_render_source);
      g_clear_pointer (&stream->frame_render_source, g_source_unref);
    }

  grd_rdp_damage_detector_invalidate_surface (stream->rdp_surface->detector);
  g_clear_pointer (&stream->pending_pointer, rdp_pointer_free);
  g_clear_pointer (&stream->pending_frame, grd_rdp_frame_unref);

  release_all_buffers (stream);
  g_clear_object (&stream->buffer_pool);

  g_mutex_clear (&stream->pointer_mutex);
  g_mutex_clear (&stream->frame_mutex);
  g_mutex_clear (&stream->dequeue_mutex);

  g_clear_pointer (&stream->pipewire_buffers, g_hash_table_unref);

  pw_deinit ();

  if (egl_thread)
    grd_egl_thread_release_slot (egl_thread, stream->egl_slot);

  G_OBJECT_CLASS (grd_rdp_pipewire_stream_parent_class)->finalize (object);
}

static void
grd_rdp_pipewire_stream_init (GrdRdpPipeWireStream *stream)
{
  stream->pipewire_buffers =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) buffer_context_free);

  g_mutex_init (&stream->dequeue_mutex);
  g_mutex_init (&stream->frame_mutex);
  g_mutex_init (&stream->pointer_mutex);
}

static void
grd_rdp_pipewire_stream_class_init (GrdRdpPipeWireStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_rdp_pipewire_stream_finalize;

  signals[ERROR] = g_signal_new ("error",
                                  G_TYPE_FROM_CLASS (klass),
                                  G_SIGNAL_RUN_LAST,
                                  0,
                                  NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[CLOSED] = g_signal_new ("closed",
                                  G_TYPE_FROM_CLASS (klass),
                                  G_SIGNAL_RUN_LAST,
                                  0,
                                  NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[VIDEO_RESIZED] = g_signal_new ("video-resized",
                                         G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST,
                                         0,
                                         NULL, NULL, NULL,
                                         G_TYPE_NONE, 2,
                                         G_TYPE_UINT, G_TYPE_UINT);
}
