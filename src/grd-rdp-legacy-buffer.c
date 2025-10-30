/*
 * Copyright (C) 2022 Pascal Nowack
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

#include "grd-rdp-legacy-buffer.h"

#include "grd-context.h"
#include "grd-egl-thread.h"
#include "grd-hwaccel-nvidia.h"
#include "grd-rdp-buffer-pool.h"
#include "grd-rdp-renderer.h"
#include "grd-rdp-server.h"
#include "grd-rdp-surface.h"
#include "grd-session-rdp.h"
#include "grd-utils.h"

typedef struct
{
  GrdHwAccelNvidia *hwaccel_nvidia;
  CUgraphicsResource cuda_resource;
  CUstream cuda_stream;
  gboolean is_mapped;
} ClearBufferData;

typedef struct
{
  GrdHwAccelNvidia *hwaccel_nvidia;
  GrdRdpLegacyBuffer *buffer;
} AllocateBufferData;

typedef struct
{
  GrdHwAccelNvidia *hwaccel_nvidia;
  CUgraphicsResource cuda_resource;
  CUstream cuda_stream;
} UnmapBufferData;

struct _GrdRdpLegacyBuffer
{
  GrdRdpBufferPool *buffer_pool;

  uint32_t stride;

  uint8_t *local_data;

  uint32_t pbo;

  CUgraphicsResource cuda_resource;
  CUstream cuda_stream;
  CUdeviceptr mapped_cuda_pointer;
};

static gboolean
cuda_allocate_buffer (gpointer user_data,
                      uint32_t pbo)
{
  AllocateBufferData *data = user_data;
  GrdRdpLegacyBuffer *buffer = data->buffer;
  gboolean success;

  success = grd_hwaccel_nvidia_register_read_only_gl_buffer (data->hwaccel_nvidia,
                                                             &buffer->cuda_resource,
                                                             pbo);
  if (success)
    buffer->pbo = pbo;

  return success;
}

static void
resources_ready (gboolean success,
                 gpointer user_data)
{
  GrdSyncPoint *sync_point = user_data;

  if (success)
    g_debug ("[RDP] Allocating GL resources was successful");
  else
    g_warning ("[RDP] Failed to allocate GL resources");

  grd_sync_point_complete (sync_point, success);
}

static GrdHwAccelNvidia *
hwaccel_nvidia_from_buffer (GrdRdpLegacyBuffer *buffer)
{
  GrdRdpSurface *rdp_surface =
    grd_rdp_buffer_pool_get_surface (buffer->buffer_pool);
  GrdSessionRdp *session_rdp =
    grd_rdp_renderer_get_session (rdp_surface->renderer);
  GrdRdpServer *rdp_server = grd_session_rdp_get_server (session_rdp);

  return grd_rdp_server_get_hwaccel_nvidia (rdp_server);
}

static GrdEglThread *
egl_thread_from_buffer (GrdRdpLegacyBuffer *buffer)
{
  GrdRdpSurface *rdp_surface =
    grd_rdp_buffer_pool_get_surface (buffer->buffer_pool);
  GrdSessionRdp *session_rdp =
    grd_rdp_renderer_get_session (rdp_surface->renderer);
  GrdRdpServer *rdp_server = grd_session_rdp_get_server (session_rdp);
  GrdContext *context = grd_rdp_server_get_context (rdp_server);

  return grd_context_get_egl_thread (context);
}

GrdRdpLegacyBuffer *
grd_rdp_legacy_buffer_new (GrdRdpBufferPool *buffer_pool,
                           uint32_t          height,
                           uint32_t          stride,
                           gboolean          preallocate_on_gpu)
{
  GrdRdpSurface *rdp_surface = grd_rdp_buffer_pool_get_surface (buffer_pool);
  GrdHwAccelNvidia *hwaccel_nvidia;
  g_autoptr (GrdRdpLegacyBuffer) buffer = NULL;
  gboolean success = TRUE;

  buffer = g_new0 (GrdRdpLegacyBuffer, 1);
  buffer->buffer_pool = buffer_pool;

  buffer->cuda_stream = rdp_surface->cuda_stream;

  buffer->stride = stride;
  buffer->local_data = g_malloc0 (stride * height * sizeof (uint8_t));

  hwaccel_nvidia = hwaccel_nvidia_from_buffer (buffer);
  if (preallocate_on_gpu && hwaccel_nvidia)
    {
      GrdEglThread *egl_thread = egl_thread_from_buffer (buffer);
      AllocateBufferData data = {};
      GrdSyncPoint sync_point = {};

      g_assert (egl_thread);

      grd_sync_point_init (&sync_point);
      data.hwaccel_nvidia = hwaccel_nvidia;
      data.buffer = buffer;

      grd_egl_thread_allocate (egl_thread,
                               height,
                               stride,
                               cuda_allocate_buffer,
                               &data,
                               resources_ready,
                               &sync_point,
                               NULL);

      success = grd_sync_point_wait_for_completion (&sync_point);
      grd_sync_point_clear (&sync_point);
    }

  if (!success)
    return NULL;

  return g_steal_pointer (&buffer);
}

static void
cuda_deallocate_buffer (gpointer user_data)
{
  ClearBufferData *data = user_data;

  if (data->is_mapped)
    {
      grd_hwaccel_nvidia_unmap_cuda_resource (data->hwaccel_nvidia,
                                              data->cuda_resource,
                                              data->cuda_stream);
    }

  grd_hwaccel_nvidia_unregister_cuda_resource (data->hwaccel_nvidia,
                                               data->cuda_resource,
                                               data->cuda_stream);
}

void
grd_rdp_legacy_buffer_free (GrdRdpLegacyBuffer *buffer)
{
  if (buffer->cuda_resource)
    {
      GrdHwAccelNvidia *hwaccel_nvidia = hwaccel_nvidia_from_buffer (buffer);
      GrdEglThread *egl_thread = egl_thread_from_buffer (buffer);
      ClearBufferData *data;

      data = g_new0 (ClearBufferData, 1);
      data->hwaccel_nvidia = hwaccel_nvidia;
      data->cuda_resource = buffer->cuda_resource;
      data->cuda_stream = buffer->cuda_stream;
      data->is_mapped = !!buffer->mapped_cuda_pointer;
      grd_egl_thread_deallocate (egl_thread,
                                 buffer->pbo,
                                 cuda_deallocate_buffer,
                                 data,
                                 NULL, data, g_free);

      buffer->mapped_cuda_pointer = 0;
      buffer->cuda_resource = NULL;
      buffer->pbo = 0;
    }

  g_clear_pointer (&buffer->local_data, g_free);

  g_free (buffer);
}

uint32_t
grd_rdp_legacy_buffer_get_stride (GrdRdpLegacyBuffer *buffer)
{
  return buffer->stride;
}

uint8_t *
grd_rdp_legacy_buffer_get_local_data (GrdRdpLegacyBuffer *buffer)
{
  return buffer->local_data;
}

uint32_t
grd_rdp_legacy_buffer_get_pbo (GrdRdpLegacyBuffer *buffer)
{
  return buffer->pbo;
}

CUdeviceptr
grd_rdp_legacy_buffer_get_mapped_cuda_pointer (GrdRdpLegacyBuffer *buffer)
{
  return buffer->mapped_cuda_pointer;
}

void
grd_rdp_legacy_buffer_release (GrdRdpLegacyBuffer *buffer)
{
  grd_rdp_buffer_pool_release_buffer (buffer->buffer_pool, buffer);
}

gboolean
grd_rdp_legacy_buffer_register_read_only_gl_buffer (GrdRdpLegacyBuffer *buffer,
                                                    uint32_t            pbo)
{
  GrdHwAccelNvidia *hwaccel_nvidia = hwaccel_nvidia_from_buffer (buffer);
  gboolean success;

  success =
    grd_hwaccel_nvidia_register_read_only_gl_buffer (hwaccel_nvidia,
                                                     &buffer->cuda_resource,
                                                     pbo);
  if (success)
    buffer->pbo = pbo;

  return success;
}

gboolean
grd_rdp_legacy_buffer_map_cuda_resource (GrdRdpLegacyBuffer *buffer)
{
  GrdHwAccelNvidia *hwaccel_nvidia = hwaccel_nvidia_from_buffer (buffer);
  size_t mapped_size = 0;

  return grd_hwaccel_nvidia_map_cuda_resource (hwaccel_nvidia,
                                               buffer->cuda_resource,
                                               &buffer->mapped_cuda_pointer,
                                               &mapped_size,
                                               buffer->cuda_stream);
}

void
grd_rdp_legacy_buffer_unmap_cuda_resource (GrdRdpLegacyBuffer *buffer)
{
  GrdHwAccelNvidia *hwaccel_nvidia = hwaccel_nvidia_from_buffer (buffer);

  if (!buffer->mapped_cuda_pointer)
    return;

  grd_hwaccel_nvidia_unmap_cuda_resource (hwaccel_nvidia,
                                          buffer->cuda_resource,
                                          buffer->cuda_stream);
  buffer->mapped_cuda_pointer = 0;
}

static gboolean
cuda_unmap_resource (gpointer user_data)
{
  UnmapBufferData *data = user_data;

  grd_hwaccel_nvidia_unmap_cuda_resource (data->hwaccel_nvidia,
                                          data->cuda_resource,
                                          data->cuda_stream);

  return TRUE;
}

void
grd_rdp_legacy_buffer_queue_resource_unmap (GrdRdpLegacyBuffer *buffer)
{
  GrdEglThread *egl_thread = egl_thread_from_buffer (buffer);
  GrdHwAccelNvidia *hwaccel_nvidia = hwaccel_nvidia_from_buffer (buffer);
  UnmapBufferData *data;

  if (!buffer->mapped_cuda_pointer)
    return;

  data = g_new0 (UnmapBufferData, 1);
  data->hwaccel_nvidia = hwaccel_nvidia;
  data->cuda_resource = buffer->cuda_resource;
  data->cuda_stream = buffer->cuda_stream;
  grd_egl_thread_run_custom_task (egl_thread,
                                  cuda_unmap_resource,
                                  data,
                                  NULL, data, g_free);

  /*
   * The mapped CUDA pointer indicates whether the resource is mapped, but
   * it is itself not needed to unmap the resource.
   */
  buffer->mapped_cuda_pointer = 0;
}
