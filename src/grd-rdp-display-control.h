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

#ifndef GRD_RDP_DISPLAY_CONTROL_H
#define GRD_RDP_DISPLAY_CONTROL_H

#include <freerdp/server/disp.h>
#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_DISPLAY_CONTROL (grd_rdp_display_control_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpDisplayControl, grd_rdp_display_control,
                      GRD, RDP_DISPLAY_CONTROL, GObject)

GrdRdpDisplayControl *grd_rdp_display_control_new (GrdRdpLayoutManager *layout_manager,
                                                   GrdSessionRdp       *session_rdp,
                                                   GrdRdpDvcHandler    *dvc_handler,
                                                   HANDLE               vcm,
                                                   uint32_t             max_monitor_count);

void grd_rdp_display_control_maybe_init (GrdRdpDisplayControl *display_control);

#endif /* GRD_RDP_DISPLAY_CONTROL_H */
