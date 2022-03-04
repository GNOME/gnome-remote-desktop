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
                      GRD, RDP_GFX_SURFACE, GObject)

typedef enum _GrdRdpGfxSurfaceFlag
{
  GRD_RDP_GFX_SURFACE_FLAG_NONE = 0,
  GRD_RDP_GFX_SURFACE_FLAG_ALIGNED_SIZE = 1 << 0,
  GRD_RDP_GFX_SURFACE_FLAG_NO_HWACCEL_SESSIONS = 1 << 1,
} GrdRdpGfxSurfaceFlag;

typedef struct _GrdRdpGfxSurfaceDescriptor
{
  /* Mandatory */
  GrdRdpGfxSurfaceFlag flags;
  uint16_t surface_id;
  uint32_t serial;

  GrdRdpSurface *rdp_surface;

  /* GRD_RDP_GFX_SURFACE_FLAG_ALIGNED_SIZE */
  uint16_t aligned_width;
  uint16_t aligned_height;
} GrdRdpGfxSurfaceDescriptor;

GrdRdpGfxSurface *grd_rdp_gfx_surface_new (GrdRdpGraphicsPipeline           *graphics_pipeline,
                                           const GrdRdpGfxSurfaceDescriptor *surface_descriptor);

uint16_t grd_rdp_gfx_surface_get_surface_id (GrdRdpGfxSurface *gfx_surface);

uint32_t grd_rdp_gfx_surface_get_codec_context_id (GrdRdpGfxSurface *gfx_surface);

uint32_t grd_rdp_gfx_surface_get_serial (GrdRdpGfxSurface *gfx_surface);

GrdRdpSurface *grd_rdp_gfx_surface_get_rdp_surface (GrdRdpGfxSurface *gfx_surface);

uint16_t grd_rdp_gfx_surface_get_width (GrdRdpGfxSurface *gfx_surface);

uint16_t grd_rdp_gfx_surface_get_height (GrdRdpGfxSurface *gfx_surface);

gboolean grd_rdp_gfx_surface_disallows_hwaccel_sessions (GrdRdpGfxSurface *gfx_surface);

GrdRdpGfxSurface *grd_rdp_gfx_surface_get_render_surface (GrdRdpGfxSurface *gfx_surface);

void grd_rdp_gfx_surface_override_render_surface (GrdRdpGfxSurface *gfx_surface,
                                                  GrdRdpGfxSurface *render_surface);

GrdRdpGfxFrameController *grd_rdp_gfx_surface_get_frame_controller (GrdRdpGfxSurface *gfx_surface);

void grd_rdp_gfx_surface_attach_frame_controller (GrdRdpGfxSurface         *gfx_surface,
                                                  GrdRdpGfxFrameController *frame_controller);

#endif /* GRD_RDP_GFX_SURFACE_H */
