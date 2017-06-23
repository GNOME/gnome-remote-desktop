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

#include "grd-pipewire-stream-monitor.h"

#include <errno.h>
#include <glib-object.h>
#include <pipewire/client/context.h>
#include <pipewire/client/introspect.h>
#include <pipewire/client/properties.h>
#include <pipewire/client/sig.h>
#include <pipewire/client/stream.h>
#include <pipewire/client/subscribe.h>
#include <spa/defs.h>

#include "grd-context.h"
#include "grd-pipewire-stream.h"

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

#define GRD_PIPEWIRE_CLIENT_NAME "gnome-remote-desktop"

typedef struct _GrdPipeWireSource
{
  GSource base;

  struct pw_loop *pipewire_loop;
} GrdPipeWireSource;

struct _GrdPipeWireStreamMonitor
{
  GObject parent;

  GrdContext *context;

  GHashTable *streams;
  GHashTable *stream_ids;

  struct pw_context *pipewire_context;
  GrdPipeWireSource *pipewire_source;

  struct pw_listener state_changed_listener;
  struct pw_listener subscription_listener;
};

G_DEFINE_TYPE (GrdPipeWireStreamMonitor, grd_pipewire_stream_monitor,
               G_TYPE_OBJECT)

static void
grd_pipewire_stream_monitor_add_stream (GrdPipeWireStreamMonitor *monitor,
                                        uint32_t                  pipewire_node_id,
                                        const char               *stream_id)
{
  GrdPipeWireStream *stream;

  stream = grd_pipewire_stream_new (monitor->context, stream_id, pipewire_node_id);
  g_hash_table_insert (monitor->streams, g_strdup (stream_id), stream);
  g_hash_table_insert (monitor->stream_ids, GUINT_TO_POINTER (pipewire_node_id),
                       g_strdup (stream_id));

  g_signal_emit (monitor, signals[STREAM_ADDED], 0, stream);
}

static void
grd_pipewire_stream_monitor_remove_stream (GrdPipeWireStreamMonitor *monitor,
                                           uint32_t                  pipewire_node_id)
{
  char *stream_id;

  stream_id = g_hash_table_lookup (monitor->stream_ids,
                                   GUINT_TO_POINTER (pipewire_node_id));
  if (stream_id)
    {
      GrdPipeWireStream *stream;

      stream = grd_pipewire_stream_monitor_get_stream (monitor, stream_id);
      grd_pipewire_stream_removed (stream);

      g_hash_table_remove (monitor->streams, stream_id);
      g_hash_table_remove (monitor->stream_ids,
                           GUINT_TO_POINTER (pipewire_node_id));
    }
}

static void
handle_node_info (struct pw_context         *context,
                  int                        result,
                  const struct pw_node_info *info,
                  void                      *user_data)
{
  GrdPipeWireStreamMonitor *monitor = user_data;
  const char *stream_id;

  if (!info)
    return;

  stream_id = spa_dict_lookup (info->props, "gnome.remote_desktop.stream_id");
  if (!stream_id)
    return;

  grd_pipewire_stream_monitor_add_stream (monitor, info->id, stream_id);
}

static void
on_subscription (struct pw_listener        *listener,
                 struct pw_context         *context,
                 enum pw_subscription_event event,
                 uint32_t                   type,
                 uint32_t                   id)
{
  GrdPipeWireStreamMonitor *monitor =
    SPA_CONTAINER_OF (listener,
                      GrdPipeWireStreamMonitor,
                      subscription_listener);

  if (type != context->type.node)
    return;

  switch (event)
    {
    case PW_SUBSCRIPTION_EVENT_NEW:
      pw_context_get_node_info_by_id (context,
                                      id,
                                      handle_node_info,
                                      monitor);
      break;
    case PW_SUBSCRIPTION_EVENT_CHANGE:
      break;
    case PW_SUBSCRIPTION_EVENT_REMOVE:
      grd_pipewire_stream_monitor_remove_stream (monitor, id);
      break;
    }
}

static void
on_state_changed (struct pw_listener *listener,
                  struct pw_context  *context)
{
  switch (context->state)
    {
    case PW_CONTEXT_STATE_ERROR:
      g_warning ("PipeWire context error: %s", context->error);
      exit (1);
      break;

    default:
      g_info ("PipeWire context state: \"%s\"\n",
              pw_context_state_as_string (context->state));
      break;
    }
}

static gboolean
pipewire_loop_source_prepare (GSource *base,
                              int     *timeout)
{
  *timeout = -1;
  return FALSE;
}


