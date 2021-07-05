/*
 * Copyright (C) 2020 Pascal Nowack
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
 */

#include "config.h"

#include "grd-rdp-server.h"

#include <freerdp/channels/channels.h>
#include <freerdp/freerdp.h>
#include <freerdp/primitives.h>
#include <gio/gio.h>
#include <winpr/ssl.h>

#include "grd-context.h"
#include "grd-rdp-nvenc.h"
#include "grd-session-rdp.h"

enum
{
  PROP_0,

  PROP_CONTEXT,
};

struct _GrdRdpServer
{
  GSocketService parent;

  GList *sessions;

  GList *stopped_sessions;
  guint idle_task;

  GrdContext *context;
#ifdef HAVE_NVENC
  GrdRdpNvenc *rdp_nvenc;
#endif /* HAVE_NVENC */
};

G_DEFINE_TYPE (GrdRdpServer, grd_rdp_server, G_TYPE_SOCKET_SERVICE);

GrdContext *
grd_rdp_server_get_context (GrdRdpServer *rdp_server)
{
  return rdp_server->context;
}

GrdRdpServer *
grd_rdp_server_new (GrdContext *context)
{
  GrdRdpServer *rdp_server;

  rdp_server = g_object_new (GRD_TYPE_RDP_SERVER,
                             "context", context,
                             NULL);

  return rdp_server;
}

static void
grd_rdp_server_cleanup_stopped_sessions (GrdRdpServer *rdp_server)
{
  g_list_free_full (rdp_server->stopped_sessions, g_object_unref);
  rdp_server->stopped_sessions = NULL;
}

static gboolean
cleanup_stopped_sessions_idle (GrdRdpServer *rdp_server)
{
  grd_rdp_server_cleanup_stopped_sessions (rdp_server);
  rdp_server->idle_task = 0;

  return G_SOURCE_REMOVE;
}

static void
on_session_stopped (GrdSession   *session,
                    GrdRdpServer *rdp_server)
{
  g_debug ("RDP session stopped");

  rdp_server->stopped_sessions = g_list_append (rdp_server->stopped_sessions,
                                                session);
  rdp_server->sessions = g_list_remove (rdp_server->sessions, session);
  if (!rdp_server->idle_task)
    {
      rdp_server->idle_task =
        g_idle_add ((GSourceFunc) cleanup_stopped_sessions_idle,
                    rdp_server);
    }
}

static gboolean
on_incoming (GSocketService    *service,
             GSocketConnection *connection)
{
  GrdRdpServer *rdp_server = GRD_RDP_SERVER (service);
  GrdSessionRdp *session_rdp;

  g_debug ("New incoming RDP connection");

  if (!(session_rdp = grd_session_rdp_new (rdp_server, connection,
#ifdef HAVE_NVENC
                                           rdp_server->rdp_nvenc,
#endif /* HAVE_NVENC */
                                           0)))
    return TRUE;

  rdp_server->sessions = g_list_append (rdp_server->sessions, session_rdp);
  grd_context_add_session (rdp_server->context, GRD_SESSION (session_rdp));

  g_signal_connect (session_rdp, "stopped",
                    G_CALLBACK (on_session_stopped),
                    rdp_server);

  return TRUE;
}

gboolean
grd_rdp_server_start (GrdRdpServer  *rdp_server,
                      GError       **error)
{
  GrdSettings *settings = grd_context_get_settings (rdp_server->context);

  if (!g_socket_listener_add_inet_port (G_SOCKET_LISTENER (rdp_server),
                                        grd_settings_get_rdp_port (settings),
                                        NULL,
                                        error))
    return FALSE;

  g_signal_connect (rdp_server, "incoming", G_CALLBACK (on_incoming), NULL);

  return TRUE;
}

static void
stop_and_unref_session (GrdSession *session)
{
  grd_session_stop (session);
  g_object_unref (session);
}

static void
grd_rdp_server_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  GrdRdpServer *rdp_server = GRD_RDP_SERVER (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      rdp_server->context = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_rdp_server_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  GrdRdpServer *rdp_server = GRD_RDP_SERVER (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, rdp_server->context);

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_rdp_server_dispose (GObject *object)
{
  GrdRdpServer *rdp_server = GRD_RDP_SERVER (object);

#ifdef HAVE_NVENC
  g_clear_object (&rdp_server->rdp_nvenc);
#endif /* HAVE_NVENC */

  if (rdp_server->idle_task)
    {
      g_source_remove (rdp_server->idle_task);
      rdp_server->idle_task = 0;
    }

  if (rdp_server->stopped_sessions)
    {
      grd_rdp_server_cleanup_stopped_sessions (rdp_server);
    }
  if (rdp_server->sessions)
    {
      g_list_free_full (rdp_server->sessions,
                        (GDestroyNotify) stop_and_unref_session);
      rdp_server->sessions = NULL;
    }

  G_OBJECT_CLASS (grd_rdp_server_parent_class)->dispose (object);
}

static void
grd_rdp_server_constructed (GObject *object)
{
  G_OBJECT_CLASS (grd_rdp_server_parent_class)->constructed (object);
}

static void
grd_rdp_server_init (GrdRdpServer *rdp_server)
{
  winpr_InitializeSSL (WINPR_SSL_INIT_DEFAULT);
  WTSRegisterWtsApiFunctionTable (FreeRDP_InitWtsApi ());

  /*
   * Run the primitives benchmark here to save time, when initializing a session
   */
  primitives_get ();

#ifdef HAVE_NVENC
  rdp_server->rdp_nvenc = grd_rdp_nvenc_new ();
  if (rdp_server->rdp_nvenc)
    {
      g_debug ("[RDP] Initialization of NVENC was successful");
    }
  else
    {
      g_message ("[RDP] Initialization of NVENC failed. "
                 "No hardware acceleration available");
    }
#else
  g_message ("[RDP] RDP backend is built WITHOUT support for NVENC and CUDA. "
             "No hardware acceleration available");
#endif /* HAVE_NVENC */
}

static void
grd_rdp_server_class_init (GrdRdpServerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = grd_rdp_server_set_property;
  object_class->get_property = grd_rdp_server_get_property;
  object_class->dispose = grd_rdp_server_dispose;
  object_class->constructed = grd_rdp_server_constructed;

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
