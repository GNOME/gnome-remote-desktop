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

enum
{
  READY,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct _GrdStreamPrivate
{
  uint32_t stream_id;
  uint32_t pipewire_node_id;

  GrdDBusMutterScreenCastStream *proxy;

  unsigned long pipewire_stream_added_id;
} GrdStreamPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdStream, grd_stream, G_TYPE_OBJECT)

uint32_t
grd_stream_get_stream_id (GrdStream *stream)
{
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);

  return priv->stream_id;
}

uint32_t
grd_stream_get_pipewire_node_id (GrdStream *stream)
{
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);

  return priv->pipewire_node_id;
}

const char *
grd_stream_get_object_path (GrdStream *stream)
{
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);

  if (!priv->proxy)
    return NULL;

  return g_dbus_proxy_get_object_path (G_DBUS_PROXY (priv->proxy));
}

void
grd_stream_disconnect_proxy_signals (GrdStream *stream)
{
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);

  g_clear_signal_handler (&priv->pipewire_stream_added_id, priv->proxy);
}

static void
on_pipewire_stream_added (GrdDBusMutterScreenCastStream *proxy,
                          unsigned int                   node_id,
                          GrdStream                     *stream)
{
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);

  priv->pipewire_node_id = (uint32_t) node_id;

  g_signal_emit (stream, signals[READY], 0);
}

GrdStream *
grd_stream_new (uint32_t                       stream_id,
                GrdDBusMutterScreenCastStream *proxy)
{
  GrdStream *stream;
  GrdStreamPrivate *priv;

  stream = g_object_new (GRD_TYPE_STREAM, NULL);
  priv = grd_stream_get_instance_private (stream);

  priv->stream_id = stream_id;
  priv->proxy = proxy;
  priv->pipewire_stream_added_id =
    g_signal_connect (proxy, "pipewire-stream-added",
                      G_CALLBACK (on_pipewire_stream_added),
                      stream);

  return stream;
}

void
grd_stream_destroy (GrdStream *stream)
{
  g_object_run_dispose (G_OBJECT (stream));
  g_object_unref (stream);
}

static void
grd_stream_dispose (GObject *object)
{
  GrdStream *stream = GRD_STREAM (object);
  GrdStreamPrivate *priv = grd_stream_get_instance_private (stream);

  g_clear_object (&priv->proxy);

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

  object_class->dispose = grd_stream_dispose;

  signals[READY] = g_signal_new ("ready",
                                 G_TYPE_FROM_CLASS (klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);
}
