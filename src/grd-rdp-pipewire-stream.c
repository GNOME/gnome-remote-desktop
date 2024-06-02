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

#include "grd-context.h"
#include "grd-egl-thread.h"
#include "grd-hwaccel-nvidia.h"
#include "grd-pipewire-utils.h"
#include "grd-rdp-buffer.h"
#include "grd-rdp-buffer-pool.h"
#include "grd-rdp-cursor-renderer.h"
#include "grd-rdp-damage-detector.h"
#include "grd-rdp-pw-buffer.h"
#include "grd-rdp-session-metrics.h"
#include "grd-rdp-surface.h"
#include "grd-rdp-surface-renderer.h"
#include "grd-utils.h"

#define DEFAULT_BUFFER_POOL_SIZE 5
#define MAX_FORMAT_PARAMS 2

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

struct _GrdRdpFrame
{
  gatomicrefcount refcount;

  GrdRdpBuffer *buffer;

  GrdRdpPipeWireStream *stream;
  GrdRdpFrameReadyCallback callback;
  gpointer callback_user_data;
};

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
  GrdRdpCursorRenderer *cursor_renderer;
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

  struct pw_stream *pipewire_stream;
  struct spa_hook pipewire_stream_listener;

  GHashTable *pipewire_buffers;

  uint32_t src_node_id;

  gboolean pending_resize;
  struct spa_video_info_raw spa_format;
};

G_DEFINE_TYPE (GrdRdpPipeWireStream, grd_rdp_pipewire_stream,
               G_TYPE_OBJECT)

static void grd_rdp_frame_unref (GrdRdpFrame *frame);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdRdpFrame, grd_rdp_frame_unref)

static void
acquire_pipewire_buffer_lock (GrdRdpPipeWireStream *stream,
                              struct pw_buffer     *buffer)
{
  GrdRdpPwBuffer *rdp_pw_buffer = NULL;

  if (!g_hash_table_lookup_extended (stream->pipewire_buffers, buffer,
                                     NULL, (gpointer *) &rdp_pw_buffer))
    g_assert_not_reached ();

  grd_rdp_pw_buffer_acquire_lock (rdp_pw_buffer);
}

static void
maybe_release_pipewire_buffer_lock (GrdRdpPipeWireStream *stream,
                                    struct pw_buffer     *buffer)
{
  GrdRdpPwBuffer *rdp_pw_buffer = NULL;

  if (!g_hash_table_lookup_extended (stream->pipewire_buffers, buffer,
                                     NULL, (gpointer *) &rdp_pw_buffer))
    g_assert_not_reached ();

  grd_rdp_pw_buffer_maybe_release_lock (rdp_pw_buffer);
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
      g_clear_pointer (&frame->buffer, grd_rdp_buffer_release);
      g_free (frame);
    }
}

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
                       SPA_POD_CHOICE_RANGE_Fraction (&max_framerate,
                                                      &min_framerate,
                                                      &max_framerate), 0);
}

