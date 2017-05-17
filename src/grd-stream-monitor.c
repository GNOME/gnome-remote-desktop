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

#include <errno.h>
#include <glib-object.h>
#include <pinos/client/context.h>
#include <pinos/client/introspect.h>
#include <pinos/client/properties.h>
#include <pinos/client/sig.h>
#include <pinos/client/stream.h>
#include <pinos/client/subscribe.h>
#include <spa/defs.h>

#include "grd-context.h"
#include "grd-stream.h"

enum
{
  PROP_0,

  PROP_CONTEXT,
};

enum
{
  STREAM_ADDED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

#define GRD_PINOS_CLIENT_NAME "gnome-remote-desktop"

typedef struct _GrdPinosSource
{
  GSource base;

  PinosLoop *pinos_loop;
} GrdPinosSource;

struct _GrdStreamMonitor
{
  GObject parent;

  GrdContext *context;

  GHashTable *streams;
  GHashTable *stream_ids;

  PinosContext *pinos_context;
  GrdPinosSource *pinos_source;

  PinosListener state_changed_listener;
  PinosListener subscription_listener;
};

G_DEFINE_TYPE (GrdStreamMonitor, grd_stream_monitor, G_TYPE_OBJECT);

static void
grd_stream_monitor_add_stream (GrdStreamMonitor *monitor,
                               uint32_t          pinos_node_id,
                               const char       *stream_id)
{
  GrdStream *stream;

  stream = grd_stream_new (monitor->context, stream_id, pinos_node_id);
  g_hash_table_insert (monitor->streams, g_strdup (stream_id), stream);
  g_hash_table_insert (monitor->stream_ids, GUINT_TO_POINTER (pinos_node_id),
                       g_strdup (stream_id));

  g_signal_emit (monitor, signals[STREAM_ADDED], 0, stream);
}

static void
grd_stream_monitor_remove_stream (GrdStreamMonitor *monitor,
                                  uint32_t          pinos_node_id)
{
  char *stream_id;

  stream_id = g_hash_table_lookup (monitor->stream_ids,
                                   GUINT_TO_POINTER (pinos_node_id));
  if (stream_id)
    {
      GrdStream *stream;

      stream = grd_stream_monitor_get_stream (monitor, stream_id);
      grd_stream_removed (stream);

      g_hash_table_remove (monitor->streams, stream_id);
      g_hash_table_remove (monitor->stream_ids,
                           GUINT_TO_POINTER (pinos_node_id));
    }
}

static void
handle_node_info (PinosContext        *context,
                  SpaResult            result,
                  const PinosNodeInfo *info,
                  void                *user_data)
{
  GrdStreamMonitor *monitor = user_data;
  const char *stream_id;

  if (!info)
    return;

  stream_id = spa_dict_lookup (info->props, "gnome.remote_desktop.stream_id");
  if (!stream_id)
    return;

  grd_stream_monitor_add_stream (monitor, info->id, stream_id);
}

static void
on_subscription (PinosListener         *listener,
                 PinosContext          *context,
                 PinosSubscriptionEvent event,
                 uint32_t               type,
                 uint32_t               id)
{
  GrdStreamMonitor *monitor = SPA_CONTAINER_OF (listener,
                                                GrdStreamMonitor,
                                                subscription_listener);

  if (type != context->type.node)
    return;

  switch (event)
    {
    case PINOS_SUBSCRIPTION_EVENT_NEW:
      pinos_context_get_node_info_by_id (context,
                                         id,
                                         handle_node_info,
                                         monitor);
      break;
    case PINOS_SUBSCRIPTION_EVENT_CHANGE:
      break;
    case PINOS_SUBSCRIPTION_EVENT_REMOVE:
      grd_stream_monitor_remove_stream (monitor, id);
      break;
    }
}

static void
on_state_changed (PinosListener *listener,
                  PinosContext  *context)
{
  switch (context->state)
    {
    case PINOS_CONTEXT_STATE_ERROR:
      g_warning ("Pinos context error: %s", context->error);
      break;

    default:
      g_info ("Pinos context state: \"%s\"\n",
              pinos_context_state_as_string (context->state));
      break;
    }
}

static gboolean
pinos_loop_source_prepare (GSource *base,
                           int     *timeout)
{
  *timeout = -1;
  return FALSE;
}


static gboolean
pinos_loop_source_dispatch (GSource    *source,
                            GSourceFunc callback,
                            gpointer    user_data)
{
  GrdPinosSource *pinos_source = (GrdPinosSource *) source;
  SpaResult result;

  result = pinos_loop_iterate (pinos_source->pinos_loop, 0);
  if (result == SPA_RESULT_ERRNO)
    {
      g_warning ("pinos_loop_iterate failed: %s", strerror (errno));
    }
  else if (result != SPA_RESULT_OK)
    {
      g_warning ("pinos_loop_iterate failed: %d", result);
    }

  return TRUE;
}

static void
pinos_loop_source_finalize (GSource *source)
{
  GrdPinosSource *pinos_source = (GrdPinosSource *) source;

  pinos_loop_destroy (pinos_source->pinos_loop);
}

static GSourceFuncs pinos_source_funcs =
{
  pinos_loop_source_prepare,
  NULL,
  pinos_loop_source_dispatch,
  pinos_loop_source_finalize
};

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

