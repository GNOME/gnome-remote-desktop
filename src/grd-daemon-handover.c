/*
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
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
 *     Joan Torres <joan.torres@suse.com>
 */

#include "config.h"

#include "grd-daemon-handover.h"

#include <gio/gunixfdlist.h>
#include <unistd.h>

#include "grd-context.h"
#include "grd-daemon.h"
#include "grd-daemon-utils.h"
#include "grd-dbus-remote-desktop.h"
#include "grd-private.h"
#include "grd-rdp-server.h"
#include "grd-session-rdp.h"
#include "grd-settings.h"

#define MAX_HANDOVER_WAIT_TIME_MS (20 * 1000)

struct _GrdDaemonHandover
{
  GrdDaemon parent;

  GrdDBusRemoteDesktopRdpDispatcher *remote_desktop_dispatcher;
  GrdDBusRemoteDesktopRdpHandover *remote_desktop_handover;

  GrdSession *session;

  unsigned int gnome_remote_desktop_watch_name_id;

  unsigned int logout_source_id;
};

G_DEFINE_TYPE (GrdDaemonHandover, grd_daemon_handover, GRD_TYPE_DAEMON)

static gboolean
grd_daemon_handover_is_ready (GrdDaemon *daemon)
{
  GrdContext *context = grd_daemon_get_context (daemon);

  if (!grd_context_get_mutter_remote_desktop_proxy (context) ||
      !grd_context_get_mutter_screen_cast_proxy (context) ||
      !GRD_DAEMON_HANDOVER (daemon)->remote_desktop_handover)
    return FALSE;

  return TRUE;
}

static void
on_take_client_finished (GObject      *object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  GrdDaemonHandover *daemon_handover = user_data;
  int fd;
  int fd_idx;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocket) socket = NULL;
  g_autoptr (GVariant) fd_variant = NULL;
  g_autoptr (GUnixFDList) fd_list = NULL;
  GrdDBusRemoteDesktopRdpHandover *proxy;
  GSocketConnection *socket_connection;
  GrdRdpServer *rdp_server;

  proxy = GRD_DBUS_REMOTE_DESKTOP_RDP_HANDOVER (object);
  if (!grd_dbus_remote_desktop_rdp_handover_call_take_client_finish (
        proxy, &fd_variant, &fd_list, result, &error))
    {
      g_warning ("[DaemonHandover] Error calling TakeClient dbus method: %s",
                 error->message);
      return;
    }

  g_variant_get (fd_variant, "h", &fd_idx);
  fd = g_unix_fd_list_get (fd_list, fd_idx, &error);
  if (fd == -1)
    {
      g_warning ("[DaemonHandover] Failed to acquire file descriptor: %s",
                 error->message);
      return;
    }

  socket = g_socket_new_from_fd (fd, &error);
  if (!socket)
    {
      g_warning ("[DaemonHandover] Error creating socket: %s",
                 error->message);
      return;
    }

  socket_connection = g_socket_connection_factory_create_connection (socket);

  rdp_server = grd_daemon_get_rdp_server (GRD_DAEMON (daemon_handover));
  grd_rdp_server_notify_incoming (G_SOCKET_SERVICE (rdp_server),
                                  socket_connection);

  g_clear_handle_id (&daemon_handover->logout_source_id, g_source_remove);
}

static void
on_get_system_credentials_finished (GObject      *object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
  GrdDaemonHandover *daemon_handover = user_data;
  GrdContext *context = grd_daemon_get_context (GRD_DAEMON (daemon_handover));
  GrdSettings *settings = grd_context_get_settings (context);
  GCancellable *cancellable =
    grd_daemon_get_cancellable (GRD_DAEMON (daemon_handover));
  g_autoptr (GError) error = NULL;
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;
  const char* object_path;

  if (!grd_dbus_remote_desktop_rdp_handover_call_get_system_credentials_finish (
        daemon_handover->remote_desktop_handover,
        &username,
        &password,
        result,
        &error))
    {
      g_warning ("[DaemonHandover] Failed getting system credentials: %s",
                 error->message);
      return;
    }

  if (!grd_settings_set_rdp_credentials (settings, username, password, &error))
    {
      g_warning ("[DaemonHanodver] Failed overwritting credentials: %s",
                 error->message);
      return;
    }

  object_path = g_dbus_proxy_get_object_path (
                  G_DBUS_PROXY (daemon_handover->remote_desktop_handover));

  g_debug ("[DaemonHandover] At: %s, calling TakeClient", object_path);

  grd_dbus_remote_desktop_rdp_handover_call_take_client (
    daemon_handover->remote_desktop_handover,
    NULL,
    cancellable,
    on_take_client_finished,
    daemon_handover);
}

