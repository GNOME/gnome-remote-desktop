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

#include "grd-vnc-server.h"

#include <gio/gio.h>
#include <rfb/rfb.h>

#include "grd-context.h"
#include "grd-debug.h"
#include "grd-session-vnc.h"

enum
{
  PROP_0,

  PROP_CONTEXT,
};

struct _GrdVncServer
{
  GSocketService parent;

  GList *sessions;

  GList *stopped_sessions;
  guint cleanup_sessions_idle_id;

  GrdContext *context;
};

G_DEFINE_TYPE (GrdVncServer, grd_vnc_server, G_TYPE_SOCKET_SERVICE)

GrdContext *
grd_vnc_server_get_context (GrdVncServer *vnc_server)
{
  return vnc_server->context;
}

GrdVncServer *
grd_vnc_server_new (GrdContext *context)
{
  GrdVncServer *vnc_server;

  vnc_server = g_object_new (GRD_TYPE_VNC_SERVER,
                             "context", context,
                             NULL);

  return vnc_server;
}

static void
grd_vnc_server_cleanup_stopped_sessions (GrdVncServer *vnc_server)
{
  g_list_free_full (vnc_server->stopped_sessions, g_object_unref);
  vnc_server->stopped_sessions = NULL;
}

static gboolean
cleanup_stopped_sessions_idle (GrdVncServer *vnc_server)
{
  grd_vnc_server_cleanup_stopped_sessions (vnc_server);
  vnc_server->cleanup_sessions_idle_id = 0;

  return G_SOURCE_REMOVE;
}

static void
on_session_stopped (GrdSession *session, GrdVncServer *vnc_server)
{
  g_debug ("VNC session stopped");

  vnc_server->stopped_sessions = g_list_append (vnc_server->stopped_sessions,
                                                session);
  vnc_server->sessions = g_list_remove (vnc_server->sessions, session);
  if (!vnc_server->cleanup_sessions_idle_id)
    {
      vnc_server->cleanup_sessions_idle_id =
        g_idle_add ((GSourceFunc) cleanup_stopped_sessions_idle,
                    vnc_server);
    }
}

static gboolean
on_incoming (GSocketService    *service,
             GSocketConnection *connection)
{
  GrdVncServer *vnc_server = GRD_VNC_SERVER (service);
  GrdSessionVnc *session_vnc;

  g_debug ("New incoming VNC connection");

  if (vnc_server->sessions)
    {
      /* TODO: Add the rfbScreen instance to GrdVncServer to support multiple
       * sessions. */
      g_message ("Refusing new VNC connection: already an active session");
      return TRUE;
    }

  session_vnc = grd_session_vnc_new (vnc_server, connection);
  vnc_server->sessions = g_list_append (vnc_server->sessions, session_vnc);

  g_signal_connect (session_vnc, "stopped",
                    G_CALLBACK (on_session_stopped),
                    vnc_server);

  return TRUE;
}

gboolean
grd_vnc_server_start (GrdVncServer  *vnc_server,
                      GError       **error)
{
  GrdSettings *settings = grd_context_get_settings (vnc_server->context);

  if (!g_socket_listener_add_inet_port (G_SOCKET_LISTENER (vnc_server),
                                        grd_settings_get_vnc_port (settings),
                                        NULL,
                                        error))
    return FALSE;

  g_signal_connect (vnc_server, "incoming", G_CALLBACK (on_incoming), NULL);

  return TRUE;
}

void
grd_vnc_server_stop (GrdVncServer *vnc_server)
{
  g_socket_service_stop (G_SOCKET_SERVICE (vnc_server));
  g_socket_listener_close (G_SOCKET_LISTENER (vnc_server));

  while (vnc_server->sessions)
    {
      GrdSession *session = vnc_server->sessions->data;

      grd_session_stop (session);
    }

  grd_vnc_server_cleanup_stopped_sessions (vnc_server);
  g_clear_handle_id (&vnc_server->cleanup_sessions_idle_id,
                     g_source_remove);
}

static void
grd_vnc_server_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  GrdVncServer *vnc_server = GRD_VNC_SERVER (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      vnc_server->context = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_vnc_server_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  GrdVncServer *vnc_server = GRD_VNC_SERVER (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, vnc_server->context);

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_vnc_server_dispose (GObject *object)
{
  GrdVncServer *vnc_server = GRD_VNC_SERVER (object);

  g_assert (!vnc_server->sessions);
  g_assert (!vnc_server->stopped_sessions);
  g_assert (!vnc_server->cleanup_sessions_idle_id);

  G_OBJECT_CLASS (grd_vnc_server_parent_class)->dispose (object);
}

static void
grd_vnc_server_constructed (GObject *object)
{
  if (grd_get_debug_flags () & GRD_DEBUG_VNC)
    rfbLogEnable (1);
  else
    rfbLogEnable (0);

  G_OBJECT_CLASS (grd_vnc_server_parent_class)->constructed (object);
}

static void
grd_vnc_server_init (GrdVncServer *vnc_server)
{
}

static void
grd_vnc_server_class_init (GrdVncServerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = grd_vnc_server_set_property;
  object_class->get_property = grd_vnc_server_get_property;
  object_class->dispose = grd_vnc_server_dispose;
  object_class->constructed = grd_vnc_server_constructed;

  g_object_class_install_property (object_class,
                                   PROP_CONTEXT,
                                   g_param_spec_object ("context",
                                                        "GrdContext",
                                                        "The GrdContext instance",
                                                        GRD_TYPE_CONTEXT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}
