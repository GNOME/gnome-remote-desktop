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

#ifndef GRD_RDP_RENDERER_H
#define GRD_RDP_RENDERER_H

#include <freerdp/freerdp.h>
#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_RENDERER (grd_rdp_renderer_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpRenderer, grd_rdp_renderer,
                      GRD, RDP_RENDERER, GObject)

GrdRdpRenderer *grd_rdp_renderer_new (GrdSessionRdp    *session_rdp,
                                      GrdHwAccelNvidia *hwaccel_nvidia);

GMainContext *grd_rdp_renderer_get_graphics_context (GrdRdpRenderer *renderer);

void grd_rdp_renderer_update_output_suppression_state (GrdRdpRenderer *renderer,
                                                       gboolean        suppress_output);

void grd_rdp_renderer_invoke_shutdown (GrdRdpRenderer *renderer);

void grd_rdp_renderer_notify_session_started (GrdRdpRenderer         *renderer,
                                              GrdRdpGraphicsPipeline *graphics_pipeline,
                                              rdpContext             *rdp_context);

void grd_rdp_renderer_notify_new_desktop_layout (GrdRdpRenderer *renderer,
                                                 uint32_t        desktop_width,
                                                 uint32_t        desktop_height);

void grd_rdp_renderer_notify_graphics_pipeline_ready (GrdRdpRenderer *renderer);

void grd_rdp_renderer_notify_graphics_pipeline_reset (GrdRdpRenderer *renderer);

void grd_rdp_renderer_inhibit_rendering (GrdRdpRenderer *renderer);

void grd_rdp_renderer_uninhibit_rendering (GrdRdpRenderer *renderer);

GrdRdpSurface *grd_rdp_renderer_try_acquire_surface (GrdRdpRenderer *renderer,
                                                     uint32_t        refresh_rate);

void grd_rdp_renderer_release_surface (GrdRdpRenderer *renderer,
                                       GrdRdpSurface  *rdp_surface);

GrdRdpRenderContext *grd_rdp_renderer_try_acquire_render_context (GrdRdpRenderer *renderer,
                                                                  GrdRdpSurface  *rdp_surface);

void grd_rdp_renderer_release_render_context (GrdRdpRenderer      *renderer,
                                              GrdRdpRenderContext *render_context);

void grd_rdp_renderer_clear_render_contexts (GrdRdpRenderer *renderer);

void grd_rdp_renderer_clear_gfx_surfaces (GrdRdpRenderer *renderer);

#endif /* GRD_RDP_RENDERER_H */
