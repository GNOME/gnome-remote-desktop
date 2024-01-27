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

#include "config.h"

#include "grd-rdp-render-context.h"

#include "grd-rdp-damage-detector.h"
#include "grd-rdp-surface.h"

struct _GrdRdpRenderContext
{
  GObject parent;
};

G_DEFINE_TYPE (GrdRdpRenderContext, grd_rdp_render_context, G_TYPE_OBJECT)

GrdRdpRenderContext *
grd_rdp_render_context_new (GrdRdpSurface *rdp_surface)
{
  if (!grd_rdp_damage_detector_invalidate_surface (rdp_surface->detector))
    return NULL;

  return g_object_new (GRD_TYPE_RDP_RENDER_CONTEXT, NULL);
}

static void
grd_rdp_render_context_init (GrdRdpRenderContext *render_context)
{
}

static void
grd_rdp_render_context_class_init (GrdRdpRenderContextClass *klass)
{
}
