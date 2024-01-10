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

#include "grd-rdp-routing-token.h"
#include "grd-rdp-server.h"

#include <freerdp/channels/channels.h>
#include <freerdp/freerdp.h>
#include <freerdp/primitives.h>
#include <gio/gio.h>
#include <winpr/ssl.h>

#include "grd-context.h"
#include "grd-hwaccel-nvidia.h"
#include "grd-session-rdp.h"
#include "grd-utils.h"

#define RDP_SERVER_N_BINDING_ATTEMPTS 10
#define RDP_SERVER_BINDING_ATTEMPT_INTERVAL_MS 500

enum
{
  PROP_0,

  PROP_CONTEXT,
};

enum
{
  INCOMING_NEW_CONNECTION,
  INCOMING_REDIRECTED_CONNECTION,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _GrdRdpServer
{
  GSocketService parent;

  GList *sessions;

  GList *stopped_sessions;
  guint cleanup_sessions_idle_id;

  GCancellable *cancellable;

  GrdContext *context;
  GrdHwAccelNvidia *hwaccel_nvidia;

  uint32_t pending_binding_attempts;
  unsigned int binding_timeout_source_id;
};

G_DEFINE_TYPE (GrdRdpServer, grd_rdp_server, G_TYPE_SOCKET_SERVICE)

GrdContext *
grd_rdp_server_get_context (GrdRdpServer *rdp_server)
{
  return rdp_server->context;
}

GrdRdpServer *
grd_rdp_server_new (GrdContext *context)
{
  GrdRdpServer *rdp_server;
  GrdEglThread *egl_thread;

  rdp_server = g_object_new (GRD_TYPE_RDP_SERVER,
                             "context", context,
                             NULL);

  egl_thread = grd_context_get_egl_thread (rdp_server->context);
  if (egl_thread &&
      (rdp_server->hwaccel_nvidia = grd_hwaccel_nvidia_new (egl_thread)))
    {
      g_message ("[RDP] Initialization of CUDA was successful");
    }
  else
    {
      g_debug ("[RDP] Initialization of CUDA failed. "
               "No hardware acceleration available");
    }

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
  rdp_server->cleanup_sessions_idle_id = 0;

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
  if (!rdp_server->cleanup_sessions_idle_id)
    {
      rdp_server->cleanup_sessions_idle_id =
        g_idle_add ((GSourceFunc) cleanup_stopped_sessions_idle,
                    rdp_server);
    }
}

static void
on_session_post_connect (GrdSessionRdp *session_rdp,
                         GrdRdpServer  *rdp_server)
{
  g_signal_emit (rdp_server, signals[INCOMING_NEW_CONNECTION], 0, session_rdp);
}

static void
on_routing_token_peeked (GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  GrdRdpServer *rdp_server;
  GSocketConnection *connection;
  GrdSessionRdp *session_rdp;
  GCancellable *server_cancellable;
  g_autofree char *routing_token = NULL;
  g_autoptr (GError) error = NULL;
  gboolean requested_rdstls = FALSE;

  routing_token = grd_routing_token_peek_finish (result,
                                                 &rdp_server,
                                                 &connection,
                                                 &server_cancellable,
                                                 &requested_rdstls,
                                                 &error);
  if (g_cancellable_is_cancelled (server_cancellable))
    return;

  if (error)
    {
      g_warning ("Error peeking routing token: %s", error->message);
      return;
    }

  if (routing_token)
    {
      g_signal_emit (rdp_server, signals[INCOMING_REDIRECTED_CONNECTION],
                     0, routing_token, requested_rdstls, connection);
    }
  else
    {
      if (!(session_rdp = grd_session_rdp_new (rdp_server, connection,
                                               rdp_server->hwaccel_nvidia)))
        return;

      rdp_server->sessions = g_list_append (rdp_server->sessions, session_rdp);

      g_signal_connect (session_rdp, "stopped",
                        G_CALLBACK (on_session_stopped),
                        rdp_server);

      g_signal_connect (session_rdp, "post-connected",
                        G_CALLBACK (on_session_post_connect),
                        rdp_server);
    }
}

static gboolean
on_incoming_as_system_headless (GSocketService    *service,
                                GSocketConnection *connection)
{
  GrdRdpServer *rdp_server = GRD_RDP_SERVER (service);

  grd_routing_token_peek_async (rdp_server,
                                connection,
                                rdp_server->cancellable,
                                on_routing_token_peeked);

  return TRUE;
}

static gboolean
on_incoming (GSocketService    *service,
             GSocketConnection *connection)
{
  GrdRdpServer *rdp_server = GRD_RDP_SERVER (service);
  GrdSessionRdp *session_rdp;

  g_debug ("New incoming RDP connection");

  if (!(session_rdp = grd_session_rdp_new (rdp_server, connection,
                                           rdp_server->hwaccel_nvidia)))
    return TRUE;

  rdp_server->sessions = g_list_append (rdp_server->sessions, session_rdp);

  g_signal_connect (session_rdp, "stopped",
                    G_CALLBACK (on_session_stopped),
                    rdp_server);

  g_signal_connect (session_rdp, "post-connected",
                    G_CALLBACK (on_session_post_connect),
                    rdp_server);

  return TRUE;
}

void
grd_rdp_server_notify_incoming (GSocketService    *service,
                                GSocketConnection *connection)
{
  on_incoming (service, connection);
}

static gboolean
attempt_to_bind_port (gpointer user_data)
{
  GrdRdpServer *rdp_server = user_data;
  GrdSettings *settings = grd_context_get_settings (rdp_server->context);
  GrdDBusRemoteDesktopRdpServer *rdp_server_iface =
    grd_context_get_rdp_server_interface (rdp_server->context);
  uint16_t rdp_port = 0;
  uint16_t selected_rdp_port = 0;

  g_assert (rdp_server->pending_binding_attempts > 0);
  --rdp_server->pending_binding_attempts;

  g_object_get (G_OBJECT (settings),
                "rdp-port", &rdp_port,
                NULL);
  g_assert (rdp_port != 0);

  if (grd_bind_socket (G_SOCKET_LISTENER (rdp_server),
                       rdp_port,
                       &selected_rdp_port,
                       FALSE,
                       NULL))
    {
      g_assert (selected_rdp_port != 0);
      grd_dbus_remote_desktop_rdp_server_set_port (rdp_server_iface,
                                                   selected_rdp_port);

      rdp_server->binding_timeout_source_id = 0;
      return G_SOURCE_REMOVE;
    }

  if (rdp_server->pending_binding_attempts == 0)
    {
      g_warning ("Failed binding to port %u after %u attempts",
                 rdp_port, RDP_SERVER_N_BINDING_ATTEMPTS);

      rdp_server->binding_timeout_source_id = 0;
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
bind_socket (GrdRdpServer  *rdp_server,
             GError       **error)
{
  GrdSettings *settings = grd_context_get_settings (rdp_server->context);
  GrdRuntimeMode runtime_mode = grd_context_get_runtime_mode (rdp_server->context);
  GrdDBusRemoteDesktopRdpServer *rdp_server_iface =
    grd_context_get_rdp_server_interface (rdp_server->context);
  uint16_t rdp_port = 0;
  uint16_t selected_rdp_port = 0;
  gboolean negotiate_port;

  g_object_get (G_OBJECT (settings),
                "rdp-port", &rdp_port,
                "rdp-negotiate-port", &negotiate_port,
                NULL);

  if (runtime_mode != GRD_RUNTIME_MODE_HANDOVER)
    {
      grd_dbus_remote_desktop_rdp_server_emit_binding (rdp_server_iface,
                                                       rdp_port);
    }

  switch (runtime_mode)
    {
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
    case GRD_RUNTIME_MODE_HEADLESS:
      if (!grd_bind_socket (G_SOCKET_LISTENER (rdp_server),
                            rdp_port,
                            &selected_rdp_port,
                            negotiate_port,
                            error))
        return FALSE;
      break;
    case GRD_RUNTIME_MODE_SYSTEM:
      if (grd_bind_socket (G_SOCKET_LISTENER (rdp_server),
                           rdp_port,
                           &selected_rdp_port,
                           FALSE,
                           error))
        break;

      g_assert (!rdp_server->binding_timeout_source_id);
      rdp_server->binding_timeout_source_id =
        g_timeout_add (RDP_SERVER_BINDING_ATTEMPT_INTERVAL_MS,
                       attempt_to_bind_port,
                       rdp_server);
      return TRUE;
    case GRD_RUNTIME_MODE_HANDOVER:
      return TRUE;
    }

  g_assert (selected_rdp_port != 0);
  grd_dbus_remote_desktop_rdp_server_set_port (rdp_server_iface,
                                               selected_rdp_port);

  return TRUE;
}

gboolean
grd_rdp_server_start (GrdRdpServer  *rdp_server,
                      GError       **error)
{
  GrdRuntimeMode runtime_mode = grd_context_get_runtime_mode (rdp_server->context);
  GrdDBusRemoteDesktopRdpServer *rdp_server_iface =
    grd_context_get_rdp_server_interface (rdp_server->context);

  if (!bind_socket (rdp_server, error))
    return FALSE;

  switch (runtime_mode)
    {
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
    case GRD_RUNTIME_MODE_HEADLESS:
      g_signal_connect (rdp_server, "incoming", G_CALLBACK (on_incoming), NULL);
      break;
    case GRD_RUNTIME_MODE_SYSTEM:
      g_signal_connect (rdp_server, "incoming",
                        G_CALLBACK (on_incoming_as_system_headless), NULL);

      g_assert (!rdp_server->cancellable);
      rdp_server->cancellable = g_cancellable_new ();
      break;
    case GRD_RUNTIME_MODE_HANDOVER:
      break;
    }

  grd_dbus_remote_desktop_rdp_server_set_enabled (rdp_server_iface, TRUE);

  return TRUE;
}

void
grd_rdp_server_stop (GrdRdpServer *rdp_server)
{
  GrdDBusRemoteDesktopRdpServer *rdp_server_iface;

  g_socket_service_stop (G_SOCKET_SERVICE (rdp_server));
  g_socket_listener_close (G_SOCKET_LISTENER (rdp_server));

  rdp_server_iface = grd_context_get_rdp_server_interface (rdp_server->context);
  grd_dbus_remote_desktop_rdp_server_set_enabled (rdp_server_iface, FALSE);
  grd_dbus_remote_desktop_rdp_server_set_port (rdp_server_iface, -1);

  while (rdp_server->sessions)
    {
      GrdSession *session = rdp_server->sessions->data;

      grd_session_stop (session);
    }

  g_clear_handle_id (&rdp_server->cleanup_sessions_idle_id, g_source_remove);
  grd_rdp_server_cleanup_stopped_sessions (rdp_server);

  if (rdp_server->cancellable)
    {
      g_cancellable_cancel (rdp_server->cancellable);
      g_clear_object (&rdp_server->cancellable);
    }

  g_clear_handle_id (&rdp_server->binding_timeout_source_id, g_source_remove);

  g_clear_object (&rdp_server->hwaccel_nvidia);
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
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_rdp_server_dispose (GObject *object)
{
  GrdRdpServer *rdp_server = GRD_RDP_SERVER (object);

  g_assert (!rdp_server->sessions);
  g_assert (!rdp_server->binding_timeout_source_id);
  g_assert (!rdp_server->cleanup_sessions_idle_id);
  g_assert (!rdp_server->stopped_sessions);

  g_assert (!rdp_server->hwaccel_nvidia);

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
  rdp_server->pending_binding_attempts = RDP_SERVER_N_BINDING_ATTEMPTS;

  winpr_InitializeSSL (WINPR_SSL_INIT_DEFAULT);
  WTSRegisterWtsApiFunctionTable (FreeRDP_InitWtsApi ());

  /*
   * Run the primitives benchmark here to save time, when initializing a session
   */
  primitives_get ();
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
  signals[INCOMING_NEW_CONNECTION] = g_signal_new ("incoming-new-connection",
                                                   G_TYPE_FROM_CLASS (klass),
                                                   G_SIGNAL_RUN_LAST,
                                                   0,
                                                   NULL, NULL, NULL,
                                                   G_TYPE_NONE, 1, GRD_TYPE_SESSION);
  signals[INCOMING_REDIRECTED_CONNECTION] = g_signal_new ("incoming-redirected-connection",
                                                          G_TYPE_FROM_CLASS (klass),
                                                          G_SIGNAL_RUN_LAST,
                                                          0,
                                                          NULL, NULL, NULL,
                                                          G_TYPE_NONE, 3,
                                                          G_TYPE_STRING,
                                                          G_TYPE_BOOLEAN,
                                                          G_TYPE_SOCKET_CONNECTION);
}
