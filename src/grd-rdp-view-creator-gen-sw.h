/*
 * Copyright (C) 2025 Pascal Nowack
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

#pragma once

#include <stdint.h>

#include "grd-rdp-view-creator.h"

#define GRD_TYPE_RDP_VIEW_CREATOR_GEN_SW (grd_rdp_view_creator_gen_sw_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpViewCreatorGenSW, grd_rdp_view_creator_gen_sw,
                      GRD, RDP_VIEW_CREATOR_GEN_SW, GrdRdpViewCreator)

GrdRdpViewCreatorGenSW *grd_rdp_view_creator_gen_sw_new (uint32_t surface_width,
                                                         uint32_t surface_height);
