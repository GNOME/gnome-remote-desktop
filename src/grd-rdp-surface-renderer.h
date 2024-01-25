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

#ifndef GRD_RDP_SURFACE_RENDERER_H
#define GRD_RDP_SURFACE_RENDERER_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_SURFACE_RENDERER (grd_rdp_surface_renderer_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpSurfaceRenderer, grd_rdp_surface_renderer,
                      GRD, RDP_SURFACE_RENDERER, GObject)

GrdRdpSurfaceRenderer *grd_rdp_surface_renderer_new (GrdRdpSurface  *rdp_surface,
                                                     GrdRdpRenderer *renderer,
                                                     GrdSessionRdp  *session_rdp,
                                                     uint32_t        refresh_rate);

uint32_t grd_rdp_surface_renderer_get_refresh_rate (GrdRdpSurfaceRenderer *surface_renderer);

gboolean grd_rdp_surface_renderer_is_rendering_suspended (GrdRdpSurfaceRenderer *surface_renderer);

void grd_rdp_surface_renderer_update_suspension_state (GrdRdpSurfaceRenderer *surface_renderer,
                                                       gboolean               suspend_rendering);

void grd_rdp_surface_renderer_submit_buffer (GrdRdpSurfaceRenderer *surface_renderer,
                                             GrdRdpBuffer          *buffer);

void grd_rdp_surface_renderer_trigger_render_source (GrdRdpSurfaceRenderer *surface_renderer);

void grd_rdp_surface_renderer_reset (GrdRdpSurfaceRenderer *surface_renderer);

#endif /* GRD_RDP_SURFACE_RENDERER_H */
