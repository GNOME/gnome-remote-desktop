/*
 * Copyright (C) 2017 Red Hat Inc.
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

#ifndef GRD_STREAM_H
#define GRD_STREAM_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-dbus-screen-cast.h"
#include "grd-types.h"

#define GRD_TYPE_STREAM (grd_stream_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdStream, grd_stream, GRD, STREAM, GObject)

struct _GrdStreamClass
{
  GObjectClass parent_class;
};

uint32_t grd_stream_get_pipewire_node_id (GrdStream *stream);

const char * grd_stream_get_object_path (GrdStream *stream);

GrdStream * grd_stream_new (GrdContext              *context,
                            GrdDBusScreenCastStream *proxy);

#endif /* GRD_STREAM_H */
