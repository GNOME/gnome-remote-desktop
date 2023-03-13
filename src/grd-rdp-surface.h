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

#ifndef GRD_RDP_SURFACE_H
#define GRD_RDP_SURFACE_H

#include <ffnvcodec/dynlink_cuda.h>
#include <gio/gio.h>
#include <stdint.h>

#include "grd-types.h"

struct _GrdRdpSurface
{
  uint16_t output_origin_x;
  uint16_t output_origin_y;
  uint16_t width;
  uint16_t height;

  GMutex surface_mutex;
  GrdRdpBuffer *new_framebuffer;
  GrdRdpBuffer *pending_framebuffer;
  GrdRdpDamageDetector *detector;

  GrdHwAccelNvidia *hwaccel_nvidia;
  CUstream cuda_stream;

  struct
  {
    CUdeviceptr main_view;
  } avc;

  gboolean needs_no_local_data;
  gboolean valid;

  GrdRdpGfxSurface *gfx_surface;
  uint32_t refresh_rate;
  gboolean encoding_suspended;
  gboolean rendering_inhibited;
};

GrdRdpSurface *grd_rdp_surface_new (GrdHwAccelNvidia *hwaccel_nvidia,
                                    uint32_t          refresh_rate);

void grd_rdp_surface_free (GrdRdpSurface *rdp_surface);

uint32_t grd_rdp_surface_get_width (GrdRdpSurface *rdp_surface);

uint32_t grd_rdp_surface_get_height (GrdRdpSurface *rdp_surface);

gboolean grd_rdp_surface_is_rendering_inhibited (GrdRdpSurface *rdp_surface);

void grd_rdp_surface_set_size (GrdRdpSurface *rdp_surface,
                               uint32_t       width,
                               uint32_t       height);

void grd_rdp_surface_inhibit_rendering (GrdRdpSurface *rdp_surface);

void grd_rdp_surface_uninhibit_rendering (GrdRdpSurface *rdp_surface);

void grd_rdp_surface_reset (GrdRdpSurface *rdp_surface);

#endif /* GRD_RDP_SURFACE_H */
