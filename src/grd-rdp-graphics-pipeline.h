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

#ifndef GRD_RDP_GRAPHICS_PIPELINE_H
#define GRD_RDP_GRAPHICS_PIPELINE_H

#include <freerdp/freerdp.h>
#include <freerdp/server/rdpgfx.h>
#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_GRAPHICS_PIPELINE (grd_rdp_graphics_pipeline_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpGraphicsPipeline, grd_rdp_graphics_pipeline,
                      GRD, RDP_GRAPHICS_PIPELINE, GObject)

GrdRdpGraphicsPipeline *grd_rdp_graphics_pipeline_new (GrdSessionRdp              *session_rdp,
                                                       GrdRdpRenderer             *renderer,
                                                       GrdRdpDvcHandler           *dvc_handler,
                                                       HANDLE                      vcm,
                                                       rdpContext                 *rdp_context,
                                                       GrdRdpNetworkAutodetection *network_autodetection,
                                                       wStream                    *encode_stream,
                                                       RFX_CONTEXT                *rfx_context);

void grd_rdp_graphics_pipeline_maybe_init (GrdRdpGraphicsPipeline *graphics_pipeline);

void grd_rdp_graphics_pipeline_get_capabilities (GrdRdpGraphicsPipeline *graphics_pipeline,
                                                 gboolean               *have_avc444,
                                                 gboolean               *have_avc420);

void grd_rdp_graphics_pipeline_set_hwaccel_nvidia (GrdRdpGraphicsPipeline *graphics_pipeline,
                                                   GrdHwAccelNvidia       *hwaccel_nvidia);

void grd_rdp_graphics_pipeline_create_surface (GrdRdpGraphicsPipeline *graphics_pipeline,
                                               GrdRdpGfxSurface       *gfx_surface);

void grd_rdp_graphics_pipeline_delete_surface (GrdRdpGraphicsPipeline *graphics_pipeline,
                                               GrdRdpGfxSurface       *gfx_surface);

GrdRdpGfxSurface *grd_rdp_graphics_pipeline_acquire_gfx_surface (GrdRdpGraphicsPipeline *graphics_pipeline,
                                                                 GrdRdpSurface          *rdp_surface);

void grd_rdp_graphics_pipeline_reset_graphics (GrdRdpGraphicsPipeline *graphics_pipeline,
                                               uint32_t                width,
                                               uint32_t                height,
                                               MONITOR_DEF            *monitors,
                                               uint32_t                n_monitors);

void grd_rdp_graphics_pipeline_notify_new_round_trip_time (GrdRdpGraphicsPipeline *graphics_pipeline,
                                                           uint64_t                round_trip_time_us);

void grd_rdp_graphics_pipeline_submit_frame (GrdRdpGraphicsPipeline *graphics_pipeline,
                                             GrdRdpFrame            *rdp_frame);

gboolean grd_rdp_graphics_pipeline_refresh_gfx (GrdRdpGraphicsPipeline *graphics_pipeline,
                                                GrdRdpSurface          *rdp_surface,
                                                GrdRdpRenderContext    *render_context,
                                                GrdRdpLegacyBuffer     *buffer);

#endif /* GRD_RDP_GRAPHICS_PIPELINE_H */
