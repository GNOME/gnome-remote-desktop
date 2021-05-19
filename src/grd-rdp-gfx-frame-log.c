/*
 * Copyright (C) 2021 Pascal Nowack
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

#include "grd-rdp-gfx-frame-log.h"

#include "grd-rdp-frame-info.h"

struct _GrdRdpGfxFrameLog
{
  GObject parent;

  GQueue *encoded_frames;
  GQueue *acked_frames;

  GHashTable *tracked_frames;
};

G_DEFINE_TYPE (GrdRdpGfxFrameLog, grd_rdp_gfx_frame_log, G_TYPE_OBJECT);

static void
track_enc_frame_info (GrdRdpGfxFrameLog *frame_log,
                      uint32_t           frame_id,
                      int64_t            enc_time_us)
{
  GrdRdpFrameInfo *frame_info;

  frame_info = g_malloc0 (sizeof (GrdRdpFrameInfo));
  frame_info->frame_id = frame_id;
  frame_info->enc_time_us = enc_time_us;

  g_queue_push_tail (frame_log->encoded_frames, frame_info);
}

static void
track_ack_frame_info (GrdRdpGfxFrameLog *frame_log,
                      uint32_t           frame_id,
                      int64_t            ack_time_us)
{
  GrdRdpFrameInfo *frame_info;

  frame_info = g_malloc0 (sizeof (GrdRdpFrameInfo));
  frame_info->frame_id = frame_id;
  frame_info->ack_time_us = ack_time_us;

  g_queue_push_tail (frame_log->acked_frames, frame_info);
}

void
grd_rdp_gfx_frame_log_track_frame (GrdRdpGfxFrameLog *frame_log,
                                   uint32_t           frame_id,
                                   int64_t            enc_time_us)
{
  track_enc_frame_info (frame_log, frame_id, enc_time_us);
  g_hash_table_add (frame_log->tracked_frames, GUINT_TO_POINTER (frame_id));
}

void
grd_rdp_gfx_frame_log_ack_tracked_frame (GrdRdpGfxFrameLog *frame_log,
                                         uint32_t           frame_id,
                                         int64_t            ack_time_us)
{
  if (!g_hash_table_remove (frame_log->tracked_frames, GUINT_TO_POINTER (frame_id)))
    return;

  track_ack_frame_info (frame_log, frame_id, ack_time_us);
}

void
grd_rdp_gfx_frame_log_unack_last_acked_frame (GrdRdpGfxFrameLog *frame_log,
                                              uint32_t           frame_id,
                                              int64_t            enc_ack_time_us)
{
  GrdRdpFrameInfo *frame_info;

  if ((frame_info = g_queue_pop_tail (frame_log->acked_frames)))
    {
      g_assert (frame_info->frame_id == frame_id);
      g_assert (frame_info->ack_time_us == enc_ack_time_us);
    }
  g_free (frame_info);

  g_hash_table_add (frame_log->tracked_frames, GUINT_TO_POINTER (frame_id));
}

static void
clear_old_enc_frame_infos (GrdRdpGfxFrameLog *frame_log,
                           int64_t            current_time_us)
{
  GrdRdpFrameInfo *frame_info;

  while ((frame_info = g_queue_peek_head (frame_log->encoded_frames)) &&
         current_time_us - frame_info->enc_time_us >= 1 * G_USEC_PER_SEC)
    g_free (g_queue_pop_head (frame_log->encoded_frames));
}

static void
clear_old_ack_frame_infos (GrdRdpGfxFrameLog *frame_log,
                           int64_t            current_time_us)
{
  GrdRdpFrameInfo *frame_info;

  while ((frame_info = g_queue_peek_head (frame_log->acked_frames)) &&
         current_time_us - frame_info->ack_time_us >= 1 * G_USEC_PER_SEC)
    g_free (g_queue_pop_head (frame_log->acked_frames));
}

void
grd_rdp_gfx_frame_log_update_rates (GrdRdpGfxFrameLog *frame_log,
                                    uint32_t          *enc_rate,
                                    uint32_t          *ack_rate)
{
  int64_t current_time_us;

  current_time_us = g_get_monotonic_time ();
  clear_old_enc_frame_infos (frame_log, current_time_us);
  clear_old_ack_frame_infos (frame_log, current_time_us);

  /*
   * Every remaining frame time, tracked in encoded_frames or acked_frames, is
   * now younger than 1 second.
   * The list lengths therefore represent the encoded_frames/s and
   * acked_frames/s values.
   */
  *enc_rate = g_queue_get_length (frame_log->encoded_frames);
  *ack_rate = g_queue_get_length (frame_log->acked_frames);
}

uint32_t
grd_rdp_gfx_frame_log_get_unacked_frames_count (GrdRdpGfxFrameLog *frame_log)
{
  return g_hash_table_size (frame_log->tracked_frames);
}

void
grd_rdp_gfx_frame_log_clear (GrdRdpGfxFrameLog *frame_log)
{
  g_hash_table_remove_all (frame_log->tracked_frames);
}

GrdRdpGfxFrameLog *
grd_rdp_gfx_frame_log_new (void)
{
  GrdRdpGfxFrameLog *frame_log;

  frame_log = g_object_new (GRD_TYPE_RDP_GFX_FRAME_LOG, NULL);

  return frame_log;
}

static void
grd_rdp_gfx_frame_log_dispose (GObject *object)
{
  GrdRdpGfxFrameLog *frame_log = GRD_RDP_GFX_FRAME_LOG (object);

  if (frame_log->acked_frames)
    {
      g_queue_free_full (frame_log->acked_frames, g_free);
      frame_log->acked_frames = NULL;
    }

  if (frame_log->encoded_frames)
    {
      g_queue_free_full (frame_log->encoded_frames, g_free);
      frame_log->encoded_frames = NULL;
    }

  g_clear_pointer (&frame_log->tracked_frames, g_hash_table_destroy);

  G_OBJECT_CLASS (grd_rdp_gfx_frame_log_parent_class)->dispose (object);
}

static void
grd_rdp_gfx_frame_log_init (GrdRdpGfxFrameLog *frame_log)
{
  frame_log->tracked_frames = g_hash_table_new (NULL, NULL);
  frame_log->encoded_frames = g_queue_new ();
  frame_log->acked_frames = g_queue_new ();
}

static void
grd_rdp_gfx_frame_log_class_init (GrdRdpGfxFrameLogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_gfx_frame_log_dispose;
}
