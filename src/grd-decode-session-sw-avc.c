/*
 * Copyright (C) 2025 Pascal Nowack
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

#include "grd-decode-session-sw-avc.h"

#include <fcntl.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/h264.h>
#include <glib/gstdio.h>
#include <sys/mman.h>

#include "grd-sample-buffer.h"
#include "grd-utils.h"

#define MAX_BASE_SIZE (1 << 18)

typedef struct
{
  uint8_t *data;

  GrdSampleBuffer *sample_buffer;
} BitstreamBuffer;

struct _GrdDecodeSessionSwAVC
{
  GrdDecodeSession parent;

  H264_CONTEXT *h264_context;
  uint32_t surface_width;
  uint32_t surface_height;

  GHashTable *bitstream_buffers;
  GHashTable *pipewire_buffers;

  GMutex pending_frames_mutex;
  GHashTable *pending_frames;
};

G_DEFINE_TYPE (GrdDecodeSessionSwAVC, grd_decode_session_sw_avc,
               GRD_TYPE_DECODE_SESSION)

static BitstreamBuffer *
bitstream_buffer_new (size_t size)
{
  BitstreamBuffer *bitstream_buffer;

  bitstream_buffer = g_new0 (BitstreamBuffer, 1);

  bitstream_buffer->data = g_malloc0 (size);
  bitstream_buffer->sample_buffer =
    grd_sample_buffer_new (bitstream_buffer->data, size);

  return bitstream_buffer;
}

static void
bitstream_buffer_free (BitstreamBuffer *bitstream_buffer)
{
  g_clear_pointer (&bitstream_buffer->sample_buffer, grd_sample_buffer_free);
  g_clear_pointer (&bitstream_buffer->data, g_free);

  g_free (bitstream_buffer);
}

static void
grd_decode_session_sw_avc_get_drm_format_modifiers (GrdDecodeSession  *decode_session,
                                                    uint32_t           drm_format,
                                                    uint32_t          *out_n_modifiers,
                                                    uint64_t         **out_modifiers)
{
  *out_n_modifiers = 0;
  *out_modifiers = NULL;
}

static gboolean
grd_decode_session_sw_avc_reset (GrdDecodeSession  *decode_session,
                                 uint32_t           surface_width,
                                 uint32_t           surface_height,
                                 uint64_t           drm_format_modifier,
                                 GError           **error)
{
  GrdDecodeSessionSwAVC *decode_session_sw_avc =
    GRD_DECODE_SESSION_SW_AVC (decode_session);

  decode_session_sw_avc->surface_width = surface_width;
  decode_session_sw_avc->surface_height = surface_height;

  if (!h264_context_reset (decode_session_sw_avc->h264_context, surface_width, surface_height))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to reset H.264 decode context");
      return FALSE;
    }

  return TRUE;
}

static gboolean
allocate_mem_fd_buffer (GrdDecodeSessionSwAVC  *decode_session_sw_avc,
                        struct pw_buffer       *pw_buffer,
                        GError                **error)
{
  uint32_t surface_width = decode_session_sw_avc->surface_width;
  uint32_t surface_height = decode_session_sw_avc->surface_height;
  struct spa_data *spa_data = &pw_buffer->buffer->datas[0];
  g_autofd int fd = -1;
  unsigned int seals;
  int ret;

  fd = memfd_create ("grd-decode-session-mem-fd",
                     MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create mem-fd: %s", strerror (errno));
      return FALSE;
    }

  spa_data->chunk->stride = surface_width * 4;
  spa_data->maxsize = spa_data->chunk->stride * surface_height;
  spa_data->mapoffset = 0;

  do
    ret = ftruncate (fd, spa_data->maxsize);
  while (ret == -1 && errno == EINTR);
  if (ret < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to truncate mem-fd: %s", strerror (errno));
      return FALSE;
    }

  seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
  if (fcntl (fd, F_ADD_SEALS, seals) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to add seals to mem-fd: %s", strerror (errno));
      return FALSE;
    }

  spa_data->data = mmap (NULL, spa_data->maxsize,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         fd,
                         spa_data->mapoffset);
  if (spa_data->data == MAP_FAILED)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to mmap memory: %s", strerror (errno));
      return FALSE;
    }

  spa_data->type = SPA_DATA_MemFd;
  spa_data->flags = SPA_DATA_FLAG_READABLE | SPA_DATA_FLAG_MAPPABLE;
  spa_data->fd = g_steal_fd (&fd);

  return TRUE;
}

static gboolean
grd_decode_session_sw_avc_register_buffer (GrdDecodeSession  *decode_session,
                                           struct pw_buffer  *pw_buffer,
                                           GError           **error)
{
  GrdDecodeSessionSwAVC *decode_session_sw_avc =
    GRD_DECODE_SESSION_SW_AVC (decode_session);
  uint32_t surface_width = decode_session_sw_avc->surface_width;
  uint32_t surface_height = decode_session_sw_avc->surface_height;
  BitstreamBuffer *bitstream_buffer;
  size_t width_in_mbs;
  size_t height_in_mbs;
  size_t size;

  if (!allocate_mem_fd_buffer (decode_session_sw_avc, pw_buffer, error))
    return FALSE;

  width_in_mbs = grd_get_aligned_size (surface_width, 16) / 16;
  height_in_mbs = grd_get_aligned_size (surface_height, 16) / 16;
  size = MAX_BASE_SIZE + width_in_mbs * height_in_mbs * 400;

  bitstream_buffer = bitstream_buffer_new (size);

  g_hash_table_insert (decode_session_sw_avc->bitstream_buffers,
                       pw_buffer, bitstream_buffer);
  g_hash_table_insert (decode_session_sw_avc->pipewire_buffers,
                       bitstream_buffer->sample_buffer, pw_buffer);

  return TRUE;
}

static void
destroy_mem_fd_buffer (GrdDecodeSessionSwAVC *decode_session_sw_avc,
                       struct pw_buffer      *pw_buffer)
{
  struct spa_data *spa_data = &pw_buffer->buffer->datas[0];

  if (spa_data->type != SPA_DATA_MemFd)
    return;

  g_assert (spa_data->data != MAP_FAILED);
  g_assert (spa_data->fd > 0);

  munmap (spa_data->data, spa_data->maxsize);
  close (spa_data->fd);
}

static void
grd_decode_session_sw_avc_unregister_buffer (GrdDecodeSession *decode_session,
                                             struct pw_buffer *pw_buffer)
{
  GrdDecodeSessionSwAVC *decode_session_sw_avc =
    GRD_DECODE_SESSION_SW_AVC (decode_session);
  BitstreamBuffer *bitstream_buffer = NULL;
  GrdSampleBuffer *sample_buffer;

  if (!g_hash_table_lookup_extended (decode_session_sw_avc->bitstream_buffers,
                                     pw_buffer,
                                     NULL, (gpointer *) &bitstream_buffer))
    return;

  sample_buffer = bitstream_buffer->sample_buffer;

  g_hash_table_remove (decode_session_sw_avc->bitstream_buffers, pw_buffer);
  g_hash_table_remove (decode_session_sw_avc->pipewire_buffers, sample_buffer);

  destroy_mem_fd_buffer (decode_session_sw_avc, pw_buffer);
}

static GrdSampleBuffer *
grd_decode_session_sw_avc_get_sample_buffer (GrdDecodeSession *decode_session,
                                             struct pw_buffer *pw_buffer)
{
  GrdDecodeSessionSwAVC *decode_session_sw_avc =
    GRD_DECODE_SESSION_SW_AVC (decode_session);
  BitstreamBuffer *bitstream_buffer = NULL;

  if (!g_hash_table_lookup_extended (decode_session_sw_avc->bitstream_buffers,
                                     pw_buffer,
                                     NULL, (gpointer *) &bitstream_buffer))
    g_assert_not_reached ();

  return bitstream_buffer->sample_buffer;
}

static gboolean
grd_decode_session_sw_avc_submit_sample (GrdDecodeSession  *decode_session,
                                         GrdSampleBuffer   *sample_buffer,
                                         GError           **error)
{
  GrdDecodeSessionSwAVC *decode_session_sw_avc =
    GRD_DECODE_SESSION_SW_AVC (decode_session);

  g_mutex_lock (&decode_session_sw_avc->pending_frames_mutex);
  g_assert (!g_hash_table_contains (decode_session_sw_avc->pending_frames,
                                    sample_buffer));

  g_hash_table_add (decode_session_sw_avc->pending_frames, sample_buffer);
  g_mutex_unlock (&decode_session_sw_avc->pending_frames_mutex);

  return TRUE;
}

static uint32_t
grd_decode_session_sw_avc_get_n_pending_frames (GrdDecodeSession *decode_session)
{
  GrdDecodeSessionSwAVC *decode_session_sw_avc =
    GRD_DECODE_SESSION_SW_AVC (decode_session);
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&decode_session_sw_avc->pending_frames_mutex);
  return g_hash_table_size (decode_session_sw_avc->pending_frames);
}

static gboolean
grd_decode_session_sw_avc_decode_frame (GrdDecodeSession  *decode_session,
                                        GrdSampleBuffer   *sample_buffer,
                                        GError           **error)
{
  GrdDecodeSessionSwAVC *decode_session_sw_avc =
    GRD_DECODE_SESSION_SW_AVC (decode_session);
  uint32_t surface_width = decode_session_sw_avc->surface_width;
  uint32_t surface_height = decode_session_sw_avc->surface_height;
  struct pw_buffer *pw_buffer = NULL;
  struct spa_data *spa_data;
  RECTANGLE_16 rect = {};
  int32_t ret;

  g_mutex_lock (&decode_session_sw_avc->pending_frames_mutex);
  if (!g_hash_table_remove (decode_session_sw_avc->pending_frames,
                            sample_buffer))
    g_assert_not_reached ();
  g_mutex_unlock (&decode_session_sw_avc->pending_frames_mutex);

  if (!g_hash_table_lookup_extended (decode_session_sw_avc->pipewire_buffers,
                                     sample_buffer,
                                     NULL, (gpointer *) &pw_buffer))
    g_assert_not_reached ();

  spa_data = &pw_buffer->buffer->datas[0];

  rect.left = 0;
  rect.top = 0;
  rect.right = surface_width;
  rect.bottom = surface_height;

  ret = avc420_decompress (decode_session_sw_avc->h264_context,
                           grd_sample_buffer_get_data_pointer (sample_buffer),
                           grd_sample_buffer_get_sample_size (sample_buffer),
                           spa_data->data, PIXEL_FORMAT_BGRA32,
                           spa_data->chunk->stride, surface_width, surface_height,
                           &rect, 1);
  if (ret < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "avc420_decompress failed with status code %i", -ret);
      return FALSE;
    }

  return TRUE;
}

GrdDecodeSessionSwAVC *
grd_decode_session_sw_avc_new (GError **error)
{
  g_autoptr (GrdDecodeSessionSwAVC) decode_session_sw_avc = NULL;

  decode_session_sw_avc = g_object_new (GRD_TYPE_DECODE_SESSION_SW_AVC, NULL);

  decode_session_sw_avc->h264_context = h264_context_new (FALSE);
  if (!decode_session_sw_avc->h264_context)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create H.264 decode context");
      return NULL;
    }

  return g_steal_pointer (&decode_session_sw_avc);
}

static void
grd_decode_session_sw_avc_dispose (GObject *object)
{
  GrdDecodeSessionSwAVC *decode_session_sw_avc =
    GRD_DECODE_SESSION_SW_AVC (object);

  g_clear_pointer (&decode_session_sw_avc->h264_context, h264_context_free);

  g_clear_pointer (&decode_session_sw_avc->pending_frames, g_hash_table_unref);
  g_clear_pointer (&decode_session_sw_avc->pipewire_buffers, g_hash_table_unref);
  g_clear_pointer (&decode_session_sw_avc->bitstream_buffers, g_hash_table_unref);

  G_OBJECT_CLASS (grd_decode_session_sw_avc_parent_class)->dispose (object);
}

static void
grd_decode_session_sw_avc_finalize (GObject *object)
{
  GrdDecodeSessionSwAVC *decode_session_sw_avc =
    GRD_DECODE_SESSION_SW_AVC (object);

  g_mutex_clear (&decode_session_sw_avc->pending_frames_mutex);

  G_OBJECT_CLASS (grd_decode_session_sw_avc_parent_class)->finalize (object);
}

static void
grd_decode_session_sw_avc_init (GrdDecodeSessionSwAVC *decode_session_sw_avc)
{
  decode_session_sw_avc->bitstream_buffers =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) bitstream_buffer_free);
  decode_session_sw_avc->pipewire_buffers = g_hash_table_new (NULL, NULL);
  decode_session_sw_avc->pending_frames = g_hash_table_new (NULL, NULL);

  g_mutex_init (&decode_session_sw_avc->pending_frames_mutex);
}

static void
grd_decode_session_sw_avc_class_init (GrdDecodeSessionSwAVCClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdDecodeSessionClass *decode_session_class =
    GRD_DECODE_SESSION_CLASS (klass);

  object_class->dispose = grd_decode_session_sw_avc_dispose;
  object_class->finalize = grd_decode_session_sw_avc_finalize;

  decode_session_class->get_drm_format_modifiers =
    grd_decode_session_sw_avc_get_drm_format_modifiers;
  decode_session_class->reset =
    grd_decode_session_sw_avc_reset;
  decode_session_class->register_buffer =
    grd_decode_session_sw_avc_register_buffer;
  decode_session_class->unregister_buffer =
    grd_decode_session_sw_avc_unregister_buffer;
  decode_session_class->get_sample_buffer =
    grd_decode_session_sw_avc_get_sample_buffer;
  decode_session_class->submit_sample =
    grd_decode_session_sw_avc_submit_sample;
  decode_session_class->get_n_pending_frames =
    grd_decode_session_sw_avc_get_n_pending_frames;
  decode_session_class->decode_frame =
    grd_decode_session_sw_avc_decode_frame;
}