static void
on_take_client_ready (GrdDBusRemoteDesktopRdpHandover *interface,
                      gboolean                         use_system_credentials,
                      GrdDaemonHandover               *daemon_handover)
{
  GCancellable *cancellable =
    grd_daemon_get_cancellable (GRD_DAEMON (daemon_handover));
  const char* object_path;

  object_path = g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface));

  g_debug ("[DaemonHandover] At: %s, received TakeClientReady signal",
           object_path);

  if (use_system_credentials)
    {
      g_debug ("[DaemonHandover] At: %s, calling GetSystemCredentials",
                object_path);

      grd_dbus_remote_desktop_rdp_handover_call_get_system_credentials (
        interface,
        cancellable,
        on_get_system_credentials_finished,
        daemon_handover);

      return;
    }

  g_debug ("[DaemonHandover] At: %s, calling TakeClient", object_path);

  grd_dbus_remote_desktop_rdp_handover_call_take_client (
    interface,
    NULL,
    cancellable,
    on_take_client_finished,
    daemon_handover);
}

static void
on_start_handover_finished (GObject      *object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  GrdDaemonHandover *daemon_handover = user_data;
  GrdContext *context = grd_daemon_get_context (GRD_DAEMON (daemon_handover));
  GrdSettings *settings = grd_context_get_settings (context);
  GrdDBusRemoteDesktopRdpHandover *proxy;
  g_autofree char *certificate = NULL;
  g_autofree char *key = NULL;
  g_autoptr (GError) error = NULL;

  proxy = GRD_DBUS_REMOTE_DESKTOP_RDP_HANDOVER (object);
  if (!grd_dbus_remote_desktop_rdp_handover_call_start_handover_finish (
        proxy, &certificate, &key, result, &error))
    {
      g_warning ("[DaemonHandover] Failed to start handover: %s",
                 error->message);
      grd_session_manager_call_logout_sync ();
      return;
    }

  g_object_set (G_OBJECT (settings),
                "rdp-server-cert", certificate,
                "rdp-server-key", key,
                NULL);
}

static gboolean
logout (gpointer user_data)
{
  GrdDaemonHandover *daemon_handover = user_data;

  g_warning ("[DaemonHandover] Logging out, handover timeout reached");

  daemon_handover->logout_source_id = 0;
  grd_session_manager_call_logout_sync ();

  return G_SOURCE_REMOVE;
}

static void
start_handover (GrdDaemonHandover *daemon_handover,
                const char        *user_name,
                const char        *password)
{
  GCancellable *cancellable =
    grd_daemon_get_cancellable (GRD_DAEMON (daemon_handover));
  const char *object_path;

  g_assert (!daemon_handover->logout_source_id);

  object_path = g_dbus_proxy_get_object_path (
                  G_DBUS_PROXY (daemon_handover->remote_desktop_handover));

  g_debug ("[DaemonHandover] At: %s, calling StartHandover", object_path);

  grd_dbus_remote_desktop_rdp_handover_call_start_handover (
    daemon_handover->remote_desktop_handover,
    user_name,
    password,
    cancellable,
    on_start_handover_finished,
    daemon_handover);

  daemon_handover->logout_source_id =
    g_timeout_add (MAX_HANDOVER_WAIT_TIME_MS, logout, daemon_handover);
}

static void
on_session_stopped (GrdSession        *session,
                    GrdDaemonHandover *daemon_handover)
{
  grd_session_manager_call_logout_sync ();

  daemon_handover->session = NULL;
}

static void
on_incoming_new_connection (GrdRdpServer      *rdp_server,
                            GrdSession        *session,
                            GrdDaemonHandover *daemon_handover)
{
  g_signal_connect (session, "stopped",
                    G_CALLBACK (on_session_stopped),
                    daemon_handover);

  daemon_handover->session = session;
}

static void
on_rdp_server_started (GrdDaemonHandover *daemon_handover)
{
  GrdDaemon *daemon = GRD_DAEMON (daemon_handover);
  GrdContext *context = grd_daemon_get_context (daemon);
  GrdSettings *settings = grd_context_get_settings (context);
  GrdRdpServer *rdp_server = grd_daemon_get_rdp_server (daemon);
  g_autofree char *user_name = NULL;
  g_autofree char *password = NULL;

  if (!grd_settings_get_rdp_credentials (settings,
                                         &user_name, &password,
                                         NULL))
    g_assert_not_reached ();

  g_signal_connect (daemon_handover->remote_desktop_handover,
                    "take-client-ready", G_CALLBACK (on_take_client_ready),
                    daemon_handover);

  start_handover (daemon_handover, user_name, password);

  g_signal_connect (rdp_server, "incoming-new-connection",
                    G_CALLBACK (on_incoming_new_connection),
                    daemon_handover);
}