static uint32_t
add_format_params (GrdRdpPipeWireStream        *stream,
                   const GrdRdpVirtualMonitor  *virtual_monitor,
                   struct spa_pod_builder      *pod_builder,
                   const struct spa_pod       **params,
                   uint32_t                     n_available_params)
{
  GrdSession *session = GRD_SESSION (stream->session_rdp);
  GrdContext *context = grd_session_get_context (session);
  GrdEglThread *egl_thread = grd_context_get_egl_thread (context);
  GrdRdpSurfaceRenderer *surface_renderer =
    grd_rdp_surface_get_surface_renderer (stream->rdp_surface);
  uint32_t refresh_rate =
    grd_rdp_surface_renderer_get_refresh_rate (surface_renderer);
  struct spa_pod_frame format_frame;
  enum spa_video_format spa_format = SPA_VIDEO_FORMAT_BGRx;
  gboolean need_fallback_format = FALSE;
  uint32_t n_params = 0;

  g_assert (n_available_params >= 2);

  spa_pod_builder_push_object (pod_builder, &format_frame,
                               SPA_TYPE_OBJECT_Format,
                               SPA_PARAM_EnumFormat);
  add_common_format_params (pod_builder, spa_format, virtual_monitor,
                            refresh_rate);

  if (egl_thread && !stream->rdp_surface->hwaccel_nvidia)
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

          spa_pod_builder_prop (pod_builder,
                                SPA_FORMAT_VIDEO_modifier,
                                (SPA_POD_PROP_FLAG_MANDATORY |
                                 SPA_POD_PROP_FLAG_DONT_FIXATE));

          spa_pod_builder_push_choice (pod_builder, &modifier_frame,
                                       SPA_CHOICE_Enum, 0);
          spa_pod_builder_long (pod_builder, modifiers[0]);

          for (i = 0; i < n_modifiers; i++)
            {
              uint64_t modifier = modifiers[i];

              spa_pod_builder_long (pod_builder, modifier);
            }
          spa_pod_builder_long (pod_builder, DRM_FORMAT_MOD_INVALID);
          spa_pod_builder_pop (pod_builder, &modifier_frame);

          need_fallback_format = TRUE;
        }
    }

  params[n_params++] = spa_pod_builder_pop (pod_builder, &format_frame);

  if (need_fallback_format)
    {
      spa_pod_builder_push_object (pod_builder, &format_frame,
                                   SPA_TYPE_OBJECT_Format,
                                   SPA_PARAM_EnumFormat);
      add_common_format_params (pod_builder, spa_format, virtual_monitor,
                                refresh_rate);
      params[n_params++] = spa_pod_builder_pop (pod_builder, &format_frame);
    }

  return n_params;
}

void
grd_rdp_pipewire_stream_resize (GrdRdpPipeWireStream *stream,
                                GrdRdpVirtualMonitor *virtual_monitor)
{
  uint8_t params_buffer[1024];
  struct spa_pod_builder pod_builder;
  const struct spa_pod *params[MAX_FORMAT_PARAMS] = {};
  uint32_t n_params = 0;

  stream->pending_resize = TRUE;

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));

  n_params += add_format_params (stream, virtual_monitor, &pod_builder,
                                 params, MAX_FORMAT_PARAMS);

  g_assert (n_params > 0);
  pw_stream_update_params (stream->pipewire_stream, params, n_params);
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
  GrdRdpSurfaceRenderer *surface_renderer;

  surface_renderer = grd_rdp_surface_get_surface_renderer (stream->rdp_surface);
  grd_rdp_surface_renderer_reset (surface_renderer);
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
  stride = width * 4;

  g_debug ("[RDP] Stream parameters changed. New surface size: [%u, %u]",
           width, height);

  if (egl_thread)
    sync_egl_thread (egl_thread);
  release_all_buffers (stream);
  grd_rdp_surface_reset (stream->rdp_surface);

  if (!grd_rdp_damage_detector_resize_surface (stream->rdp_surface->detector,
                                               width, height) ||
      !grd_rdp_buffer_pool_resize_buffers (stream->buffer_pool, height, stride))
    {
      grd_session_rdp_notify_error (
        stream->session_rdp, GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED);
      return;
    }

  grd_rdp_surface_set_size (stream->rdp_surface, width, height);
  g_signal_emit (stream, signals[VIDEO_RESIZED], 0, width, height);
  stream->pending_resize = FALSE;

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));

  allowed_buffer_types = 1 << SPA_DATA_MemFd;
  if (egl_thread && !stream->rdp_surface->hwaccel_nvidia)
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
handle_graphics_subsystem_failure (GrdRdpPipeWireStream *stream)
{
  stream->dequeuing_disallowed = TRUE;

  grd_session_rdp_notify_error (stream->session_rdp,
                                GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED);
}

static void
on_stream_add_buffer (void             *user_data,
                      struct pw_buffer *buffer)
{
  GrdRdpPipeWireStream *stream = user_data;
  GrdRdpPwBuffer *rdp_pw_buffer;
  g_autoptr (GError) error = NULL;

