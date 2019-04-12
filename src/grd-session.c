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
#include "grd-stream.h"

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

typedef enum _GrdScreenCastCursorMode
{
  GRD_SCREEN_CAST_CURSOR_MODE_HIDDEN = 0,
  GRD_SCREEN_CAST_CURSOR_MODE_EMBEDDED = 1,
  GRD_SCREEN_CAST_CURSOR_MODE_METADATA = 2,
} GrdScreenCastCursorMode;

typedef struct _GrdSessionPrivate
{
  GrdContext *context;

  GrdDBusRemoteDesktopSession *remote_desktop_session;
  GrdDBusScreenCastSession *screen_cast_session;

  GrdStream *stream;

  GCancellable *cancellable;

  gboolean started;
} GrdSessionPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdSession, grd_session, G_TYPE_OBJECT);

GrdContext *
grd_session_get_context (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  return priv->context;
}

void
grd_session_stop (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  GRD_SESSION_GET_CLASS (session)->stop (session);

  if (priv->remote_desktop_session && priv->started)
    {
      GrdDBusRemoteDesktopSession *proxy = priv->remote_desktop_session;
      GError *error = NULL;

      if (!grd_dbus_remote_desktop_session_call_stop_sync (proxy, NULL, &error))
        {
          g_warning ("Failed to stop: %s\n", error->message);
          g_error_free (error);
        }
    }

  g_clear_object (&priv->remote_desktop_session);
  g_clear_object (&priv->screen_cast_session);

  g_signal_emit (session, signals[STOPPED], 0);
}

void
grd_session_notify_keyboard_keysym (GrdSession *session,
                                    uint32_t    keysym,
                                    GrdKeyState state)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *session_proxy = priv->remote_desktop_session;

  grd_dbus_remote_desktop_session_call_notify_keyboard_keysym (session_proxy,
                                                               keysym,
                                                               state,
                                                               NULL,
                                                               NULL,
                                                               NULL);
}

void
grd_session_notify_pointer_button (GrdSession *session,
                                   int32_t        button,
                                   GrdButtonState state)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *session_proxy = priv->remote_desktop_session;

  grd_dbus_remote_desktop_session_call_notify_pointer_button (session_proxy,
                                                              button,
                                                              state,
                                                              NULL,
                                                              NULL,
                                                              NULL);
}

void
grd_session_notify_pointer_axis_discrete (GrdSession    *session,
                                          GrdPointerAxis axis,
                                          int            steps)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *session_proxy = priv->remote_desktop_session;

  grd_dbus_remote_desktop_session_call_notify_pointer_axis_discrete (
    session_proxy, axis, steps, NULL, NULL, NULL);
}

void
grd_session_notify_pointer_motion_absolute (GrdSession *session,
                                            double      x,
                                            double      y)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  const char *stream_path;

  stream_path = grd_stream_get_object_path (priv->stream);

  grd_dbus_remote_desktop_session_call_notify_pointer_motion_absolute (
    priv->remote_desktop_session, stream_path, x, y, NULL, NULL, NULL);
}

static void
on_stream_ready (GrdStream  *stream,
                 GrdSession *session)
{
  GRD_SESSION_GET_CLASS (session)->stream_ready (session, stream);
}

static void
on_stream_closed (GrdStream  *stream,
                  GrdSession *session)
{
  grd_session_stop (session);
}

static void
on_session_start_finished (GObject      *object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  GrdSession *session = user_data;
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *proxy = priv->remote_desktop_session;
  GError *error = NULL;

  if (!grd_dbus_remote_desktop_session_call_start_finish (proxy,
                                                          result,
                                                          &error))
    {
      g_warning ("Failed to start session: %s", error->message);
      g_error_free (error);

      grd_session_stop (session);

      return;
    }

  priv->started = TRUE;
}

static void
start_session (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *proxy = priv->remote_desktop_session;

  grd_dbus_remote_desktop_session_call_start (proxy,
                                              priv->cancellable,
                                              on_session_start_finished,
                                              session);
}

static void
on_screen_cast_stream_proxy_acquired (GObject      *object,
                                      GAsyncResult *result,
                                      gpointer      user_data)
{
  GrdSession *session = user_data;
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusScreenCastStream *stream_proxy;
  GError *error = NULL;
  GrdStream *stream;

  stream_proxy = grd_dbus_screen_cast_stream_proxy_new_finish (result, &error);
  if (!stream_proxy)
    {
      g_warning ("Failed to acquire stream proxy: %s", error->message);
      g_error_free (error);

      grd_session_stop (session);

      return;
    }

  stream = grd_stream_new (priv->context, stream_proxy);
  g_signal_connect (stream, "ready", G_CALLBACK (on_stream_ready),
                    session);
  g_signal_connect (stream, "closed", G_CALLBACK (on_stream_closed),
                    session);
  priv->stream = stream;

  start_session (session);
}

static void
on_record_monitor_finished (GObject      *object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  GrdSession *session = user_data;
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusScreenCastSession *proxy = priv->screen_cast_session;
  g_autofree char *stream_path = NULL;
  GError *error = NULL;
  GDBusConnection *connection;

  if (!grd_dbus_screen_cast_session_call_record_monitor_finish (proxy,
                                                                &stream_path,
                                                                result,
                                                                &error))
    {
      g_warning ("Failed to record monitor: %s", error->message);
      g_error_free (error);

      grd_session_stop (session);

      return;
    }

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy));
  grd_dbus_screen_cast_stream_proxy_new (connection,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         MUTTER_SCREEN_CAST_BUS_NAME,
                                         stream_path,
                                         priv->cancellable,
                                         on_screen_cast_stream_proxy_acquired,
                                         session);
}