static void
on_rdp_server_stopped (GrdDaemonHandover *daemon_handover)
{
  GrdRdpServer *rdp_server =
    grd_daemon_get_rdp_server (GRD_DAEMON (daemon_handover));

  if (daemon_handover->remote_desktop_handover)
    {
      g_signal_handlers_disconnect_by_func (
        daemon_handover->remote_desktop_handover,
        G_CALLBACK (on_take_client_ready),
        daemon_handover);
    }

  g_signal_handlers_disconnect_by_func (rdp_server,
                                        G_CALLBACK (on_incoming_new_connection),
                                        daemon_handover);
}

static void
on_redirect_client (GrdDBusRemoteDesktopRdpHandover *interface,
                    const char                      *routing_token,
                    const char                      *user_name,
                    const char                      *password,
                    GrdDaemonHandover               *daemon_handover)
{
  const char *object_path =
    g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface));
  GrdContext *context = grd_daemon_get_context (GRD_DAEMON (daemon_handover));
  GrdSettings *settings = grd_context_get_settings (context);
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (daemon_handover->session);
  g_autofree char *certificate = NULL;

  g_debug ("[DaemonHandover] At: %s, received RedirectClient signal",
           object_path);

  g_object_get (G_OBJECT (settings),
                "rdp-server-cert", &certificate,
                NULL);

  if (!grd_session_rdp_send_server_redirection (session_rdp, routing_token,
                                                user_name, password,
                                                certificate))
    grd_session_manager_call_logout_sync ();
}

static void
on_remote_desktop_rdp_handover_proxy_acquired (GObject      *object,
                                               GAsyncResult *result,
                                               gpointer      user_data)
{
  GrdDaemonHandover *daemon_handover = user_data;
  g_autoptr (GrdDBusRemoteDesktopRdpHandover) proxy = NULL;
  g_autoptr (GError) error = NULL;

  proxy =
    grd_dbus_remote_desktop_rdp_handover_proxy_new_for_bus_finish (result,
                                                                   &error);
  if (!proxy)
    {
      g_warning ("[DaemonHandover] Failed to create remote desktop handover "
                 "proxy: %s", error->message);
      return;
    }

  daemon_handover->remote_desktop_handover = g_steal_pointer (&proxy);

  g_signal_connect (daemon_handover->remote_desktop_handover, "redirect-client",
                    G_CALLBACK (on_redirect_client), daemon_handover);

  grd_daemon_maybe_enable_services (GRD_DAEMON (daemon_handover));
}

static void
on_remote_desktop_rdp_dispatcher_handover_requested (GObject      *object,
                                                     GAsyncResult *result,
                                                     gpointer      user_data)
{
  GrdDaemonHandover *daemon_handover = user_data;
  GCancellable *cancellable =
    grd_daemon_get_cancellable (GRD_DAEMON (daemon_handover));
  g_autofree char *object_path = NULL;
  g_autoptr (GError) error = NULL;
  gboolean success;

  success =
    grd_dbus_remote_desktop_rdp_dispatcher_call_request_handover_finish (
      daemon_handover->remote_desktop_dispatcher,
      &object_path,
      result,
      &error);
  if (!success)
    {
      g_warning ("[DaemonHandover] Failed requesting remote desktop "
                 "handover: %s", error->message);
      return;
    }

  g_debug ("[DaemonHandover] Using: %s, from dispatcher request",
           object_path);

  grd_dbus_remote_desktop_rdp_handover_proxy_new_for_bus (
    G_BUS_TYPE_SYSTEM,
    G_DBUS_PROXY_FLAGS_NONE,
    REMOTE_DESKTOP_BUS_NAME,
    object_path,
    cancellable,
    on_remote_desktop_rdp_handover_proxy_acquired,
    daemon_handover);
}

