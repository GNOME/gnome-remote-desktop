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

#ifndef GRD_RDP_FRAME_STATS_H
#define GRD_RDP_FRAME_STATS_H

#include <glib.h>
#include <stdint.h>

#include "grd-types.h"

GrdRdpFrameStats *grd_rdp_frame_stats_new (uint32_t missing_dual_frame_acks,
                                           uint32_t enc_rate,
                                           uint32_t ack_rate);

void grd_rdp_frame_stats_free (GrdRdpFrameStats *frame_stats);

uint32_t grd_rdp_frame_stats_get_missing_dual_frame_acks (GrdRdpFrameStats *frame_stats);

uint32_t grd_rdp_frame_stats_get_enc_rate (GrdRdpFrameStats *frame_stats);

uint32_t grd_rdp_frame_stats_get_ack_rate (GrdRdpFrameStats *frame_stats);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdRdpFrameStats, grd_rdp_frame_stats_free)

#endif /* GRD_RDP_FRAME_STATS_H */
