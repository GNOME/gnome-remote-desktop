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

#ifndef GRD_RDP_GFX_SURFACE_H
#define GRD_RDP_GFX_SURFACE_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_GFX_SURFACE (grd_rdp_gfx_surface_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpGfxSurface, grd_rdp_gfx_surface,
                      GRD, RDP_GFX_SURFACE, GObject);

GrdRdpGfxSurface *grd_rdp_gfx_surface_new (GrdRdpGraphicsPipeline *graphics_pipeline,
                                           GrdSessionRdp          *session_rdp,
                                           GMainContext           *pipeline_context,
                                           GrdRdpSurface          *rdp_surface,
                                           uint16_t                surface_id,
                                           uint32_t                serial);

uint16_t grd_rdp_gfx_surface_get_surface_id (GrdRdpGfxSurface *gfx_surface);

uint32_t grd_rdp_gfx_surface_get_codec_context_id (GrdRdpGfxSurface *gfx_surface);

uint32_t grd_rdp_gfx_surface_get_serial (GrdRdpGfxSurface *gfx_surface);

GrdRdpSurface *grd_rdp_gfx_surface_get_rdp_surface (GrdRdpGfxSurface *gfx_surface);

void grd_rdp_gfx_surface_unack_frame (GrdRdpGfxSurface *gfx_surface,
                                      uint32_t          frame_id,
                                      int64_t           enc_time_us);

void grd_rdp_gfx_surface_ack_frame (GrdRdpGfxSurface *gfx_surface,
                                    uint32_t          frame_id,
                                    int64_t           ack_time_us);

void grd_rdp_gfx_surface_unack_last_acked_frame (GrdRdpGfxSurface *gfx_surface,
                                                 uint32_t          frame_id,
                                                 int64_t           enc_ack_time_us);

void grd_rdp_gfx_surface_clear_all_unacked_frames (GrdRdpGfxSurface *gfx_surface);

void grd_rdp_gfx_surface_notify_new_round_trip_time (GrdRdpGfxSurface *gfx_surface,
                                                     int64_t           round_trip_time_us);

#endif /* GRD_RDP_GFX_SURFACE_H */
