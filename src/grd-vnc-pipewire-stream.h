/*
 * Copyright (C) 2018 Red Hat Inc.
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
 *
 */

#ifndef GRD_VNC_PIPEWIRE_STREAM_H
#define GRD_VNC_PIPEWIRE_STREAM_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-session-vnc.h"

#define GRD_TYPE_VNC_PIPEWIRE_STREAM grd_vnc_pipewire_stream_get_type ()
G_DECLARE_FINAL_TYPE (GrdVncPipeWireStream, grd_vnc_pipewire_stream,
                      GRD, VNC_PIPEWIRE_STREAM,
                      GObject)

GrdVncPipeWireStream * grd_vnc_pipewire_stream_new (GrdSessionVnc  *session_vnc,
                                                    uint32_t        src_node_id,
                                                    GError        **error);

#endif /* GRD_VNC_PIPEWIRE_STREAM_H */
