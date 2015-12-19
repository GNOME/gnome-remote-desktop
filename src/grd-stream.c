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

struct _GrdStream
{
  GObject parent;

  char *source_path;

  GrdContext *context;
};

G_DEFINE_TYPE (GrdStream, grd_stream, G_TYPE_OBJECT);

const char *
grd_stream_get_pinos_source_path (GrdStream *stream)
{
  return stream->source_path;
}

GrdStream *grd_stream_new (GrdContext *context,
                           const char *source_path)
{
  GrdStream *stream;
 
  stream = g_object_new (GRD_TYPE_STREAM, NULL);
  stream->source_path = g_strdup (source_path);
  stream->context = context;

  return stream;
}

static void
grd_stream_finalize (GObject *object)
{
  GrdStream *stream = GRD_STREAM (object);

  g_free (stream->source_path);
}

static void
grd_stream_init (GrdStream *stream)
{
}

static void
grd_stream_class_init (GrdStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_stream_finalize;
}
