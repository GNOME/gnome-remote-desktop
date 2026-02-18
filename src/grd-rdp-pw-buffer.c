/*
 * Copyright (C) 2024 Pascal Nowack
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

#include "grd-rdp-pw-buffer.h"

#include <glib/gstdio.h>
#include <sys/mman.h>

#include "grd-drm-utils.h"
#include "grd-pipewire-utils.h"

typedef struct
{
  uint8_t *map;
  size_t map_size;

  uint8_t *data;
} GrdRdpMemFd;

struct _GrdRdpPwBuffer
{
  struct pw_stream *pw_stream;
  struct pw_buffer *pw_buffer;

  GrdRdpBufferType buffer_type;
  GrdRdpPwBufferDmaBufInfo *dma_buf_info;

  GMutex buffer_mutex;
  gboolean is_locked;

  GrdRdpMemFd mem_fd;

  int device_fd;
  int acquire_syncobj_fd;
  uint64_t acquire_point;
  int release_syncobj_fd;
  uint64_t release_point;
  gboolean needs_release;
};

GrdRdpBufferType
grd_rdp_pw_buffer_get_buffer_type (GrdRdpPwBuffer *rdp_pw_buffer)
{
  return rdp_pw_buffer->buffer_type;
}

const GrdRdpPwBufferDmaBufInfo *
grd_rdp_pw_buffer_get_dma_buf_info (GrdRdpPwBuffer *rdp_pw_buffer)
{
  return rdp_pw_buffer->dma_buf_info;
}

uint8_t *
grd_rdp_pw_buffer_get_mapped_data (GrdRdpPwBuffer *rdp_pw_buffer,
                                   int32_t        *stride)
{
  struct pw_buffer *pw_buffer = rdp_pw_buffer->pw_buffer;
  struct spa_buffer *spa_buffer = pw_buffer->buffer;
  GrdRdpMemFd *mem_fd = &rdp_pw_buffer->mem_fd;

  g_assert (mem_fd->data);

  *stride = spa_buffer->datas[0].chunk->stride;

  return mem_fd->data;
}

gboolean
grd_rdp_pw_buffer_has_syncobjs (GrdRdpPwBuffer *rdp_pw_buffer)
{
  return rdp_pw_buffer->acquire_syncobj_fd != -1 &&
         rdp_pw_buffer->release_syncobj_fd != -1;
}

void
grd_rdp_pw_buffer_queue_pw_buffer (GrdRdpPwBuffer *rdp_pw_buffer)
{
  if (rdp_pw_buffer->release_syncobj_fd != -1 &&
      rdp_pw_buffer->needs_release)
    {
      int release_syncobj_fd = rdp_pw_buffer->release_syncobj_fd;
      uint64_t release_timeline_point = rdp_pw_buffer->release_point;
      g_autoptr (GError) error = NULL;

      if (!grd_signal_drm_timeline_point (rdp_pw_buffer->device_fd,
                                          release_syncobj_fd,
                                          release_timeline_point,
                                          &error))
        g_warning ("Failed to signal release point: %s", error->message);

      rdp_pw_buffer->needs_release = FALSE;
    }

  pw_stream_queue_buffer (rdp_pw_buffer->pw_stream, rdp_pw_buffer->pw_buffer);
}

void
grd_rdp_pw_buffer_ensure_unlocked (GrdRdpPwBuffer *rdp_pw_buffer)
{
  g_mutex_lock (&rdp_pw_buffer->buffer_mutex);
  g_mutex_unlock (&rdp_pw_buffer->buffer_mutex);
}

void
grd_rdp_pw_buffer_acquire_lock (GrdRdpPwBuffer *rdp_pw_buffer)
{
  g_mutex_lock (&rdp_pw_buffer->buffer_mutex);
  g_assert (!rdp_pw_buffer->is_locked);
  rdp_pw_buffer->is_locked = TRUE;
}

void
grd_rdp_pw_buffer_maybe_release_lock (GrdRdpPwBuffer *rdp_pw_buffer)
{
  if (!rdp_pw_buffer->is_locked)
    return;

  rdp_pw_buffer->is_locked = FALSE;
  g_mutex_unlock (&rdp_pw_buffer->buffer_mutex);
}

static enum spa_data_type
get_pw_buffer_type (struct pw_buffer *pw_buffer)
{
  struct spa_buffer *spa_buffer = pw_buffer->buffer;

  return spa_buffer->datas[0].type;
}

static gboolean
is_supported_dma_buf_buffer (struct pw_buffer  *pw_buffer,
                             GError           **error)
{
  struct spa_buffer *spa_buffer = pw_buffer->buffer;
  size_t i;
  int n_planes = 0;

  for (i = 0; i < spa_buffer->n_datas; i++)
    {
      if (spa_buffer->datas[i].type == SPA_DATA_DmaBuf)
        n_planes++;
    }

  if (n_planes != 1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unsupported dma-buf: Expected exactly 1 plane for format");
      return FALSE;
    }

  return TRUE;
}

static GrdRdpPwBufferDmaBufInfo *
dma_buf_info_new (struct pw_buffer *pw_buffer)
{
  struct spa_buffer *spa_buffer = pw_buffer->buffer;
  GrdRdpPwBufferDmaBufInfo *dma_buf_info;

  dma_buf_info = g_new0 (GrdRdpPwBufferDmaBufInfo, 1);
  dma_buf_info->fd = spa_buffer->datas[0].fd;
  dma_buf_info->offset = spa_buffer->datas[0].chunk->offset;
  dma_buf_info->stride = spa_buffer->datas[0].chunk->stride;

  return dma_buf_info;
}

static gboolean
try_mmap_buffer (GrdRdpPwBuffer  *rdp_pw_buffer,
                 GError         **error)
{
  struct pw_buffer *pw_buffer = rdp_pw_buffer->pw_buffer;
  struct spa_buffer *spa_buffer = pw_buffer->buffer;
  GrdRdpMemFd *mem_fd = &rdp_pw_buffer->mem_fd;

  mem_fd->map_size = spa_buffer->datas[0].maxsize +
                     spa_buffer->datas[0].mapoffset;
  mem_fd->map = mmap (NULL, mem_fd->map_size, PROT_READ, MAP_PRIVATE,
                      spa_buffer->datas[0].fd, 0);
  if (mem_fd->map == MAP_FAILED)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to mmap buffer: %s", g_strerror (errno));
      return FALSE;
    }
  mem_fd->data = SPA_MEMBER (mem_fd->map, spa_buffer->datas[0].mapoffset,
                             uint8_t);

  return TRUE;
}

GrdRdpPwBuffer *
grd_rdp_pw_buffer_new (struct pw_stream  *pw_stream,
                       struct pw_buffer  *pw_buffer,
                       int                device_fd,
                       GError           **error)
{
  g_autoptr (GrdRdpPwBuffer) rdp_pw_buffer = NULL;

  rdp_pw_buffer = g_new0 (GrdRdpPwBuffer, 1);
  rdp_pw_buffer->pw_stream = pw_stream;
  rdp_pw_buffer->pw_buffer = pw_buffer;
  rdp_pw_buffer->device_fd = device_fd;
  rdp_pw_buffer->acquire_syncobj_fd = -1;
  rdp_pw_buffer->release_syncobj_fd = -1;

  g_mutex_init (&rdp_pw_buffer->buffer_mutex);

  switch (get_pw_buffer_type (pw_buffer))
    {
    case SPA_DATA_MemFd:
      rdp_pw_buffer->buffer_type = GRD_RDP_BUFFER_TYPE_MEM_FD;
      break;
    case SPA_DATA_DmaBuf:
      rdp_pw_buffer->buffer_type = GRD_RDP_BUFFER_TYPE_DMA_BUF;
      break;
    default:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "PipeWire buffer contains invalid data type 0x%08X",
                   get_pw_buffer_type (pw_buffer));
      return NULL;
    }

  if (get_pw_buffer_type (pw_buffer) == SPA_DATA_DmaBuf &&
      !is_supported_dma_buf_buffer (pw_buffer, error))
    return NULL;

  if (get_pw_buffer_type (pw_buffer) == SPA_DATA_DmaBuf)
    {
      struct spa_buffer *spa_buffer = rdp_pw_buffer->pw_buffer->buffer;
      int acquire_syncobj_fd, release_syncobj_fd;

      rdp_pw_buffer->dma_buf_info = dma_buf_info_new (pw_buffer);

      if (grd_spa_buffer_find_syncobj_fds (spa_buffer,
                                           &acquire_syncobj_fd,
                                           &release_syncobj_fd))
        {
          rdp_pw_buffer->acquire_syncobj_fd = dup (acquire_syncobj_fd);
          rdp_pw_buffer->release_syncobj_fd = dup (release_syncobj_fd);

          if (rdp_pw_buffer->acquire_syncobj_fd == -1 ||
              rdp_pw_buffer->release_syncobj_fd == -1)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           "Failed to duplicate syncobj file descriptors: %s",
                           g_strerror (errno));
              return NULL;
            }
        }
    }

  if (get_pw_buffer_type (pw_buffer) == SPA_DATA_MemFd &&
      !try_mmap_buffer (rdp_pw_buffer, error))
    return NULL;

  return g_steal_pointer (&rdp_pw_buffer);
}

static void
maybe_unmap_buffer (GrdRdpPwBuffer* rdp_pw_buffer)
{
  GrdRdpMemFd *mem_fd = &rdp_pw_buffer->mem_fd;

  if (!mem_fd->data)
    return;

  munmap (mem_fd->map, mem_fd->map_size);
  mem_fd->data = NULL;
}

void
grd_rdp_pw_buffer_free (GrdRdpPwBuffer *rdp_pw_buffer)
{
  maybe_unmap_buffer (rdp_pw_buffer);
  g_clear_pointer (&rdp_pw_buffer->dma_buf_info, g_free);

  g_clear_fd (&rdp_pw_buffer->acquire_syncobj_fd, NULL);
  g_clear_fd (&rdp_pw_buffer->release_syncobj_fd, NULL);

  g_mutex_clear (&rdp_pw_buffer->buffer_mutex);
  g_free (rdp_pw_buffer);
}

void
grd_rdp_pw_buffer_update_timeline_points (GrdRdpPwBuffer *rdp_pw_buffer)
{
  struct spa_buffer *spa_buffer = rdp_pw_buffer->pw_buffer->buffer;
  struct spa_meta_sync_timeline *sync_timeline;

  sync_timeline =
    spa_buffer_find_meta_data (spa_buffer, SPA_META_SyncTimeline,
                               sizeof (struct spa_meta_sync_timeline));
  if (!sync_timeline)
    return;

  rdp_pw_buffer->acquire_point = sync_timeline->acquire_point;
  rdp_pw_buffer->release_point = sync_timeline->release_point;
  rdp_pw_buffer->needs_release = TRUE;
}

void
grd_rdp_pw_buffer_get_acquire_timeline_data (GrdRdpPwBuffer *rdp_pw_buffer,
                                             int            *syncobj_fd,
                                             uint64_t       *timeline_point)
{
  *syncobj_fd = rdp_pw_buffer->acquire_syncobj_fd;
  *timeline_point = rdp_pw_buffer->acquire_point;
}