static void
on_remote_desktop_rdp_dispatcher_proxy_acquired (GObject      *object,
                                                 GAsyncResult *result,
                                                 gpointer      user_data)
{
  GrdDaemonHandover *daemon_handover = user_data;
  GCancellable *cancellable =
    grd_daemon_get_cancellable (GRD_DAEMON (daemon_handover));
  g_autoptr (GrdDBusRemoteDesktopRdpDispatcher) proxy = NULL;
  g_autoptr (GError) error = NULL;

  if (daemon_handover->remote_desktop_dispatcher)
    return;

  proxy =
    grd_dbus_remote_desktop_rdp_dispatcher_proxy_new_finish (result, &error);
  if (!proxy)
    {
      g_warning ("[DaemonHandover] Failed to create remote desktop "
                 "dispatcher proxy: %s", error->message);
      return;
    }

  daemon_handover->remote_desktop_dispatcher = g_steal_pointer (&proxy);

  grd_dbus_remote_desktop_rdp_dispatcher_call_request_handover (
    daemon_handover->remote_desktop_dispatcher,
    cancellable,
    on_remote_desktop_rdp_dispatcher_handover_requested,
    daemon_handover);
}

static void
on_gnome_remote_desktop_name_appeared (GDBusConnection *connection,
                                       const char      *name,
                                       const char      *name_owner,
                                       gpointer         user_data)
{
  GrdDaemonHandover *daemon_handover = user_data;
  GCancellable *cancellable =
    grd_daemon_get_cancellable (GRD_DAEMON (daemon_handover));

  grd_dbus_remote_desktop_rdp_dispatcher_proxy_new (
    connection,
    G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
    REMOTE_DESKTOP_BUS_NAME,
    REMOTE_DESKTOP_DISPATCHER_OBJECT_PATH,
    cancellable,
    on_remote_desktop_rdp_dispatcher_proxy_acquired,
    daemon_handover);
}

static void
on_gnome_remote_desktop_name_vanished (GDBusConnection *connection,
                                       const char      *name,
                                       gpointer         user_data)
{
  GrdDaemonHandover *daemon_handover = user_data;

  g_warning ("[DaemonHandover] %s name vanished, shutting down daemon",
             REMOTE_DESKTOP_BUS_NAME);

  g_application_release (G_APPLICATION (daemon_handover));
}

GrdDaemonHandover *
grd_daemon_handover_new (GError **error)
{
  GrdContext *context;

  context = grd_context_new (GRD_RUNTIME_MODE_HANDOVER, error);
  if (!context)
    return NULL;

  return g_object_new (GRD_TYPE_DAEMON_HANDOVER,
                       "application-id", GRD_DAEMON_HANDOVER_APPLICATION_ID,
                       "flags", G_APPLICATION_IS_SERVICE,
                       "context", context,
                       NULL);
}

static void
grd_daemon_handover_init (GrdDaemonHandover *daemon_handover)
{
}

static void
grd_daemon_handover_startup (GApplication *app)
{
  GrdDaemonHandover *daemon_handover = GRD_DAEMON_HANDOVER (app);

  grd_daemon_acquire_mutter_dbus_proxies (GRD_DAEMON (daemon_handover));

  g_signal_connect (daemon_handover, "mutter-proxy-acquired",
                    G_CALLBACK (grd_daemon_maybe_enable_services), NULL);

  daemon_handover->gnome_remote_desktop_watch_name_id =
    g_bus_watch_name (G_BUS_TYPE_SYSTEM,
                      REMOTE_DESKTOP_BUS_NAME,
                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                      on_gnome_remote_desktop_name_appeared,
                      on_gnome_remote_desktop_name_vanished,
                      daemon_handover, NULL);

  g_signal_connect (daemon_handover, "rdp-server-started",
                    G_CALLBACK (on_rdp_server_started), NULL);

  g_signal_connect (daemon_handover, "rdp-server-stopped",
                    G_CALLBACK (on_rdp_server_stopped), NULL);

  G_APPLICATION_CLASS (grd_daemon_handover_parent_class)->startup (app);
}

static void
grd_daemon_handover_shutdown (GApplication *app)
{
  GrdDaemonHandover *daemon_handover = GRD_DAEMON_HANDOVER (app);

  g_clear_object (&daemon_handover->remote_desktop_handover);
  g_clear_object (&daemon_handover->remote_desktop_dispatcher);

  g_clear_handle_id (&daemon_handover->gnome_remote_desktop_watch_name_id,
                     g_bus_unwatch_name);

  g_clear_handle_id (&daemon_handover->logout_source_id, g_source_remove);

  G_APPLICATION_CLASS (grd_daemon_handover_parent_class)->shutdown (app);
}

static void
grd_daemon_handover_class_init (GrdDaemonHandoverClass *klass)
{
  GApplicationClass *g_application_class = G_APPLICATION_CLASS (klass);
  GrdDaemonClass *daemon_class = GRD_DAEMON_CLASS (klass);

  g_application_class->startup = grd_daemon_handover_startup;
  g_application_class->shutdown = grd_daemon_handover_shutdown;

  daemon_class->is_daemon_ready = grd_daemon_handover_is_ready;
}
