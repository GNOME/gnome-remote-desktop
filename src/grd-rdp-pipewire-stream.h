/*
 * Copyright (C) 2020 Pascal Nowack
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

#ifndef GRD_RDP_PIPEWIRE_STREAM_H
#define GRD_RDP_PIPEWIRE_STREAM_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-rdp-monitor-config.h"
#include "grd-session-rdp.h"

#define GRD_TYPE_RDP_PIPEWIRE_STREAM grd_rdp_pipewire_stream_get_type ()
G_DECLARE_FINAL_TYPE (GrdRdpPipeWireStream, grd_rdp_pipewire_stream,
                      GRD, RDP_PIPEWIRE_STREAM,
                      GObject)

GrdRdpPipeWireStream *grd_rdp_pipewire_stream_new (GrdSessionRdp               *session_rdp,
                                                   GrdHwAccelNvidia            *hwaccel_nvidia,
                                                   GMainContext                *render_context,
                                                   GrdRdpSurface               *rdp_surface,
                                                   const GrdRdpVirtualMonitor  *virtual_monitor,
                                                   uint32_t                     src_node_id,
                                                   GError                     **error);

#endif /* GRD_RDP_PIPEWIRE_STREAM_H */
