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

#include "grd-rdp-surface.h"

#include "grd-hwaccel-nvidia.h"
#include "grd-rdp-damage-detector-cuda.h"
#include "grd-rdp-damage-detector-memcmp.h"
#include "grd-rdp-surface-renderer.h"

static void
destroy_hwaccel_util_objects (GrdRdpSurface *rdp_surface)
{
  if (rdp_surface->cuda_stream)
    {
      grd_hwaccel_nvidia_destroy_cuda_stream (rdp_surface->hwaccel_nvidia,
                                              rdp_surface->cuda_stream);
      rdp_surface->cuda_stream = NULL;
    }
  if (rdp_surface->avc.main_view)
    {
      grd_hwaccel_nvidia_clear_mem_ptr (rdp_surface->hwaccel_nvidia,
                                        &rdp_surface->avc.main_view);
    }
}

GrdRdpSurface *
grd_rdp_surface_new (GrdHwAccelNvidia *hwaccel_nvidia)
{
  g_autofree GrdRdpSurface *rdp_surface = NULL;

  rdp_surface = g_malloc0 (sizeof (GrdRdpSurface));
  rdp_surface->hwaccel_nvidia = hwaccel_nvidia;

  if (hwaccel_nvidia &&
      !grd_hwaccel_nvidia_create_cuda_stream (hwaccel_nvidia,
                                              &rdp_surface->cuda_stream))
    return NULL;

  if (hwaccel_nvidia)
    {
      GrdRdpDamageDetectorCuda *detector;

      detector = grd_rdp_damage_detector_cuda_new (hwaccel_nvidia,
                                                   rdp_surface->cuda_stream);
      if (!detector)
        {
          destroy_hwaccel_util_objects (rdp_surface);
          return NULL;
        }

      rdp_surface->detector = GRD_RDP_DAMAGE_DETECTOR (detector);
    }
  else
    {
      GrdRdpDamageDetectorMemcmp *detector;

      detector = grd_rdp_damage_detector_memcmp_new ();
      rdp_surface->detector = GRD_RDP_DAMAGE_DETECTOR (detector);
    }

  return g_steal_pointer (&rdp_surface);
}

void
grd_rdp_surface_free (GrdRdpSurface *rdp_surface)
{
  g_assert (!rdp_surface->gfx_surface);

  g_assert (!rdp_surface->pending_framebuffer);

  g_clear_object (&rdp_surface->surface_renderer);

  g_clear_pointer (&rdp_surface->surface_mapping, g_free);

  g_clear_object (&rdp_surface->detector);
  destroy_hwaccel_util_objects (rdp_surface);

  g_free (rdp_surface);
}

uint32_t
grd_rdp_surface_get_width (GrdRdpSurface *rdp_surface)
{
  return rdp_surface->width;
}

uint32_t
grd_rdp_surface_get_height (GrdRdpSurface *rdp_surface)
{
  return rdp_surface->height;
}

GrdRdpSurfaceMapping *
grd_rdp_surface_get_mapping (GrdRdpSurface *rdp_surface)
{
  return rdp_surface->surface_mapping;
}

GrdRdpSurfaceRenderer *
grd_rdp_surface_get_surface_renderer (GrdRdpSurface *rdp_surface)
{
  return rdp_surface->surface_renderer;
}

void
grd_rdp_surface_set_size (GrdRdpSurface *rdp_surface,
                          uint32_t       width,
                          uint32_t       height)
{
  rdp_surface->width = width;
  rdp_surface->height = height;
}

void
grd_rdp_surface_set_mapping (GrdRdpSurface        *rdp_surface,
                             GrdRdpSurfaceMapping *surface_mapping)
{
  g_clear_pointer (&rdp_surface->surface_mapping, g_free);
  rdp_surface->surface_mapping = surface_mapping;
}

void
grd_rdp_surface_attach_surface_renderer (GrdRdpSurface         *rdp_surface,
                                         GrdRdpSurfaceRenderer *surface_renderer)
{
  g_assert (!rdp_surface->surface_renderer);

  rdp_surface->surface_renderer = surface_renderer;
}

void
grd_rdp_surface_invalidate_surface (GrdRdpSurface *rdp_surface)
{
  rdp_surface->valid = FALSE;
}

void
grd_rdp_surface_reset (GrdRdpSurface *rdp_surface)
{
  rdp_surface->needs_no_local_data = FALSE;

  if (rdp_surface->avc.main_view)
    {
      grd_hwaccel_nvidia_clear_mem_ptr (rdp_surface->hwaccel_nvidia,
                                        &rdp_surface->avc.main_view);
    }
}
