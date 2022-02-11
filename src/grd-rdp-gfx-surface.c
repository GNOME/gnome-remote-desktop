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

#include "grd-rdp-gfx-surface.h"

#include "grd-rdp-graphics-pipeline.h"
#include "grd-rdp-surface.h"

struct _GrdRdpGfxSurface
{
  GObject parent;

  GrdRdpGraphicsPipeline *graphics_pipeline;
  GrdRdpSurface *rdp_surface;
  gboolean created;

  uint16_t surface_id;
  uint32_t codec_context_id;
  uint32_t serial;

  GrdRdpGfxFrameController *frame_controller;
};

G_DEFINE_TYPE (GrdRdpGfxSurface, grd_rdp_gfx_surface, G_TYPE_OBJECT)

uint16_t
grd_rdp_gfx_surface_get_surface_id (GrdRdpGfxSurface *gfx_surface)
{
  return gfx_surface->surface_id;
}

uint32_t
grd_rdp_gfx_surface_get_codec_context_id (GrdRdpGfxSurface *gfx_surface)
{
  return gfx_surface->codec_context_id;
}

uint32_t
grd_rdp_gfx_surface_get_serial (GrdRdpGfxSurface *gfx_surface)
{
  return gfx_surface->serial;
}

GrdRdpSurface *
grd_rdp_gfx_surface_get_rdp_surface (GrdRdpGfxSurface *gfx_surface)
{
  return gfx_surface->rdp_surface;
}

GrdRdpGfxFrameController *
grd_rdp_gfx_surface_get_frame_controller (GrdRdpGfxSurface *gfx_surface)
{
  return gfx_surface->frame_controller;
}

void
grd_rdp_gfx_surface_attach_frame_controller (GrdRdpGfxSurface         *gfx_surface,
                                             GrdRdpGfxFrameController *frame_controller)
{
  g_assert (!gfx_surface->frame_controller);

  gfx_surface->frame_controller = frame_controller;
}

GrdRdpGfxSurface *
grd_rdp_gfx_surface_new (GrdRdpGraphicsPipeline *graphics_pipeline,
                         GrdRdpSurface          *rdp_surface,
                         uint16_t                surface_id,
                         uint32_t                serial)
{
  GrdRdpGfxSurface *gfx_surface;

  gfx_surface = g_object_new (GRD_TYPE_RDP_GFX_SURFACE, NULL);
  gfx_surface->graphics_pipeline = graphics_pipeline;
  gfx_surface->rdp_surface = rdp_surface;
  gfx_surface->surface_id = surface_id;
  gfx_surface->serial = serial;
  /*
   * Use the same id for the codec context as for the surface
   * (only relevant for RDPGFX_WIRE_TO_SURFACE_PDU_2 PDUs)
   */
  gfx_surface->codec_context_id = surface_id;

  grd_rdp_graphics_pipeline_create_surface (graphics_pipeline, gfx_surface);
  gfx_surface->created = TRUE;

  return gfx_surface;
}

static void
grd_rdp_gfx_surface_dispose (GObject *object)
{
  GrdRdpGfxSurface *gfx_surface = GRD_RDP_GFX_SURFACE (object);

  g_clear_object (&gfx_surface->frame_controller);

  if (gfx_surface->created)
    {
      grd_rdp_graphics_pipeline_delete_surface (gfx_surface->graphics_pipeline,
                                                gfx_surface);
      gfx_surface->created = FALSE;
    }

  G_OBJECT_CLASS (grd_rdp_gfx_surface_parent_class)->dispose (object);
}

static void
grd_rdp_gfx_surface_init (GrdRdpGfxSurface *gfx_surface)
{
}

static void
grd_rdp_gfx_surface_class_init (GrdRdpGfxSurfaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_gfx_surface_dispose;
}
