/*
 * Copyright (C) 2015 Red Hat Inc.
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
 * Written by:
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_VNC_SERVER (grd_vnc_server_get_type ())
G_DECLARE_FINAL_TYPE (GrdVncServer,
                      grd_vnc_server,
                      GRD, VNC_SERVER,
                      GSocketService)

GrdContext *grd_vnc_server_get_context (GrdVncServer *vnc_server);

gboolean grd_vnc_server_start (GrdVncServer *vnc_server, GError **error);

void grd_vnc_server_stop (GrdVncServer *vnc_server);

GrdVncServer *grd_vnc_server_new (GrdContext *context);