  rdp_pw_buffer = grd_rdp_pw_buffer_new (buffer, &error);
  if (!rdp_pw_buffer)
    {
      g_warning ("[RDP] Failed to add PipeWire buffer: %s", error->message);
      handle_graphics_subsystem_failure (stream);
      return;
    }

  g_hash_table_insert (stream->pipewire_buffers, buffer, rdp_pw_buffer);
}

static void
on_stream_remove_buffer (void             *user_data,
                         struct pw_buffer *buffer)
{
  GrdRdpPipeWireStream *stream = user_data;
  GrdRdpPwBuffer *rdp_pw_buffer = NULL;
  g_autoptr (GMutexLocker) locker = NULL;

  if (!g_hash_table_lookup_extended (stream->pipewire_buffers, buffer,
                                     NULL, (gpointer *) &rdp_pw_buffer))
    return;

  locker = g_mutex_locker_new (&stream->dequeue_mutex);
  grd_rdp_pw_buffer_ensure_unlocked (rdp_pw_buffer);

  g_hash_table_remove (stream->pipewire_buffers, rdp_pw_buffer);
}

static void
process_mouse_cursor_data (GrdRdpPipeWireStream *stream,
                           struct spa_buffer    *buffer)
{
  struct spa_meta_cursor *spa_meta_cursor;
  struct spa_meta_bitmap *spa_meta_bitmap;
  GrdRdpCursorUpdate *cursor_update = NULL;
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

      cursor_update = g_new0 (GrdRdpCursorUpdate, 1);
      cursor_update->update_type = GRD_RDP_CURSOR_UPDATE_TYPE_NORMAL;
      cursor_update->hotspot_x = spa_meta_cursor->hotspot.x;
      cursor_update->hotspot_y = spa_meta_cursor->hotspot.y;
      cursor_update->width = spa_meta_bitmap->size.width;
      cursor_update->height = spa_meta_bitmap->size.height;
      cursor_update->bitmap =
        g_memdup2 (buf, spa_meta_bitmap->size.height * spa_meta_bitmap->stride);
    }
  else if (spa_meta_bitmap)
    {
      cursor_update = g_new0 (GrdRdpCursorUpdate, 1);
      cursor_update->update_type = GRD_RDP_CURSOR_UPDATE_TYPE_HIDDEN;
    }

  if (cursor_update)
    {
      grd_rdp_cursor_renderer_submit_cursor_update (stream->cursor_renderer,
                                                    cursor_update);
    }
}

static void
on_frame_ready (GrdRdpPipeWireStream *stream,
                GrdRdpFrame          *frame,
                gboolean              success,
                gpointer              user_data)
{
  struct pw_buffer *buffer = user_data;
  GrdRdpSurface *rdp_surface = stream->rdp_surface;
  GrdRdpSurfaceRenderer *surface_renderer;

  g_assert (frame);
  g_assert (buffer);

  if (!success)
    goto out;

  g_assert (frame->buffer);

  if (!stream->pending_resize)
    {
      GrdRdpSessionMetrics *session_metrics =
        grd_session_rdp_get_session_metrics (stream->session_rdp);

      grd_rdp_session_metrics_notify_frame_reception (session_metrics,
                                                      rdp_surface);
    }

  surface_renderer = grd_rdp_surface_get_surface_renderer (rdp_surface);
  grd_rdp_surface_renderer_submit_buffer (surface_renderer,
                                          g_steal_pointer (&frame->buffer));
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
  int32_t src_stride;
  int dst_stride;

  g_assert (buffer->datas[0].chunk->size > 0);

  height = stream->spa_format.size.height;
  width = stream->spa_format.size.width;
  src_stride = buffer->datas[0].chunk->stride;
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
      GrdRdpPwBuffer *rdp_pw_buffer = NULL;
      AllocateBufferData *allocate_buffer_data;
      RealizeBufferData *realize_buffer_data;
      GrdRdpBuffer *rdp_buffer;
      void *src_data;
      uint32_t pbo;
      uint8_t *data_to_upload;

      acquire_pipewire_buffer_lock (stream, pw_buffer);
      if (!g_hash_table_lookup_extended (stream->pipewire_buffers, pw_buffer,
                                         NULL, (gpointer *) &rdp_pw_buffer))
        g_assert_not_reached ();

      src_data = grd_rdp_pw_buffer_get_mapped_data (rdp_pw_buffer, &src_stride);

      frame->buffer = grd_rdp_buffer_pool_acquire (stream->buffer_pool);
      if (!frame->buffer)
        {
          maybe_release_pipewire_buffer_lock (stream, pw_buffer);
          grd_session_rdp_notify_error (stream->session_rdp,
                                        GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED);
          callback (stream, g_steal_pointer (&frame), FALSE, user_data);
          return;
        }
      rdp_buffer = frame->buffer;
      dst_stride = grd_rdp_buffer_get_stride (rdp_buffer);
      pbo = grd_rdp_buffer_get_pbo (rdp_buffer);

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

          data_to_upload = grd_rdp_buffer_get_local_data (rdp_buffer);
        }

      if (!hwaccel_nvidia)
        {
          maybe_release_pipewire_buffer_lock (stream, pw_buffer);
          callback (stream, g_steal_pointer (&frame), TRUE, user_data);
          return;
        }

      unmap_cuda_resources (egl_thread, rdp_buffer);

      allocate_buffer_data = g_new0 (AllocateBufferData, 1);
      allocate_buffer_data->rdp_buffer = rdp_buffer;

      realize_buffer_data = g_new0 (RealizeBufferData, 1);
      realize_buffer_data->rdp_buffer = rdp_buffer;

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
      dst_stride = grd_rdp_buffer_get_stride (rdp_buffer);

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
      process_mouse_cursor_data (stream, last_pointer_buffer->buffer);
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

