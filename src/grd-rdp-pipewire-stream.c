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
#include <spa/utils/result.h>
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
  CLOSED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct _GrdRdpFrame GrdRdpFrame;

typedef void (* GrdRdpFrameReadyCallback) (GrdRdpPipeWireStream *stream,
                                           GrdRdpFrame          *frame,
                                           gboolean              success,
                                           gpointer              user_data);

struct _GrdRdpFrame
{
  gatomicrefcount refcount;

  GrdRdpBuffer *buffer;

  gboolean has_map;
  size_t map_size;
  uint8_t *map;

  gboolean has_pointer_data;
  uint8_t *pointer_bitmap;
  uint16_t pointer_hotspot_x;
  uint16_t pointer_hotspot_y;
  uint16_t pointer_width;
  uint16_t pointer_height;
  gboolean pointer_is_hidden;

  GrdRdpPipeWireStream *stream;
  GrdRdpFrameReadyCallback callback;
  gpointer callback_user_data;
};

typedef struct
{
  GrdHwAccelNvidia *hwaccel_nvidia;
  GrdRdpBuffer *rdp_buffer;
} UnmapBufferData;

typedef struct
{
  GrdHwAccelNvidia *hwaccel_nvidia;
  GrdRdpBuffer *rdp_buffer;
} AllocateBufferData;

typedef struct
{
  GrdHwAccelNvidia *hwaccel_nvidia;
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

  GSource *pipewire_source;
  struct pw_context *pipewire_context;
  struct pw_core *pipewire_core;

  struct spa_hook pipewire_core_listener;

  GrdRdpBufferPool *buffer_pool;

  GSource *render_source;
  GMutex frame_mutex;
  GrdRdpFrame *pending_frame;

  struct pw_stream *pipewire_stream;
  struct spa_hook pipewire_stream_listener;

  uint32_t src_node_id;

  struct spa_video_info_raw spa_format;
};

G_DEFINE_TYPE (GrdRdpPipeWireStream, grd_rdp_pipewire_stream,
               G_TYPE_OBJECT)

static void grd_rdp_frame_unref (GrdRdpFrame *frame);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdRdpFrame, grd_rdp_frame_unref)

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
      g_free (frame->pointer_bitmap);
      g_free (frame);
    }
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
do_render (gpointer user_data)
{
  GrdRdpPipeWireStream *stream = GRD_RDP_PIPEWIRE_STREAM (user_data);
  GrdRdpFrame *frame;

  g_mutex_lock (&stream->frame_mutex);
  frame = g_steal_pointer (&stream->pending_frame);

  if (frame && frame->buffer)
    {
      g_mutex_lock (&stream->rdp_surface->surface_mutex);
      g_assert (!stream->rdp_surface->new_framebuffer);

      stream->rdp_surface->new_framebuffer = g_steal_pointer (&frame->buffer);
      g_mutex_unlock (&stream->rdp_surface->surface_mutex);
    }
  g_mutex_unlock (&stream->frame_mutex);

  if (!frame)
    return G_SOURCE_CONTINUE;

  grd_session_rdp_maybe_encode_new_frame (stream->session_rdp,
                                          stream->rdp_surface);

  if (frame->pointer_bitmap)
    {
      grd_session_rdp_update_pointer (stream->session_rdp,
                                      frame->pointer_hotspot_x,
                                      frame->pointer_hotspot_y,
                                      frame->pointer_width,
                                      frame->pointer_height,
                                      g_steal_pointer (&frame->pointer_bitmap));
    }
  else if (frame->pointer_is_hidden)
    {
      grd_session_rdp_hide_pointer (stream->session_rdp);
    }

  grd_rdp_frame_unref (frame);

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
create_render_source (GrdRdpPipeWireStream *stream,
                      GMainContext         *render_context)
{
  stream->render_source = g_source_new (&render_source_funcs, sizeof (GSource));
  g_source_set_callback (stream->render_source, do_render, stream, NULL);
  g_source_set_ready_time (stream->render_source, -1);
  g_source_attach (stream->render_source, render_context);
}

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
  g_clear_pointer (&stream->rdp_surface->new_framebuffer,
                   grd_rdp_buffer_release);
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
process_mouse_pointer_bitmap (GrdRdpPipeWireStream *stream,
                              struct spa_buffer    *buffer,
                              GrdRdpFrame          *frame)
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

      buf = SPA_MEMBER (spa_meta_bitmap, spa_meta_bitmap->offset, uint8_t);
      frame->pointer_bitmap =
        g_memdup2 (buf, spa_meta_bitmap->size.height *
                        spa_meta_bitmap->stride);
      frame->pointer_hotspot_x = spa_meta_cursor->hotspot.x;
      frame->pointer_hotspot_y = spa_meta_cursor->hotspot.y;
      frame->pointer_width = spa_meta_bitmap->size.width;
      frame->pointer_height = spa_meta_bitmap->size.height;
      frame->has_pointer_data = TRUE;
    }
  else if (spa_meta_bitmap)
    {
      frame->pointer_is_hidden = TRUE;
      frame->has_pointer_data = TRUE;
    }
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
      memcpy (((uint8_t *) buffer->local_data) + y * dst_stride,
              ((uint8_t *) src_data) + y * src_stride,
              width * 4);
    }
}