  monitor = g_object_new (GRD_TYPE_STREAM_MONITOR,
                          "context", context,
                          NULL);

  return monitor;
}

static void
grd_stream_monitor_finalize (GObject *object)
{
  GrdStreamMonitor *monitor = GRD_STREAM_MONITOR (object);

  g_hash_table_destroy (monitor->streams);

  pinos_loop_leave (monitor->pinos_source->pinos_loop);

  pinos_context_destroy (monitor->pinos_context);

  g_source_destroy ((GSource *) monitor->pinos_source);
}

static void
grd_stream_monitor_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GrdStreamMonitor *monitor = GRD_STREAM_MONITOR (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      monitor->context = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_stream_monitor_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GrdStreamMonitor *monitor = GRD_STREAM_MONITOR (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, monitor->context);

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static GrdPinosSource *
create_pinos_source (GMainContext *main_context)
{
  GrdPinosSource *pinos_source;

  pinos_source =
    (GrdPinosSource *) g_source_new (&pinos_source_funcs,
                                     sizeof (GrdPinosSource));
  pinos_source->pinos_loop = pinos_loop_new ();
  g_source_add_unix_fd (&pinos_source->base,
                        pinos_loop_get_fd (pinos_source->pinos_loop),
                        G_IO_IN | G_IO_ERR);

  pinos_loop_enter (pinos_source->pinos_loop);
  g_source_attach (&pinos_source->base, main_context);

  return pinos_source;
}

static void
grd_stream_monitor_constructed (GObject *object)
{
  GrdStreamMonitor *monitor = GRD_STREAM_MONITOR (object);
  GMainContext *main_context;

  monitor->streams = g_hash_table_new_full (g_str_hash,
                                            g_str_equal,
                                            g_free,
                                            g_object_unref);
  monitor->stream_ids = g_hash_table_new_full (g_direct_hash,
                                               g_direct_equal,
                                               NULL,
                                               g_free);

  main_context = grd_context_get_main_context (monitor->context);
  monitor->pinos_source = create_pinos_source (main_context);
  monitor->pinos_context = pinos_context_new (monitor->pinos_source->pinos_loop,
                                              GRD_PINOS_CLIENT_NAME,
                                              NULL);

  pinos_signal_add (&monitor->pinos_context->state_changed,
                    &monitor->state_changed_listener,
                    on_state_changed);
  pinos_signal_add (&monitor->pinos_context->subscription,
                    &monitor->subscription_listener,
                    on_subscription);

  pinos_context_connect (monitor->pinos_context, PINOS_CONTEXT_FLAG_NONE);
}

static void
grd_stream_monitor_init (GrdStreamMonitor *monitor)
{
}

static void
grd_stream_monitor_class_init (GrdStreamMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_stream_monitor_finalize;
  object_class->set_property = grd_stream_monitor_set_property;
  object_class->get_property = grd_stream_monitor_get_property;
  object_class->constructed = grd_stream_monitor_constructed;

  g_object_class_install_property (object_class,
                                   PROP_CONTEXT,
                                   g_param_spec_object ("context",
                                                        "GrdContext",
                                                        "The GrdContext instance",
                                                        GRD_TYPE_CONTEXT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  signals[STREAM_ADDED] = g_signal_new ("stream-added",
                                        G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST,
                                        0,
                                        NULL, NULL, NULL,
                                        G_TYPE_NONE, 1,
                                        GRD_TYPE_STREAM);
}