static gboolean
pipewire_loop_source_dispatch (GSource    *source,
                               GSourceFunc callback,
                               gpointer    user_data)
{
  GrdPipeWireSource *pipewire_source = (GrdPipeWireSource *) source;
  int result;

  result = pw_loop_iterate (pipewire_source->pipewire_loop, 0);
  if (result == SPA_RESULT_ERRNO)
    {
      g_warning ("pw_loop_iterate failed: %s", strerror (errno));
      exit (1);
    }
  else if (result != SPA_RESULT_OK)
    {
      g_warning ("pw_loop_iterate failed: %d", result);
    }

  return TRUE;
}

static void
pipewire_loop_source_finalize (GSource *source)
{
  GrdPipeWireSource *pipewire_source = (GrdPipeWireSource *) source;

  pw_loop_destroy (pipewire_source->pipewire_loop);
}

static GSourceFuncs pipewire_source_funcs =
{
  pipewire_loop_source_prepare,
  NULL,
  pipewire_loop_source_dispatch,
  pipewire_loop_source_finalize
};

struct pw_context *
grd_pipewire_stream_monitor_get_pipewire_context (GrdPipeWireStreamMonitor *monitor)
{
  return monitor->pipewire_context;
}

GrdPipeWireStream *
grd_pipewire_stream_monitor_get_stream (GrdPipeWireStreamMonitor *monitor,
                                        const char               *stream_id)
{
  return g_hash_table_lookup (monitor->streams, stream_id);
}

GrdPipeWireStreamMonitor *
grd_pipewire_stream_monitor_new (GrdContext *context)
{
  GrdPipeWireStreamMonitor *monitor;

  monitor = g_object_new (GRD_TYPE_PIPEWIRE_STREAM_MONITOR,
                          "context", context,
                          NULL);

  return monitor;
}

static void
grd_pipewire_stream_monitor_finalize (GObject *object)
{
  GrdPipeWireStreamMonitor *monitor = GRD_PIPEWIRE_STREAM_MONITOR (object);

  g_hash_table_destroy (monitor->streams);

  pw_loop_leave (monitor->pipewire_source->pipewire_loop);

  pw_context_destroy (monitor->pipewire_context);

  g_source_destroy ((GSource *) monitor->pipewire_source);
}

static void
grd_pipewire_stream_monitor_set_property (GObject      *object,
                                          guint         prop_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  GrdPipeWireStreamMonitor *monitor = GRD_PIPEWIRE_STREAM_MONITOR (object);

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
grd_pipewire_stream_monitor_get_property (GObject    *object,
                                          guint       prop_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
  GrdPipeWireStreamMonitor *monitor = GRD_PIPEWIRE_STREAM_MONITOR (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, monitor->context);

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static GrdPipeWireSource *
create_pipewire_source (GMainContext *main_context)
{
  GrdPipeWireSource *pipewire_source;

  pipewire_source =
    (GrdPipeWireSource *) g_source_new (&pipewire_source_funcs,
                                        sizeof (GrdPipeWireSource));
  pipewire_source->pipewire_loop = pw_loop_new ();
  g_source_add_unix_fd (&pipewire_source->base,
                        pw_loop_get_fd (pipewire_source->pipewire_loop),
                        G_IO_IN | G_IO_ERR);

  pw_loop_enter (pipewire_source->pipewire_loop);
  g_source_attach (&pipewire_source->base, main_context);

  return pipewire_source;
}

static void
grd_pipewire_stream_monitor_constructed (GObject *object)
{
  GrdPipeWireStreamMonitor *monitor = GRD_PIPEWIRE_STREAM_MONITOR (object);
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
  monitor->pipewire_source = create_pipewire_source (main_context);
  monitor->pipewire_context = pw_context_new (monitor->pipewire_source->pipewire_loop,
                                              GRD_PIPEWIRE_CLIENT_NAME,
                                              NULL);

  pw_signal_add (&monitor->pipewire_context->state_changed,
                 &monitor->state_changed_listener,
                 on_state_changed);
  pw_signal_add (&monitor->pipewire_context->subscription,
                 &monitor->subscription_listener,
                 on_subscription);

  pw_context_connect (monitor->pipewire_context, PW_CONTEXT_FLAG_NONE);
}

static void
grd_pipewire_stream_monitor_init (GrdPipeWireStreamMonitor *monitor)
{
}

static void
grd_pipewire_stream_monitor_class_init (GrdPipeWireStreamMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_pipewire_stream_monitor_finalize;
  object_class->set_property = grd_pipewire_stream_monitor_set_property;
  object_class->get_property = grd_pipewire_stream_monitor_get_property;
  object_class->constructed = grd_pipewire_stream_monitor_constructed;

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
                                        GRD_TYPE_PIPEWIRE_STREAM);
}
