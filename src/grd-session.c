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

#include "grd-session.h"

#include <glib-object.h>

#include "grd-dbus-remote-desktop.h"
#include "grd-context.h"
#include "grd-private.h"
#include "grd-stream-monitor.h"

enum
{
  PROP_0,

  PROP_CONTEXT,
};

enum
{
  STOPPED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

typedef struct _GrdSessionPrivate
{
  GrdContext *context;

  GrdDBusRemoteDesktopSession *session_proxy;

  GrdStream *stream;
  guint stream_removed_handler_id;
  guint stream_added_handler_id;

  GCancellable *cancellable;
} GrdSessionPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdSession, grd_session, G_TYPE_OBJECT);

void
grd_session_stop (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GError *error = NULL;

  if (priv->stream_removed_handler_id)
    {
      g_signal_handler_disconnect (priv->stream, priv->stream_removed_handler_id);
      priv->stream_removed_handler_id = 0;
    }

  GRD_SESSION_GET_CLASS (session)->stop (session);

  if (!priv->session_proxy)
    return;

  if (!grd_dbus_remote_desktop_session_call_stop_sync (priv->session_proxy,
                                                       NULL,
                                                       &error))
    {
      g_warning ("Failed to stop: %s\n", error->message);
    }
  g_clear_object (&priv->session_proxy);

  g_signal_emit (session, signals[STOPPED], 0);
}

static void
on_stream_removed (GrdStream  *stream,
                   GrdSession *session)
{
  grd_session_stop (session);
}

static void
grd_session_set_stream (GrdSession *session,
                        GrdStream  *stream)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  priv->stream = g_object_ref (stream);

  priv->stream_removed_handler_id =
    g_signal_connect (stream, "removed",
                      G_CALLBACK (on_stream_removed),
                      session);

  GRD_SESSION_GET_CLASS (session)->stream_added (session, stream);
}

static void
on_stream_added (GrdStreamMonitor *monitor,
                 GrdStream        *stream,
                 GrdSession       *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  grd_session_set_stream (session, stream);

  g_signal_handler_disconnect (monitor, priv->stream_added_handler_id);
}

static void
on_session_proxy_acquired (GObject      *object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  GrdSession *session = user_data;
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *session_proxy;
  GError *error = NULL;
  const char *pinos_stream_id;
  GrdStreamMonitor *monitor;
  GrdStream *stream;

  session_proxy =
    grd_dbus_remote_desktop_session_proxy_new_finish (result, &error);
  if (!session_proxy)
    {
      g_warning ("Failed to acquire session proxy: %s\n", error->message);
      return;
    }

  priv->session_proxy = session_proxy;

  pinos_stream_id =
    grd_dbus_remote_desktop_session_get_pinos_stream_id (session_proxy);
  monitor = grd_context_get_stream_monitor (priv->context);
  stream = grd_stream_monitor_get_stream (monitor, pinos_stream_id);

  if (stream)
    {
      grd_session_set_stream (session, stream);
    }
  else
    {
      priv->stream_added_handler_id =
        g_signal_connect_object (monitor, "stream-added",
                                 G_CALLBACK (on_stream_added),
                                 session,
                                 0);
    }
}

static void
on_remote_desktop_started (GObject      *source_object,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  GrdSession *session = user_data;
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktop *proxy;
  char *session_path = NULL;
  GDBusConnection *connection;
  GError *error = NULL;

  proxy = grd_context_get_dbus_proxy (priv->context);
  if (!grd_dbus_remote_desktop_call_start_finish (proxy,
                                                  &session_path,
                                                  res,
                                                  &error))
    {
      g_warning ("Failed to start remote desktop session: %s\n", error->message);
      return;
    }

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy));
  grd_dbus_remote_desktop_session_proxy_new (connection,
                                             G_DBUS_PROXY_FLAGS_NONE,
                                             MUTTER_REMOTE_DESKTOP_BUS_NAME,
                                             session_path,
                                             priv->cancellable,
                                             on_session_proxy_acquired,
                                             session);
}

static void
grd_session_constructed (GObject *object)
{
  GrdSession *session = GRD_SESSION (object);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktop *proxy;

  priv->cancellable = g_cancellable_new ();

  proxy = grd_context_get_dbus_proxy (priv->context);
  grd_dbus_remote_desktop_call_start (proxy,
                                      priv->cancellable,
                                      on_remote_desktop_started,
                                      session);
}

static void
grd_session_dispose (GObject *object)
{
  GrdSession *session = GRD_SESSION (object);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  g_clear_object (&priv->stream);

  G_OBJECT_CLASS (grd_session_parent_class)->dispose (object);
}

static void
grd_session_finalize (GObject *object)
{
  GrdSession *session = GRD_SESSION (object);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  g_assert (!priv->session_proxy);
  g_assert (!priv->stream_removed_handler_id);

  if (priv->cancellable)
    g_cancellable_cancel (priv->cancellable);
  g_clear_object (&priv->cancellable);

  G_OBJECT_CLASS (grd_session_parent_class)->finalize (object);
}

static void
grd_session_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  GrdSession *session = GRD_SESSION (object);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      priv->context = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_session_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  GrdSession *session = GRD_SESSION (object);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, priv->context);

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_session_init (GrdSession *session)
{
}

static void
grd_session_class_init (GrdSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = grd_session_constructed;
  object_class->dispose = grd_session_dispose;
  object_class->finalize = grd_session_finalize;
  object_class->set_property = grd_session_set_property;
  object_class->get_property = grd_session_get_property;

  g_object_class_install_property (object_class,
                                   PROP_CONTEXT,
                                   g_param_spec_object ("context",
                                                        "GrdContext",
                                                        "The GrdContext instance",
                                                        GRD_TYPE_CONTEXT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  signals[STOPPED] = g_signal_new ("stopped",
                                   G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST,
                                   0,
                                   NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}
