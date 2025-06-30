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

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_SERVER (grd_rdp_server_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpServer,
                      grd_rdp_server,
                      GRD, RDP_SERVER,
                      GSocketService)

GrdContext *grd_rdp_server_get_context (GrdRdpServer *rdp_server);

gboolean grd_rdp_server_start (GrdRdpServer  *rdp_server,
                               GError       **error);

void grd_rdp_server_stop (GrdRdpServer *rdp_server);

GrdRdpServer *grd_rdp_server_new (GrdContext *context);

void grd_rdp_server_notify_incoming (GSocketService    *service,
                                     GSocketConnection *connection);
