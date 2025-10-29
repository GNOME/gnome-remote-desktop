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

#pragma once

#include <glib-object.h>

#include "grd-enums.h"
#include "grd-rdp-monitor-config.h"
#include "grd-rdp-stream-owner.h"
#include "grd-session.h"

#define GRD_TYPE_RDP_LAYOUT_MANAGER (grd_rdp_layout_manager_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpLayoutManager, grd_rdp_layout_manager,
                      GRD, RDP_LAYOUT_MANAGER, GrdRdpStreamOwner)

GrdRdpLayoutManager *grd_rdp_layout_manager_new (GrdSessionRdp *session_rdp);

void grd_rdp_layout_manager_notify_session_started (GrdRdpLayoutManager *layout_manager);

void grd_rdp_layout_manager_submit_new_monitor_config (GrdRdpLayoutManager *layout_manager,
                                                       GrdRdpMonitorConfig *monitor_config);

gboolean grd_rdp_layout_manager_transform_position (GrdRdpLayoutManager  *layout_manager,
                                                    uint32_t              x,
                                                    uint32_t              y,
                                                    GrdStream           **stream,
                                                    GrdEventMotionAbs    *motion_abs);