static gboolean
connect_to_stream (GrdRdpPipeWireStream        *stream,
                   const GrdRdpVirtualMonitor  *virtual_monitor,
                   GError                     **error)
{
  struct pw_stream *pipewire_stream;
  uint8_t params_buffer[1024];
  struct spa_pod_builder pod_builder;
  const struct spa_pod *params[MAX_FORMAT_PARAMS] = {};
  uint32_t n_params = 0;
  int ret;

  pipewire_stream = pw_stream_new (stream->pipewire_core,
                                   "grd-rdp-pipewire-stream",
                                   NULL);

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));

  n_params += add_format_params (stream, virtual_monitor, &pod_builder,
                                 params, MAX_FORMAT_PARAMS);

  stream->pipewire_stream = pipewire_stream;

  pw_stream_add_listener (pipewire_stream,
                          &stream->pipewire_stream_listener,
                          &stream_events,
                          stream);

  g_assert (n_params > 0);
  ret = pw_stream_connect (stream->pipewire_stream,
                           PW_DIRECTION_INPUT,
                           stream->src_node_id,
                           (PW_STREAM_FLAG_RT_PROCESS |
                            PW_STREAM_FLAG_AUTOCONNECT),
                           params, n_params);
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
                             GrdRdpCursorRenderer        *cursor_renderer,
                             GrdHwAccelNvidia            *hwaccel_nvidia,
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
  stream->cursor_renderer = cursor_renderer;
  stream->rdp_surface = rdp_surface;
  stream->src_node_id = src_node_id;

  stream->buffer_pool = grd_rdp_buffer_pool_new (egl_thread,
                                                 hwaccel_nvidia,
                                                 rdp_surface->cuda_stream,
                                                 DEFAULT_BUFFER_POOL_SIZE);

  if (egl_thread && !hwaccel_nvidia)
    stream->egl_slot = grd_egl_thread_acquire_slot (egl_thread);

  pw_init (NULL, NULL);

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

  if (!connect_to_stream (stream, virtual_monitor, error))
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

  grd_rdp_damage_detector_invalidate_surface (stream->rdp_surface->detector);

  release_all_buffers (stream);
  g_clear_object (&stream->buffer_pool);

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
  stream->pending_resize = TRUE;

  stream->pipewire_buffers =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) grd_rdp_pw_buffer_free);

  g_mutex_init (&stream->dequeue_mutex);
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
