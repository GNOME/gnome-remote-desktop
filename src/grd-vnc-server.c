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

#include <rfb/rfb.h>
#include <gio/gio.h>
#include <rfb/rfb.h>

#include "grd-context.h"
#include "grd-session-vnc.h"
#include "grd-vnc-tls.h"


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
  vnc_server->idle_task = 0;

  return G_SOURCE_REMOVE;
}

static void
on_session_stopped (GrdSession *session, GrdVncServer *vnc_server)
{
  g_debug ("VNC session stopped");

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

  g_debug ("New incoming VNC connection");

  if (vnc_server->sessions)
    {
      /* TODO: Add the rfbScreen instance to GrdVncServer to support multiple
       * sessions. */
      g_debug ("Refusing new VNC connection: already an active session");
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

static void
sync_encryption_settings (GrdVncServer *vnc_server)
{
  GrdSettings *settings = grd_context_get_settings (vnc_server->context);
  rfbSecurityHandler *tls_security_handler;
  GrdVncEncryption encryption;

  tls_security_handler = grd_vnc_tls_get_security_handler ();
  encryption = grd_settings_get_vnc_encryption (settings);

  if (encryption == (GRD_VNC_ENCRYPTION_NONE | GRD_VNC_ENCRYPTION_TLS_ANON))
    {
      rfbRegisterSecurityHandler (tls_security_handler);
      rfbUnregisterChannelSecurityHandler (tls_security_handler);
    }
  else if (encryption == GRD_VNC_ENCRYPTION_NONE)
    {
      rfbUnregisterSecurityHandler (tls_security_handler);
      rfbUnregisterChannelSecurityHandler (tls_security_handler);
    }
  else
    {
      if (encryption != GRD_VNC_ENCRYPTION_TLS_ANON)
        g_warning ("Invalid VNC encryption setting, falling back to TLS-ANON");

      rfbRegisterChannelSecurityHandler (tls_security_handler);
      rfbUnregisterSecurityHandler (tls_security_handler);
    }
}

static void
on_vnc_encryption_changed (GrdSettings  *settings,
                           GrdVncServer *vnc_server)
{
  sync_encryption_settings (vnc_server);
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

static void
stop_and_unref_session (GrdSession *session)
{
  grd_session_stop (session);
  g_object_unref (session);
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

  G_OBJECT_CLASS (grd_vnc_server_parent_class)->dispose (object);
}

static void
grd_vnc_server_constructed (GObject *object)
{
  GrdVncServer *vnc_server = GRD_VNC_SERVER (object);
  GrdSettings *settings = grd_context_get_settings (vnc_server->context);

  if (grd_context_get_debug_flags (vnc_server->context) & GRD_DEBUG_VNC)
    rfbLogEnable (1);
  else
    rfbLogEnable (0);

  g_signal_connect (settings, "vnc-encryption-changed",
                    G_CALLBACK (on_vnc_encryption_changed),
                    vnc_server);
  sync_encryption_settings (vnc_server);

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
