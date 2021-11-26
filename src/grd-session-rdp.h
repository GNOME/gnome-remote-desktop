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

#ifndef GRD_SESSION_RDP_H
#define GRD_SESSION_RDP_H

#include <gio/gio.h>
#include <glib-object.h>

#include "grd-session.h"
#include "grd-types.h"

#define GRD_TYPE_SESSION_RDP (grd_session_rdp_get_type ())
G_DECLARE_FINAL_TYPE (GrdSessionRdp,
                      grd_session_rdp,
                      GRD, SESSION_RDP,
                      GrdSession)

GrdSessionRdp *grd_session_rdp_new (GrdRdpServer      *rdp_server,
                                    GSocketConnection *connection,
#ifdef HAVE_NVENC
                                    GrdRdpNvenc       *rdp_nvenc,
#endif /* HAVE_NVENC */
                                    int                reserved);

void grd_session_rdp_notify_error (GrdSessionRdp *session_rdp,
                                   uint32_t       error_info);

void grd_session_rdp_notify_graphics_pipeline_reset (GrdSessionRdp *session_rdp);

void grd_session_rdp_notify_graphics_pipeline_ready (GrdSessionRdp *session_rdp);

int grd_session_rdp_get_stride_for_width (GrdSessionRdp *session_rdp,
                                          int            width);

void grd_session_rdp_take_buffer (GrdSessionRdp *session_rdp,
                                  void          *data,
                                  uint16_t       width,
                                  uint16_t       height);

void grd_session_rdp_maybe_encode_pending_frame (GrdSessionRdp *session_rdp,
                                                 GrdRdpSurface *rdp_surface);

void grd_session_rdp_update_pointer (GrdSessionRdp *session_rdp,
                                     uint16_t       hotspot_x,
                                     uint16_t       hotspot_y,
                                     uint16_t       width,
                                     uint16_t       height,
                                     uint8_t       *data);

void grd_session_rdp_hide_pointer (GrdSessionRdp *session_rdp);

#endif /* GRD_SESSION_RDP_H */
