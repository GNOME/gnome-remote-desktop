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

#include "config.h"

#include "grd-stream.h"

#include "grd-context.h"
#include "grd-stream-monitor.h"

enum
{
  REMOVED,

  LAST_SIGNAL
};

guint signals[LAST_SIGNAL];

struct _GrdStream
{
  GObject parent;

  uint32_t pinos_node_id;

  GrdContext *context;
};

G_DEFINE_TYPE (GrdStream, grd_stream, G_TYPE_OBJECT);

uint32_t
grd_stream_get_pinos_node_id (GrdStream *stream)
{
  return stream->pinos_node_id;
}

GrdStream *grd_stream_new (GrdContext *context,
                           uint32_t    pinos_node_id)
{
  GrdStream *stream;
 
  stream = g_object_new (GRD_TYPE_STREAM, NULL);
  stream->pinos_node_id = pinos_node_id;
  stream->context = context;

  return stream;
}

void
grd_stream_removed (GrdStream *stream)
{
  g_signal_emit (stream, signals[REMOVED], 0);
}

static void
grd_stream_init (GrdStream *stream)
{
}

static void
grd_stream_class_init (GrdStreamClass *klass)
{
  signals[REMOVED] = g_signal_new ("removed",
                                   G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST,
                                   0,
                                   NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}