static gboolean
cuda_unmap_resource (gpointer user_data)
{
  UnmapBufferData *data = user_data;

  if (!data->rdp_buffer->mapped_cuda_pointer)
    return TRUE;

  grd_hwaccel_nvidia_unmap_cuda_resource (data->hwaccel_nvidia,
                                          data->rdp_buffer->cuda_resource,
                                          data->rdp_buffer->cuda_stream);
  data->rdp_buffer->mapped_cuda_pointer = 0;

  return TRUE;
}

static void
unmap_cuda_resources (GrdEglThread     *egl_thread,
                      GrdHwAccelNvidia *hwaccel_nvidia,
                      GrdRdpBuffer     *rdp_buffer)
{
  UnmapBufferData *data;

  data = g_new0 (UnmapBufferData, 1);
  data->hwaccel_nvidia = hwaccel_nvidia;
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
  gboolean success;

  success = grd_hwaccel_nvidia_register_read_only_gl_buffer (data->hwaccel_nvidia,
                                                             &rdp_buffer->cuda_resource,
                                                             pbo);
  if (success)
    rdp_buffer->pbo = pbo;

  return success;
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
  size_t mapped_size = 0;

  return grd_hwaccel_nvidia_map_cuda_resource (data->hwaccel_nvidia,
                                               rdp_buffer->cuda_resource,
                                               &rdp_buffer->mapped_cuda_pointer,
                                               &mapped_size,
                                               rdp_buffer->cuda_stream);
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
                    struct spa_buffer    *buffer,
                    GrdRdpFrame          *frame)
{
  GrdRdpFrameReadyCallback callback = frame->callback;
  gpointer user_data = frame->callback_user_data;
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
          callback (stream, g_steal_pointer (&frame), TRUE, user_data);
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
      pbo = rdp_buffer->pbo;

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

          data_to_upload = rdp_buffer->local_data;
        }

      if (!hwaccel_nvidia)
        {
          callback (stream, g_steal_pointer (&frame), TRUE, user_data);
          return;
        }

      unmap_cuda_resources (egl_thread, hwaccel_nvidia, rdp_buffer);

      allocate_buffer_data = g_new0 (AllocateBufferData, 1);
      allocate_buffer_data->hwaccel_nvidia = hwaccel_nvidia;
      allocate_buffer_data->rdp_buffer = rdp_buffer;

      realize_buffer_data = g_new0 (RealizeBufferData, 1);
      realize_buffer_data->hwaccel_nvidia = hwaccel_nvidia;
      realize_buffer_data->rdp_buffer = rdp_buffer;

      grd_egl_thread_upload (egl_thread,
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
        dst_data = frame->buffer->local_data;

      if (hwaccel_nvidia)
        {
          unmap_cuda_resources (egl_thread, hwaccel_nvidia, rdp_buffer);

          iface.allocate = allocate_buffer;
          iface.realize = realize_buffer;

          import_buffer_data = g_new0 (ImportBufferData, 1);
          import_buffer_data->allocate.hwaccel_nvidia = hwaccel_nvidia;
          import_buffer_data->allocate.rdp_buffer = rdp_buffer;

          import_buffer_data->realize.hwaccel_nvidia = hwaccel_nvidia;
          import_buffer_data->realize.rdp_buffer = rdp_buffer;

          import_buffer_data->rdp_buffer = rdp_buffer;
        }

      grd_egl_thread_download (egl_thread,
                               rdp_buffer->pbo,
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
  else if (buffer->datas[0].type == SPA_DATA_MemPtr)
    {
      GrdSession *session = GRD_SESSION (stream->session_rdp);
      GrdContext *context = grd_session_get_context (session);
      GrdEglThread *egl_thread = grd_context_get_egl_thread (context);
      GrdHwAccelNvidia *hwaccel_nvidia = stream->rdp_surface->hwaccel_nvidia;
      AllocateBufferData *allocate_buffer_data;
      RealizeBufferData *realize_buffer_data;
      GrdRdpBuffer *rdp_buffer;
      void *src_data;
      uint32_t pbo;
      uint8_t *data_to_upload;

      src_data = buffer->datas[0].data;

      frame->buffer = grd_rdp_buffer_pool_acquire (stream->buffer_pool);
      if (!frame->buffer)
        {
          grd_session_rdp_notify_error (stream->session_rdp,
                                        GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED);
          callback (stream, g_steal_pointer (&frame), FALSE, user_data);
          return;
        }
      rdp_buffer = frame->buffer;
      pbo = rdp_buffer->pbo;

      if (stream->rdp_surface->needs_no_local_data &&
          src_stride == dst_stride)
        {
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

          data_to_upload = rdp_buffer->local_data;
        }

      if (!hwaccel_nvidia)
        {
          callback (stream, g_steal_pointer (&frame), TRUE, user_data);
          return;
        }

      unmap_cuda_resources (egl_thread, hwaccel_nvidia, rdp_buffer);

      allocate_buffer_data = g_new0 (AllocateBufferData, 1);
      allocate_buffer_data->hwaccel_nvidia = hwaccel_nvidia;
      allocate_buffer_data->rdp_buffer = rdp_buffer;

      realize_buffer_data = g_new0 (RealizeBufferData, 1);
      realize_buffer_data->hwaccel_nvidia = hwaccel_nvidia;
      realize_buffer_data->rdp_buffer = rdp_buffer;

      grd_egl_thread_upload (egl_thread,
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
  else
    {
      callback (stream, g_steal_pointer (&frame), TRUE, user_data);
    }
}

static void
take_pointer_data_from (GrdRdpFrame *src_frame,
                        GrdRdpFrame *dst_frame)
{
  g_assert (!dst_frame->pointer_bitmap);
  dst_frame->pointer_bitmap = g_steal_pointer (&src_frame->pointer_bitmap);

  dst_frame->pointer_hotspot_x = src_frame->pointer_hotspot_x;
  dst_frame->pointer_hotspot_y = src_frame->pointer_hotspot_y;
  dst_frame->pointer_width = src_frame->pointer_width;
  dst_frame->pointer_height = src_frame->pointer_height;
  dst_frame->pointer_is_hidden = src_frame->pointer_is_hidden;
  dst_frame->has_pointer_data = TRUE;
}

static void
on_frame_ready (GrdRdpPipeWireStream *stream,
                GrdRdpFrame          *frame,
                gboolean              success,
                gpointer              user_data)
{
  struct pw_buffer *buffer = user_data;
  GrdRdpFrame *pending_frame;

  g_assert (frame);

  if (frame->has_map)
    {
      munmap (frame->map, frame->map_size);
      frame->has_map = FALSE;
    }

  if (!success)
    goto out;

  g_mutex_lock (&stream->frame_mutex);
  pending_frame = g_steal_pointer (&stream->pending_frame);
  if (pending_frame)
    {
      if (!frame->buffer && pending_frame->buffer)
        frame->buffer = g_steal_pointer (&pending_frame->buffer);
      if (!frame->has_pointer_data && pending_frame->has_pointer_data)
        take_pointer_data_from (pending_frame, frame);

      grd_rdp_frame_unref (pending_frame);
    }
  stream->pending_frame = g_steal_pointer (&frame);
  g_mutex_unlock (&stream->frame_mutex);

out:
  if (buffer)
    pw_stream_queue_buffer (stream->pipewire_stream, buffer);

  g_source_set_ready_time (stream->render_source, 0);

  g_clear_pointer (&frame, grd_rdp_frame_unref);
}

static void
on_stream_process (void *user_data)
{
  GrdRdpPipeWireStream *stream = GRD_RDP_PIPEWIRE_STREAM (user_data);
  g_autoptr (GrdRdpFrame) frame = NULL;
  struct pw_buffer *last_pointer_buffer = NULL;
  struct pw_buffer *last_frame_buffer = NULL;
  struct pw_buffer *next_buffer;

  while ((next_buffer = pw_stream_dequeue_buffer (stream->pipewire_stream)))
    {
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

  frame = grd_rdp_frame_new (stream, on_frame_ready, last_frame_buffer);
  if (last_pointer_buffer)
    {
      process_mouse_pointer_bitmap (stream, last_pointer_buffer->buffer, frame);
      if (last_pointer_buffer != last_frame_buffer)
        pw_stream_queue_buffer (stream->pipewire_stream, last_pointer_buffer);
    }

  if (!last_frame_buffer)
    {
      GrdRdpFrameReadyCallback callback = frame->callback;
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
    g_signal_emit (stream, signals[CLOSED], 0);
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .error = on_core_error,
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

  grd_maybe_initialize_pipewire ();

  stream = g_object_new (GRD_TYPE_RDP_PIPEWIRE_STREAM, NULL);
  stream->session_rdp = session_rdp;
  stream->rdp_surface = rdp_surface;
  stream->src_node_id = src_node_id;

  create_render_source (stream, render_context);

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

  /* Setting a PipeWire stream inactive will wait for the data thread to end */
  if (stream->pipewire_stream)
    pw_stream_set_active (stream->pipewire_stream, false);

  egl_thread = grd_context_get_egl_thread (context);
  if (egl_thread && stream->pipewire_stream)
    sync_egl_thread (egl_thread);

  g_clear_pointer (&stream->pipewire_stream, pw_stream_destroy);

  g_clear_pointer (&stream->pipewire_core, pw_core_disconnect);
  g_clear_pointer (&stream->pipewire_context, pw_context_destroy);
  if (stream->pipewire_source)
    {
      g_source_destroy (stream->pipewire_source);
      g_clear_pointer (&stream->pipewire_source, g_source_unref);
    }

  if (stream->render_source)
    {
      g_source_destroy (stream->render_source);
      g_clear_pointer (&stream->render_source, g_source_unref);
    }

  grd_rdp_damage_detector_invalidate_surface (stream->rdp_surface->detector);
  g_clear_pointer (&stream->pending_frame, grd_rdp_frame_unref);

  release_all_buffers (stream);
  g_clear_object (&stream->buffer_pool);

  g_mutex_clear (&stream->frame_mutex);

  G_OBJECT_CLASS (grd_rdp_pipewire_stream_parent_class)->finalize (object);
}

static void
grd_rdp_pipewire_stream_init (GrdRdpPipeWireStream *stream)
{
  g_mutex_init (&stream->frame_mutex);
}

static void
grd_rdp_pipewire_stream_class_init (GrdRdpPipeWireStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_rdp_pipewire_stream_finalize;

  signals[CLOSED] = g_signal_new ("closed",
                                  G_TYPE_FROM_CLASS (klass),
                                  G_SIGNAL_RUN_LAST,
                                  0,
                                  NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}
