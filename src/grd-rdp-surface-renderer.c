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

#include "grd-rdp-surface-renderer.h"

#include "grd-session-rdp.h"

struct _GrdRdpSurfaceRenderer
{
  GObject parent;

  GrdRdpSurface *rdp_surface;
  GrdSessionRdp *session_rdp;

  uint32_t refresh_rate;

  GSource *render_source;
  gboolean rendering_suspended;
};

G_DEFINE_TYPE (GrdRdpSurfaceRenderer, grd_rdp_surface_renderer, G_TYPE_OBJECT)

uint32_t
grd_rdp_surface_renderer_get_refresh_rate (GrdRdpSurfaceRenderer *surface_renderer)
{
  return surface_renderer->refresh_rate;
}

gboolean
grd_rdp_surface_renderer_is_rendering_suspended (GrdRdpSurfaceRenderer *surface_renderer)
{
  return surface_renderer->rendering_suspended;
}

void
grd_rdp_surface_renderer_update_suspension_state (GrdRdpSurfaceRenderer *surface_renderer,
                                                  gboolean               suspend_rendering)
{
  gboolean rendering_was_suspended = surface_renderer->rendering_suspended;

  surface_renderer->rendering_suspended = suspend_rendering;
  if (rendering_was_suspended && !surface_renderer->rendering_suspended)
    grd_rdp_surface_renderer_trigger_render_source (surface_renderer);
}

void
grd_rdp_surface_renderer_trigger_render_source (GrdRdpSurfaceRenderer *surface_renderer)
{
  g_source_set_ready_time (surface_renderer->render_source, 0);
}

static gboolean
maybe_render_frame (gpointer user_data)
{
  GrdRdpSurfaceRenderer *surface_renderer = user_data;

  grd_session_rdp_maybe_encode_pending_frame (surface_renderer->session_rdp,
                                              surface_renderer->rdp_surface);

  return G_SOURCE_CONTINUE;
}

static gboolean
render_source_dispatch (GSource     *source,
                        GSourceFunc  callback,
                        gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs render_source_funcs =
{
  .dispatch = render_source_dispatch,
};

GrdRdpSurfaceRenderer *
grd_rdp_surface_renderer_new (GrdRdpSurface *rdp_surface,
                              GMainContext  *graphics_context,
                              GrdSessionRdp *session_rdp,
                              uint32_t       refresh_rate)
{
  GrdRdpSurfaceRenderer *surface_renderer;
  GSource *render_source;

  surface_renderer = g_object_new (GRD_TYPE_RDP_SURFACE_RENDERER, NULL);
  surface_renderer->rdp_surface = rdp_surface;
  surface_renderer->session_rdp = session_rdp;
  surface_renderer->refresh_rate = refresh_rate;

  render_source = g_source_new (&render_source_funcs, sizeof (GSource));
  g_source_set_callback (render_source, maybe_render_frame,
                         surface_renderer, NULL);
  g_source_set_ready_time (render_source, -1);
  g_source_attach (render_source, graphics_context);
  surface_renderer->render_source = render_source;

  return surface_renderer;
}

static void
grd_rdp_surface_renderer_dispose (GObject *object)
{
  GrdRdpSurfaceRenderer *surface_renderer = GRD_RDP_SURFACE_RENDERER (object);

  if (surface_renderer->render_source)
    {
      g_source_destroy (surface_renderer->render_source);
      g_clear_pointer (&surface_renderer->render_source, g_source_unref);
    }

  G_OBJECT_CLASS (grd_rdp_surface_renderer_parent_class)->dispose (object);
}

static void
grd_rdp_surface_renderer_init (GrdRdpSurfaceRenderer *surface_renderer)
{
}

static void
grd_rdp_surface_renderer_class_init (GrdRdpSurfaceRendererClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_surface_renderer_dispose;
}
