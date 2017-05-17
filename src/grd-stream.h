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

#ifndef GRD_STREAM_H
#define GRD_STREAM_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_STREAM (grd_stream_get_type ())
G_DECLARE_FINAL_TYPE (GrdStream, grd_stream, GRD, STREAM, GObject);

uint32_t grd_stream_get_pinos_node_id (GrdStream *stream);

void grd_stream_removed (GrdStream *stream);

GrdStream *grd_stream_new (GrdContext *context,
                           uint32_t    pinos_node_id);

#endif /* GRD_STREAM_H */
