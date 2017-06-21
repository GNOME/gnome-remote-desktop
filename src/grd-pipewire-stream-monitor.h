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

#ifndef GRD_PIPEWIRE_STREAM_MONITOR_H
#define GRD_PIPEWIRE_STREAM_MONITOR_H

#include <glib-object.h>
#include <pipewire/client/context.h>

#include "grd-types.h"

#define GRD_TYPE_PIPEWIRE_STREAM_MONITOR (grd_pipewire_stream_monitor_get_type ())
G_DECLARE_FINAL_TYPE (GrdPipeWireStreamMonitor, grd_pipewire_stream_monitor,
                      GRD, PIPEWIRE_STREAM_MONITOR, GObject);

struct pw_context * grd_pipewire_stream_monitor_get_pipewire_context (GrdPipeWireStreamMonitor *monitor);

GrdPipeWireStream *grd_pipewire_stream_monitor_get_stream (GrdPipeWireStreamMonitor *monitor,
                                                           const char               *stream_id);

GrdPipeWireStreamMonitor * grd_pipewire_stream_monitor_new (GrdContext *context);

#endif /* GRD_PIPEWIRE_STREAM_MONITOR_H */
