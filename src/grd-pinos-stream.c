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

#include "grd-pinos-stream.h"

#include "grd-context.h"
#include "grd-pinos-stream-monitor.h"

enum
{
  REMOVED,

  LAST_SIGNAL
};

guint signals[LAST_SIGNAL];

struct _GrdPinosStream
{
  GObject parent;

  char *stream_id;
  uint32_t pinos_node_id;

  GrdContext *context;
};

G_DEFINE_TYPE (GrdPinosStream, grd_pinos_stream, G_TYPE_OBJECT);

uint32_t
grd_pinos_stream_get_node_id (GrdPinosStream *stream)
{
  return stream->pinos_node_id;
}

const char *
grd_pinos_stream_get_stream_id (GrdPinosStream *stream)
{
  return stream->stream_id;
}

GrdPinosStream *grd_pinos_stream_new (GrdContext *context,
				      const char *stream_id,
				      uint32_t    pinos_node_id)
{
  GrdPinosStream *stream;
 
  stream = g_object_new (GRD_TYPE_PINOS_STREAM, NULL);
  stream->stream_id = g_strdup (stream_id);
  stream->pinos_node_id = pinos_node_id;
  stream->context = context;

  return stream;
}

void
grd_pinos_stream_removed (GrdPinosStream *stream)
{
  g_signal_emit (stream, signals[REMOVED], 0);
}

static void
grd_pinos_stream_finalize (GObject *object)
{
  GrdPinosStream *stream = GRD_PINOS_STREAM (object);

  g_free (stream->stream_id);

  G_OBJECT_CLASS (grd_pinos_stream_parent_class)->finalize (object);
}

static void
grd_pinos_stream_init (GrdPinosStream *stream)
{
}

static void
grd_pinos_stream_class_init (GrdPinosStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_pinos_stream_finalize;

  signals[REMOVED] = g_signal_new ("removed",
                                   G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST,
                                   0,
                                   NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}
