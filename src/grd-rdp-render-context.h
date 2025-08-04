/*
 * Copyright (C) 2023 Pascal Nowack
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

#pragma once

#include <cairo/cairo.h>
#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_RENDER_CONTEXT (grd_rdp_render_context_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpRenderContext, grd_rdp_render_context,
                      GRD, RDP_RENDER_CONTEXT, GObject)

typedef enum
{
  GRD_RDP_CODEC_CAPROGRESSIVE,
  GRD_RDP_CODEC_AVC420,
  GRD_RDP_CODEC_AVC444v2,
} GrdRdpCodec;

GrdRdpRenderContext *grd_rdp_render_context_new (GrdRdpDvcGraphicsPipeline *graphics_pipeline,
                                                 GrdRdpSurface             *rdp_surface,
                                                 GrdEglThread              *egl_thread,
                                                 GrdVkDevice               *vk_device,
                                                 GrdHwAccelVaapi           *hwaccel_vaapi,
                                                 GrdRdpSwEncoderCa         *encoder_ca);

GrdRdpCodec grd_rdp_render_context_get_codec (GrdRdpRenderContext *render_context);

GrdRdpGfxSurface *grd_rdp_render_context_get_gfx_surface (GrdRdpRenderContext *render_context);

GrdRdpViewCreator *grd_rdp_render_context_get_view_creator (GrdRdpRenderContext *render_context);

GrdEncodeSession *grd_rdp_render_context_get_encode_session (GrdRdpRenderContext *render_context);

gboolean grd_rdp_render_context_must_delay_view_finalization (GrdRdpRenderContext *render_context);

gboolean grd_rdp_render_context_should_avoid_dual_frame (GrdRdpRenderContext *render_context);

GrdImageView *grd_rdp_render_context_acquire_image_view (GrdRdpRenderContext *render_context);

void grd_rdp_render_context_release_image_view (GrdRdpRenderContext *render_context,
                                                GrdImageView        *image_view);

void grd_rdp_render_context_update_frame_state (GrdRdpRenderContext *render_context,
                                                GrdRdpFrame         *rdp_frame,
                                                GrdRdpRenderState   *render_state);

void grd_rdp_render_context_fetch_progressive_render_state (GrdRdpRenderContext  *render_context,
                                                            GrdImageView        **image_view,
                                                            cairo_region_t      **damage_region);
