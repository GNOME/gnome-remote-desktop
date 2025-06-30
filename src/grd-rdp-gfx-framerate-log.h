/*
 * Copyright (C) 2024 Pascal Nowack
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

#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_GFX_FRAMERATE_LOG (grd_rdp_gfx_framerate_log_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpGfxFramerateLog, grd_rdp_gfx_framerate_log,
                      GRD, RDP_GFX_FRAMERATE_LOG, GObject)

GrdRdpGfxFramerateLog *grd_rdp_gfx_framerate_log_new (void);

void grd_rdp_gfx_framerate_log_notify_frame_stats (GrdRdpGfxFramerateLog *framerate_log,
                                                   GrdRdpFrameStats      *frame_stats);

gboolean grd_rdp_gfx_framerate_log_should_avoid_dual_frame (GrdRdpGfxFramerateLog *framerate_log);
