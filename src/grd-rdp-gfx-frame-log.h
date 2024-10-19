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

#ifndef GRD_RDP_GFX_FRAME_LOG_H
#define GRD_RDP_GFX_FRAME_LOG_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_GFX_FRAME_LOG (grd_rdp_gfx_frame_log_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpGfxFrameLog, grd_rdp_gfx_frame_log,
                      GRD, RDP_GFX_FRAME_LOG, GObject)

GrdRdpGfxFrameLog *grd_rdp_gfx_frame_log_new (void);

void grd_rdp_gfx_frame_log_track_frame (GrdRdpGfxFrameLog *frame_log,
                                        uint32_t           frame_id,
                                        uint32_t           n_subframes,
                                        int64_t            enc_time_us);

void grd_rdp_gfx_frame_log_ack_tracked_frame (GrdRdpGfxFrameLog *frame_log,
                                              uint32_t           frame_id,
                                              int64_t            ack_time_us);

void grd_rdp_gfx_frame_log_unack_last_acked_frame (GrdRdpGfxFrameLog *frame_log,
                                                   uint32_t           frame_id,
                                                   uint32_t           n_subframes,
                                                   int64_t            enc_ack_time_us);

void grd_rdp_gfx_frame_log_update_rates (GrdRdpGfxFrameLog *frame_log,
                                         uint32_t          *enc_rate,
                                         uint32_t          *ack_rate);

uint32_t grd_rdp_gfx_frame_log_get_unacked_frames_count (GrdRdpGfxFrameLog *frame_log);

uint32_t grd_rdp_gfx_frame_log_get_unacked_stereo_frames_count (GrdRdpGfxFrameLog *frame_log);

void grd_rdp_gfx_frame_log_clear (GrdRdpGfxFrameLog *frame_log);

#endif /* GRD_RDP_GFX_FRAME_LOG_H */
