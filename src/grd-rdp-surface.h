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

#include <gio/gio.h>
#include <stdint.h>

struct _GrdRdpSurface
{
  uint16_t output_origin_x;
  uint16_t output_origin_y;
  uint16_t width;
  uint16_t height;

  uint8_t *last_frame;
  uint8_t *pending_frame;

  gboolean valid;
};

void grd_rdp_surface_free (GrdRdpSurface *rdp_surface);

#endif /* GRD_RDP_SURFACE_H */
