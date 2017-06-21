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

#include "config.h"

#include "grd-stream.h"

#include "grd-context.h"
#include "grd-pipewire-stream.h"
#include "grd-pipewire-stream-monitor.h"

enum
{
  READY,
  CLOSED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct _GrdStreamPrivate
{
  GrdContext *context;

  GrdDBusScreenCastStream *proxy;
  char *stream_id;
  GrdPipeWireStream *pipewire_stream;

  guint stream_removed_handler_id;
  guint stream_added_handler_id;
} GrdStreamPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdStream, grd_stream, G_TYPE_OBJECT)

static void
on_stream_removed (GrdPipeWireStream *pipewire_stream,
                   GrdStream         *stream)
{
  g_signal_emit (stream, signals[CLOSED], 0);
}

GrdPipeWireStream *
grd_stream_get_pipewire_stream (GrdStream *stream)
{
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);

  return priv->pipewire_stream;
}

const char *
grd_stream_get_object_path (GrdStream *stream)
{
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);

  return g_dbus_proxy_get_object_path (G_DBUS_PROXY (priv->proxy));
}

static void
grd_stream_set_pipewire_stream (GrdStream         *stream,
                                GrdPipeWireStream *pipewire_stream)
{
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);

  priv->pipewire_stream = g_object_ref (pipewire_stream);

  priv->stream_removed_handler_id =
    g_signal_connect (pipewire_stream, "removed",
                      G_CALLBACK (on_stream_removed),
                      stream);

  g_signal_emit (stream, signals[READY], 0);
}

static void
on_stream_added (GrdPipeWireStreamMonitor *monitor,
                 GrdPipeWireStream        *pipewire_stream,
                 GrdStream                *stream)
{
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);

  if (!g_str_equal (grd_pipewire_stream_get_stream_id (pipewire_stream),
                    priv->stream_id))
    return;

  grd_stream_set_pipewire_stream (stream, pipewire_stream);

  g_signal_handler_disconnect (monitor, priv->stream_added_handler_id);
  priv->stream_added_handler_id = 0;
}

static void
on_pipewire_stream_added (GrdDBusScreenCastStream *proxy,
                          const char              *stream_id,
                          GrdStream               *stream)
{
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);
  GrdContext *context = priv->context;
  GrdPipeWireStreamMonitor *pipewire_stream_monitor =
    grd_context_get_pipewire_stream_monitor (context);
  GrdPipeWireStream *pipewire_stream;

  priv->stream_id = g_strdup (stream_id);

  pipewire_stream =
    grd_pipewire_stream_monitor_get_stream (pipewire_stream_monitor,
                                            stream_id);
  if (!pipewire_stream)
    {
      priv->stream_added_handler_id =
        g_signal_connect_object (pipewire_stream_monitor, "stream-added",
                                 G_CALLBACK (on_stream_added),
                                 stream, 0);
    }
  else
    {
      grd_stream_set_pipewire_stream (stream, pipewire_stream);
    }
}

GrdStream *
grd_stream_new (GrdContext              *context,
                GrdDBusScreenCastStream *proxy)
{
  GrdStream *stream;
  GrdStreamPrivate *priv;

  stream = g_object_new (GRD_TYPE_STREAM, NULL);
  priv = grd_stream_get_instance_private (stream);

  priv->context = context;
  priv->proxy = proxy;
  g_signal_connect (proxy, "pipewire-stream-added",
                    G_CALLBACK (on_pipewire_stream_added),
                    stream);

  return stream;
}

static void
grd_stream_finalize (GObject *object)
{
  GrdStream *stream = GRD_STREAM (object);
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);

  if (priv->stream_removed_handler_id)
    g_signal_handler_disconnect (priv->pipewire_stream,
                                 priv->stream_removed_handler_id);

  if (priv->stream_added_handler_id)
    {
      GrdContext *context = priv->context;
      GrdPipeWireStreamMonitor *monitor =
        grd_context_get_pipewire_stream_monitor (context);

      g_signal_handler_disconnect (monitor, priv->stream_added_handler_id);
    }

  g_clear_object (&priv->proxy);
  g_clear_pointer (&priv->stream_id, g_free);

  G_OBJECT_CLASS (grd_stream_parent_class)->finalize (object);
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

  signals[READY] = g_signal_new ("ready",
                                 G_TYPE_FROM_CLASS (klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);
  signals[CLOSED] = g_signal_new ("closed",
                                  G_TYPE_FROM_CLASS (klass),
                                  G_SIGNAL_RUN_LAST,
                                  0,
                                  NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}
