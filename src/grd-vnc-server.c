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

#include "grd-context.h"
#include "grd-session-vnc.h"

#define GRD_VNC_SERVER_PORT 5900

struct _GrdVncServer
{
  GSocketService parent;

  GList *sessions;

  GList *stopped_sessions;
  guint idle_task;

  GrdContext *context;
};

G_DEFINE_TYPE (GrdVncServer, grd_vnc_server, G_TYPE_SOCKET_SERVICE);

GrdContext *
grd_vnc_server_get_context (GrdVncServer *vnc_server)
{
  return vnc_server->context;
}

GrdVncServer *
grd_vnc_server_new (GrdContext *context)
{
  GrdVncServer *vnc_server;

  vnc_server = g_object_new (GRD_TYPE_VNC_SERVER, NULL);
  vnc_server->context = context;

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
  vnc_server->idle_task = 0;

  return G_SOURCE_REMOVE;
}

static void
on_session_stopped (GrdSession *session, GrdVncServer *vnc_server)
{
  vnc_server->stopped_sessions = g_list_append (vnc_server->stopped_sessions,
                                                session);
  vnc_server->sessions = g_list_remove (vnc_server->sessions, session);
  if (!vnc_server->idle_task)
    {
      vnc_server->idle_task =
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

  if (vnc_server->sessions)
    {
      /* TODO: Add the rfbScreen instance to GrdVncServer to support multiple
       * sessions. */
      return TRUE;
    }

  session_vnc = grd_session_vnc_new (vnc_server, connection);
  vnc_server->sessions = g_list_append (vnc_server->sessions, session_vnc);
  grd_context_add_session (vnc_server->context, GRD_SESSION (session_vnc));

  g_signal_connect (session_vnc, "stopped",
                    G_CALLBACK (on_session_stopped),
                    vnc_server);

  return TRUE;
}

gboolean
grd_vnc_server_start (GrdVncServer *vnc_server, GError **error)
{
  if (!g_socket_listener_add_inet_port (G_SOCKET_LISTENER (vnc_server),
                                        GRD_VNC_SERVER_PORT,
                                        NULL,
                                        error))
    return FALSE;

  g_signal_connect (vnc_server, "incoming", G_CALLBACK (on_incoming), NULL);

  return TRUE;
}

static void
stop_and_unref_session (GrdSession *session)
{
  grd_session_stop (session);
  g_object_unref (session);
}

static void
grd_vnc_server_dispose (GObject *object)
{
  GrdVncServer *vnc_server = GRD_VNC_SERVER (object);

  if (vnc_server->idle_task)
    {
      g_source_remove (vnc_server->idle_task);
      vnc_server->idle_task = 0;
    }

  if (vnc_server->stopped_sessions)
    {
      grd_vnc_server_cleanup_stopped_sessions (vnc_server);
    }
  if (vnc_server->sessions)
    {
      g_list_free_full (vnc_server->sessions,
                        (GDestroyNotify) stop_and_unref_session);
      vnc_server->sessions = NULL;
    }
}

static void
grd_vnc_server_init (GrdVncServer *vnc_server)
{
}

static void
grd_vnc_server_class_init (GrdVncServerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_vnc_server_dispose;
}
