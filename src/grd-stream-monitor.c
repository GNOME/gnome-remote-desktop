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

#include "grd-stream-monitor.h"

#include <glib-object.h>
#include <pinos/client/context.h>
#include <pinos/client/introspect.h>
#include <pinos/client/properties.h>
#include <pinos/client/stream.h>
#include <pinos/client/subscribe.h>

#include "grd-context.h"
#include "grd-stream.h"

enum
{
  STREAM_ADDED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

#define GRD_PINOS_CLIENT_NAME "gnome-remote-desktop"

struct _GrdStreamMonitor
{
  GObject parent;

  GHashTable *streams;
  GHashTable *stream_ids;

  GrdContext *context;
  PinosContext *pinos_context;
};

G_DEFINE_TYPE (GrdStreamMonitor, grd_stream_monitor, G_TYPE_OBJECT);

static void
grd_stream_monitor_add_stream (GrdStreamMonitor *monitor,
                               gpointer          id,
                               const char       *stream_id,
                               const char       *source_path)
{
  GrdStream *stream;

  stream = grd_stream_new (monitor->context, source_path);
  g_hash_table_insert (monitor->streams, g_strdup (stream_id), stream);
  g_hash_table_insert (monitor->stream_ids, id, g_strdup (stream_id));

  g_signal_emit (monitor, signals[STREAM_ADDED], 0, stream);
}

static void
grd_stream_monitor_remove_stream (GrdStreamMonitor *monitor,
                                  gpointer          id)
{
  char *stream_id;

  stream_id = g_hash_table_lookup (monitor->stream_ids, id);
  if (stream_id)
    {
      GrdStream *stream;

      stream = grd_stream_monitor_get_stream (monitor, stream_id);
      grd_stream_removed (stream);

      g_hash_table_remove (monitor->streams, stream_id);
      g_hash_table_remove (monitor->stream_ids, id);
    }
}

static gboolean
handle_source_output_info (PinosContext                *pinos_context,
                           const PinosSourceOutputInfo *info,
                           gpointer                     user_data)
{
  GrdStreamMonitor *monitor = user_data;
  const char *stream_id;

  if (!info)
    return TRUE;

  stream_id = pinos_properties_get (info->properties,
                                    "gnome.remote_desktop.stream_id");
  if (!stream_id)
    return TRUE;

  grd_stream_monitor_add_stream (monitor,
                                 info->id,
                                 stream_id,
                                 info->source_path);

  return TRUE;
}

static void
on_subscription_event (PinosContext           *pinos_context,
                       PinosSubscriptionEvent  type,
                       PinosSubscriptionFlags  flags,
                       gpointer                id,
                       gpointer                user_data)
{
  GrdStreamMonitor *monitor = user_data;

  g_assert (flags & PINOS_SUBSCRIPTION_FLAG_SOURCE_OUTPUT);

  switch (type)
    {
    case PINOS_SUBSCRIPTION_EVENT_NEW:
      pinos_context_get_source_output_info_by_id (pinos_context,
                                                  id,
                                                  PINOS_SOURCE_OUTPUT_INFO_FLAGS_NONE,
                                                  handle_source_output_info,
                                                  NULL,
                                                  monitor);
      break;
    case PINOS_SUBSCRIPTION_EVENT_CHANGE:
      break;
    case PINOS_SUBSCRIPTION_EVENT_REMOVE:
      grd_stream_monitor_remove_stream (monitor, id);
      break;
    }
}

PinosContext *
grd_stream_monitor_get_pinos_context (GrdStreamMonitor *monitor)
{
  return monitor->pinos_context;
}

GrdStream *
grd_stream_monitor_get_stream (GrdStreamMonitor *monitor,
                               const char       *stream_id)
{
  return g_hash_table_lookup (monitor->streams, stream_id);
}

GrdStreamMonitor *
grd_stream_monitor_new (GrdContext *context)
{
  GrdStreamMonitor *monitor;

  monitor = g_object_new (GRD_TYPE_STREAM_MONITOR, NULL);
  monitor->context = context;

  return monitor;
}

static void
grd_stream_monitor_finalize (GObject *object)
{
  GrdStreamMonitor *monitor = GRD_STREAM_MONITOR (object);

  g_hash_table_destroy (monitor->streams);
}

static void
grd_stream_monitor_init (GrdStreamMonitor *monitor)
{
  monitor->streams = g_hash_table_new_full (g_str_hash,
                                            g_str_equal,
                                            g_free,
                                            g_object_unref);
  monitor->stream_ids = g_hash_table_new_full (g_direct_hash,
                                               g_direct_equal,
                                               NULL,
                                               g_free);

  monitor->pinos_context = pinos_context_new (NULL,
                                              GRD_PINOS_CLIENT_NAME,
                                              NULL);
  g_object_set (monitor->pinos_context,
                "subscription-mask",
                PINOS_SUBSCRIPTION_FLAG_SOURCE_OUTPUT,
                NULL);
  g_signal_connect (monitor->pinos_context,
                    "subscription-event",
                    G_CALLBACK (on_subscription_event),
                    monitor);

  pinos_context_connect (monitor->pinos_context, PINOS_CONTEXT_FLAGS_NONE);
}

static void
grd_stream_monitor_class_init (GrdStreamMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_stream_monitor_finalize;

  signals[STREAM_ADDED] = g_signal_new ("stream-added",
                                        G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST,
                                        0,
                                        NULL, NULL, NULL,
                                        G_TYPE_NONE, 1,
                                        GRD_TYPE_STREAM);
}
