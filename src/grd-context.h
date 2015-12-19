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

#ifndef GRD_CONTEXT_H
#define GRD_CONTEXT_H

#include <glib-object.h>

#include "grd-dbus-remote-desktop.h"
#include "grd-types.h"

#define GRD_TYPE_CONTEXT (grd_context_get_type ())
G_DECLARE_FINAL_TYPE (GrdContext, grd_context, GRD, CONTEXT, GObject);

GrdDBusRemoteDesktop *grd_context_get_dbus_proxy (GrdContext *context);

void grd_context_set_dbus_proxy (GrdContext           *context,
                                 GrdDBusRemoteDesktop *proxy);

GrdStreamMonitor *grd_context_get_stream_monitor (GrdContext *context);

#endif /* GRD_CONTEXT_H */
