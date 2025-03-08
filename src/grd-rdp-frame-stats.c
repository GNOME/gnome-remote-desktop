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

#include "grd-rdp-frame-stats.h"

struct _GrdRdpFrameStats
{
  /*
   * Amount of dual frames (frames containing both a main view and an
   * auxiliary view), that were sent to the client, but not acknowledged yet.
   * Each of those frames is either in the sending process, waiting on the
   * client side to be processed, currently being decoded, currently in the
   * process of being displayed, or their respective frame acknowledge message
   * is currently in the process of being sent to the server side.
   */
  uint32_t missing_dual_frame_acks;

  /*
   * Current encoding rate per second showing how many frames are currently on
   * average produced by the server side every second.
   */
  uint32_t enc_rate;
  /*
   * Current acknowledge rate per second showing how many frames are currently
   * on average acknowledged by the client every second. Every acknowledged
   * frame was sent to the client, decoded and displayed by the client, and
   * its frame acknowledge message was sent to the server side.
   */
  uint32_t ack_rate;
};

uint32_t
grd_rdp_frame_stats_get_missing_dual_frame_acks (GrdRdpFrameStats *frame_stats)
{
  return frame_stats->missing_dual_frame_acks;
}

uint32_t
grd_rdp_frame_stats_get_enc_rate (GrdRdpFrameStats *frame_stats)
{
  return frame_stats->enc_rate;
}

uint32_t
grd_rdp_frame_stats_get_ack_rate (GrdRdpFrameStats *frame_stats)
{
  return frame_stats->ack_rate;
}

GrdRdpFrameStats *
grd_rdp_frame_stats_new (uint32_t missing_dual_frame_acks,
                         uint32_t enc_rate,
                         uint32_t ack_rate)
{
  GrdRdpFrameStats *frame_stats;

  frame_stats = g_new0 (GrdRdpFrameStats, 1);
  frame_stats->missing_dual_frame_acks = missing_dual_frame_acks;
  frame_stats->enc_rate = enc_rate;
  frame_stats->ack_rate = ack_rate;

  return frame_stats;
}

void
grd_rdp_frame_stats_free (GrdRdpFrameStats *frame_stats)
{
  g_free (frame_stats);
}