static void
on_screen_cast_session_proxy_acquired (GObject      *object,
                                       GAsyncResult *result,
                                       gpointer      user_data)
{
  GrdSession *session = user_data;
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusScreenCastSession *session_proxy;
  GVariantBuilder properties_builder;
  GError *error = NULL;

  session_proxy =
    grd_dbus_screen_cast_session_proxy_new_finish (result, &error);
  if (!session_proxy)
    {
      g_warning ("Failed to acquire screen cast session proxy: %s\n",
                 error->message);
      g_error_free (error);

      grd_session_stop (session);

      return;
    }

  priv->screen_cast_session = session_proxy;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "cursor-mode",
                         g_variant_new_uint32 (GRD_SCREEN_CAST_CURSOR_MODE_METADATA));

  /* TODO: Support something other than primary monitor */
  grd_dbus_screen_cast_session_call_record_monitor (session_proxy,
                                                    "",
                                                    g_variant_builder_end (&properties_builder),
                                                    priv->cancellable,
                                                    on_record_monitor_finished,
                                                    session);
}

static void
on_screen_cast_session_created (GObject      *source_object,
                                GAsyncResult *res,
                                gpointer      user_data)
{
  GrdSession *session = user_data;
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusScreenCast *screen_cast_proxy;
  g_autofree char *session_path = NULL;
  GError *error = NULL;
  GDBusConnection *connection;

  screen_cast_proxy = grd_context_get_screen_cast_proxy (priv->context);
  if (!grd_dbus_screen_cast_call_create_session_finish (screen_cast_proxy,
                                                        &session_path,
                                                        res,
                                                        &error))
    {
      g_warning ("Failed to start screen cast session: %s\n", error->message);
      g_error_free (error);

      grd_session_stop (session);

      return;
    }

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (screen_cast_proxy));
  grd_dbus_screen_cast_session_proxy_new (connection,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          MUTTER_SCREEN_CAST_BUS_NAME,
                                          session_path,
                                          priv->cancellable,
                                          on_screen_cast_session_proxy_acquired,
                                          session);
}

static void
on_remote_desktop_session_closed (GrdDBusRemoteDesktopSession *session_proxy,
                                  GrdSession                  *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  g_clear_object (&priv->remote_desktop_session);
  g_clear_object (&priv->screen_cast_session);

  grd_session_stop (session);
}

static void
on_remote_desktop_session_proxy_acquired (GObject      *object,
                                          GAsyncResult *result,
                                          gpointer      user_data)
{
  GrdSession *session = user_data;
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *session_proxy;
  GError *error = NULL;
  const char *remote_desktop_session_id;
  GrdDBusScreenCast *screen_cast_proxy;
  GVariantBuilder properties_builder;
  GVariant *properties_variant;

  session_proxy =
    grd_dbus_remote_desktop_session_proxy_new_finish (result, &error);
  if (!session_proxy)
    {
      g_warning ("Failed to acquire remote desktop session proxy: %s\n",
                 error->message);
      g_error_free (error);

      grd_session_stop (session);

      return;
    }

  g_signal_connect (session_proxy, "closed",
                    G_CALLBACK (on_remote_desktop_session_closed),
                    session);

  priv->remote_desktop_session = session_proxy;

  remote_desktop_session_id =
    grd_dbus_remote_desktop_session_get_session_id (session_proxy);

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "remote-desktop-session-id",
                         g_variant_new_string (remote_desktop_session_id));
  properties_variant = g_variant_builder_end (&properties_builder);

  screen_cast_proxy = grd_context_get_screen_cast_proxy (priv->context);
  grd_dbus_screen_cast_call_create_session (screen_cast_proxy,
                                            properties_variant,
                                            priv->cancellable,
                                            on_screen_cast_session_created,
                                            session);
}

static void
on_remote_desktop_session_created (GObject      *source_object,
                                   GAsyncResult *res,
                                   gpointer      user_data)
{
  GrdSession *session = user_data;
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktop *remote_desktop_proxy;
  g_autofree char *session_path = NULL;
  GDBusConnection *connection;
  GError *error = NULL;

  remote_desktop_proxy = grd_context_get_remote_desktop_proxy (priv->context);
  if (!grd_dbus_remote_desktop_call_create_session_finish (remote_desktop_proxy,
                                                           &session_path,
                                                           res,
                                                           &error))
    {
      g_warning ("Failed to start remote desktop session: %s\n", error->message);
      g_error_free (error);

      grd_session_stop (session);

      return;
    }

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (remote_desktop_proxy));
  grd_dbus_remote_desktop_session_proxy_new (connection,
                                             G_DBUS_PROXY_FLAGS_NONE,
                                             MUTTER_REMOTE_DESKTOP_BUS_NAME,
                                             session_path,
                                             priv->cancellable,
                                             on_remote_desktop_session_proxy_acquired,
                                             session);
}

void
grd_session_start (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktop *remote_desktop_proxy;

  priv->cancellable = g_cancellable_new ();

  remote_desktop_proxy = grd_context_get_remote_desktop_proxy (priv->context);
  grd_dbus_remote_desktop_call_create_session (remote_desktop_proxy,
                                               priv->cancellable,
                                               on_remote_desktop_session_created,
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

  g_assert (!priv->remote_desktop_session);

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
