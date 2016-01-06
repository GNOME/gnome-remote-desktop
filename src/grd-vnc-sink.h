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

#ifndef GRD_VNC_SINK_H
#define GRD_VNC_SINK_H

#include <glib.h>
#include <gst/base/gstbasesink.h>

#include "grd-types.h"

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GstBaseSink, gst_object_unref);

#define GRD_TYPE_VNC_SINK (grd_vnc_sink_get_type ())
G_DECLARE_FINAL_TYPE (GrdVncSink, grd_vnc_sink, GRD, VNC_SINK, GstBaseSink);

GstElement *grd_vnc_sink_new (GrdSessionVnc *session_vnc);

#endif /* GRD_VNC_SINK_H */
