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

#include "grd-types.h"

#define GRD_TYPE_RDP_GFX_SURFACE (grd_rdp_gfx_surface_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpGfxSurface, grd_rdp_gfx_surface,
                      GRD, RDP_GFX_SURFACE, GObject);

#endif /* GRD_RDP_GFX_SURFACE_H */
