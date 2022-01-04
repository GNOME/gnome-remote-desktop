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
#include "grd-rdp-buffer.h"
#include "grd-rdp-damage-detector-memcmp.h"

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

  rdp_surface->detector = (GrdRdpDamageDetector *)
    grd_rdp_damage_detector_memcmp_new ();

  g_mutex_init (&rdp_surface->surface_mutex);

  return g_steal_pointer (&rdp_surface);
}

void
grd_rdp_surface_free (GrdRdpSurface *rdp_surface)
{
  g_assert (!rdp_surface->gfx_surface);

  g_assert (!rdp_surface->new_framebuffer);
  g_assert (!rdp_surface->pending_framebuffer);

  g_mutex_clear (&rdp_surface->surface_mutex);

  g_clear_object (&rdp_surface->detector);
  if (rdp_surface->cuda_stream)
    {
      grd_hwaccel_nvidia_destroy_cuda_stream (rdp_surface->hwaccel_nvidia,
                                              rdp_surface->cuda_stream);
      rdp_surface->cuda_stream = NULL;
    }

  g_free (rdp_surface);
}
