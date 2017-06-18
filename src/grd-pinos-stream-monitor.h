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

#ifndef GRD_PINOS_STREAM_MONITOR_H
#define GRD_PINOS_STREAM_MONITOR_H

#include <glib-object.h>
#include <pinos/client/context.h>

#include "grd-types.h"

#define GRD_TYPE_PINOS_STREAM_MONITOR (grd_pinos_stream_monitor_get_type ())
G_DECLARE_FINAL_TYPE (GrdPinosStreamMonitor, grd_pinos_stream_monitor,
                      GRD, PINOS_STREAM_MONITOR, GObject);

PinosContext *grd_pinos_stream_monitor_get_pinos_context (GrdPinosStreamMonitor *monitor);

GrdPinosStream *grd_pinos_stream_monitor_get_stream (GrdPinosStreamMonitor *monitor,
                                                     const char            *stream_id);

GrdPinosStreamMonitor *grd_pinos_stream_monitor_new (GrdContext *context);

#endif /* GRD_PINOS_STREAM_MONITOR_H */
