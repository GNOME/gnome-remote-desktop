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

#ifndef GRD_PIPEWIRE_STREAM_H
#define GRD_PIPEWIRE_STREAM_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_PIPEWIRE_STREAM (grd_pipewire_stream_get_type ())
G_DECLARE_FINAL_TYPE (GrdPipeWireStream, grd_pipewire_stream,
                      GRD, PIPEWIRE_STREAM,
                      GObject)

uint32_t grd_pipewire_stream_get_node_id (GrdPipeWireStream *stream);

const char * grd_pipewire_stream_get_stream_id (GrdPipeWireStream *stream);

void grd_pipewire_stream_removed (GrdPipeWireStream *stream);

GrdPipeWireStream * grd_pipewire_stream_new (GrdContext *context,
                                             const char *stream_id,
                                             uint32_t    pipewire_node_id);

#endif /* GRD_PIPEWIRE_STREAM_H */
