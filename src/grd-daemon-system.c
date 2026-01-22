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

#include "grd-daemon-system.h"

#include <gio/gunixfdlist.h>

#include "grd-context.h"
#include "grd-daemon.h"
#include "grd-daemon-utils.h"
#include "grd-dbus-gdm.h"
#include "grd-dbus-remote-desktop.h"
#include "grd-private.h"
#include "grd-rdp-server.h"
#include "grd-session-rdp.h"
#include "grd-settings.h"
#include "grd-utils.h"

#define MAX_HANDOVER_WAIT_TIME_MS (30 * 1000)

typedef struct
{
  GrdDBusRemoteDesktopRdpHandover *interface;
  GDBusObjectSkeleton *skeleton;
  char *object_path;
  char *sender_name;
} HandoverInterface;

typedef struct
{
  GrdDaemonSystem *daemon_system;

  char *id;

  GrdSession *session;
  GSocketConnection *socket_connection;

  HandoverInterface *handover_src;
  HandoverInterface *handover_dst;

  unsigned int abort_handover_source_id;

  gboolean is_client_mstsc;
  gboolean use_system_credentials;

  GrdDBusGdmRemoteDisplay *remote_display;
} GrdRemoteClient;

struct _GrdDaemonSystem
{
  GrdDaemon parent;

  GrdDBusGdmRemoteDisplayFactory *remote_display_factory_proxy;
  GDBusObjectManager *display_objects;

  unsigned int system_grd_name_id;
  GrdDBusRemoteDesktopRdpDispatcher *dispatcher_skeleton;
  GDBusObjectManagerServer *handover_manager_server;

  GHashTable *remote_clients;
};

G_DEFINE_TYPE (GrdDaemonSystem, grd_daemon_system, GRD_TYPE_DAEMON)

static void
on_remote_display_remote_id_changed (GrdDBusGdmRemoteDisplay *remote_display,
                                     GParamSpec              *pspec,
                                     GrdRemoteClient         *remote_client);

static void
on_gdm_remote_display_session_id_changed (GrdDBusGdmRemoteDisplay *remote_display,
                                          GParamSpec              *pspec,
                                          GrdRemoteClient         *remote_client);

static void
disconnect_from_remote_display (GrdRemoteClient *remote_client)
{
  if (!remote_client->remote_display)
    return;

  g_signal_handlers_disconnect_by_func (remote_client->remote_display,
                                        G_CALLBACK (on_remote_display_remote_id_changed),
                                        remote_client);
  g_signal_handlers_disconnect_by_func (remote_client->remote_display,
                                        G_CALLBACK (on_gdm_remote_display_session_id_changed),
                                        remote_client);

  g_clear_object (&remote_client->remote_display);
}

static gboolean
grd_daemon_system_is_ready (GrdDaemon *daemon)
{
  GrdDaemonSystem *daemon_system = GRD_DAEMON_SYSTEM (daemon);
  GDBusProxy *remote_display_factory_proxy =
    G_DBUS_PROXY (daemon_system->remote_display_factory_proxy);
  GDBusObjectManagerClient *display_objects_manager =
    G_DBUS_OBJECT_MANAGER_CLIENT (daemon_system->display_objects);
  GDBusInterfaceSkeleton *dispatcher_skeleton =
    G_DBUS_INTERFACE_SKELETON (daemon_system->dispatcher_skeleton);
  g_autofree const char *gdm_remote_display_factory_name_owner = NULL;
  g_autofree const char *gdm_display_objects_name_owner = NULL;
  g_autoptr (GDBusConnection) manager_connection = NULL;
  GDBusConnection *dispatcher_connection = NULL;

  if (!daemon_system->remote_display_factory_proxy ||
      !daemon_system->display_objects ||
      !daemon_system->handover_manager_server ||
      !daemon_system->dispatcher_skeleton)
    return FALSE;

  gdm_remote_display_factory_name_owner = g_dbus_proxy_get_name_owner (
                                            remote_display_factory_proxy);
  gdm_display_objects_name_owner = g_dbus_object_manager_client_get_name_owner (
                                     display_objects_manager);
  manager_connection = g_dbus_object_manager_server_get_connection (
                         daemon_system->handover_manager_server);
  dispatcher_connection = g_dbus_interface_skeleton_get_connection (
                            dispatcher_skeleton);

  if (!gdm_remote_display_factory_name_owner ||
      !gdm_display_objects_name_owner ||
      !manager_connection ||
      !dispatcher_connection)
    return FALSE;

  return TRUE;
}

static gboolean
on_handle_take_client (GrdDBusRemoteDesktopRdpHandover *interface,
                       GDBusMethodInvocation           *invocation,
                       GUnixFDList                     *list,
                       GrdRemoteClient                 *remote_client)
{
  GSocket *socket = NULL;
  GVariant *fd_variant = NULL;
  g_autoptr (GUnixFDList) fd_list = NULL;
  int fd;
  int fd_idx;

  g_assert (interface == remote_client->handover_dst->interface);

  g_debug ("[DaemonSystem] At: %s, received TakeClient call",
           remote_client->handover_dst->object_path);

  socket = g_socket_connection_get_socket (remote_client->socket_connection);
  fd = g_socket_get_fd (socket);

  fd_list = g_unix_fd_list_new ();
  fd_idx = g_unix_fd_list_append (fd_list, fd, NULL);
  fd_variant = g_variant_new_handle (fd_idx);

  grd_dbus_remote_desktop_rdp_handover_complete_take_client (interface,
                                                             invocation,
                                                             fd_list,
                                                             fd_variant);

  grd_close_connection_and_notify (remote_client->socket_connection);
  g_clear_object (&remote_client->socket_connection);
  g_clear_handle_id (&remote_client->abort_handover_source_id, g_source_remove);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
abort_handover (gpointer user_data)
{
  GrdRemoteClient *remote_client = user_data;
  GrdDaemonSystem *daemon_system = remote_client->daemon_system;

  g_warning ("[DaemonSystem] Aborting handover, removing remote client with "
             "remote id %s", remote_client->id);

  if (remote_client->session)
    {
      grd_session_rdp_notify_error (GRD_SESSION_RDP (remote_client->session),
                                    GRD_SESSION_RDP_ERROR_SERVER_REDIRECTION);
    }

  g_hash_table_remove (daemon_system->remote_clients, remote_client->id);

  return G_SOURCE_REMOVE;
}

static char *
get_id_from_routing_token (uint32_t routing_token)
{
  return g_strdup_printf (REMOTE_DESKTOP_CLIENT_OBJECT_PATH "/%u",
                          routing_token);
}

static char *
get_routing_token_from_id (const char *id)
{
  g_assert (strlen (id) > strlen (REMOTE_DESKTOP_CLIENT_OBJECT_PATH "/"));

  return g_strdup (id + strlen (REMOTE_DESKTOP_CLIENT_OBJECT_PATH "/"));
}

static char *
get_session_id_of_sender (GDBusConnection  *connection,
                          const char       *name,
                          GCancellable     *cancellable,
                          GError          **error)
{
  char *session_id = NULL;
  gboolean success;
  pid_t pid = 0;
  uid_t uid = 0;

  success = grd_get_pid_of_sender_sync (connection,
                                        name,
                                        &pid,
                                        cancellable,
                                        error);
  if (!success)
    return NULL;

  session_id = grd_get_session_id_from_pid (pid);
  if (session_id)
    return session_id;

  success = grd_get_uid_of_sender_sync (connection,
                                        name,
                                        &uid,
                                        cancellable,
                                        error);
  if (!success)
    return NULL;

  session_id = grd_get_session_id_from_uid (uid);
  if (!session_id)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_NOT_FOUND,
                   "Could not find a session for user %d",
                   (int) uid);
    }

  return session_id;
}

static char *
get_handover_object_path_for_call (GrdDaemonSystem        *daemon_system,
                                   GDBusMethodInvocation  *invocation,
                                   GError                **error)
{
  g_autoptr (GDBusObject) object = NULL;
  g_autofree char *object_path = NULL;
  g_autofree char *session_id = NULL;
  GDBusConnection *connection = NULL;
  const char *sender = NULL;
  GCancellable *cancellable;

  connection = g_dbus_method_invocation_get_connection (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);
  cancellable = grd_daemon_get_cancellable (GRD_DAEMON (daemon_system));

  session_id = get_session_id_of_sender (connection,
                                         sender,
                                         cancellable,
                                         error);
  if (!session_id)
    return NULL;

  object_path = g_strdup_printf ("%s/session%s",
                                 REMOTE_DESKTOP_HANDOVERS_OBJECT_PATH,
                                 session_id);

  object = g_dbus_object_manager_get_object (
             G_DBUS_OBJECT_MANAGER (daemon_system->handover_manager_server),
             object_path);
  if (!object)
    {
      g_set_error (error,
                   G_DBUS_ERROR,
                   G_DBUS_ERROR_UNKNOWN_OBJECT,
                   "No connection waiting for handover");
      return NULL;
    }

  return g_steal_pointer (&object_path);
}

static gboolean
on_authorize_handover_method (GrdDBusRemoteDesktopRdpHandover *interface,
                              GDBusMethodInvocation           *invocation,
                              gpointer                         user_data)
{
  GrdRemoteClient *remote_client = user_data;
  g_autofree char *object_path = NULL;
  g_autoptr (GError) error = NULL;

  g_assert (interface == remote_client->handover_dst->interface);
  g_assert (remote_client->handover_dst->object_path);

  object_path = get_handover_object_path_for_call (remote_client->daemon_system,
                                                   invocation,
                                                   &error);
  if (!object_path)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return FALSE;
    }

  if (g_strcmp0 (object_path, remote_client->handover_dst->object_path) != 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR,
                                                     G_DBUS_ERROR_ACCESS_DENIED,
                                                     "Caller using incorrect "
                                                     "handover");
      return FALSE;
    }

  return TRUE;
}

static gboolean
on_handle_start_handover (GrdDBusRemoteDesktopRdpHandover *interface,
                          GDBusMethodInvocation           *invocation,
                          const char                      *username,
                          const char                      *password,
                          GrdRemoteClient                 *remote_client)
{
  GrdDaemon *daemon = GRD_DAEMON (remote_client->daemon_system);
  GrdContext *context = grd_daemon_get_context (daemon);
  GrdSettings *settings = grd_context_get_settings (context);
  g_autofree char *key = NULL;
  g_autofree char *certificate = NULL;
  g_autofree char *routing_token = NULL;
  g_autoptr (GVariant) redirect_variant = NULL;
  GDBusConnection *connection;
  const char *sender;

  g_assert (interface == remote_client->handover_dst->interface);

  g_debug ("[DaemonSystem] At: %s, received StartHandover call",
           remote_client->handover_dst->object_path );

  if (!remote_client->session && !remote_client->handover_src)
    {
      g_warning ("[DaemonSystem] RDP client disconnected during the handover");
      goto err;
    }

  routing_token = get_routing_token_from_id (remote_client->id);

  g_object_get (G_OBJECT (settings),
                "rdp-server-cert", &certificate,
                "rdp-server-key", &key,
                NULL);

  /* The remote client is at daemon-system */
  if (remote_client->session)
    {
      if (!grd_session_rdp_send_server_redirection (
             GRD_SESSION_RDP (remote_client->session),
             routing_token,
             username,
             password,
             certificate))
        goto err;
    }
  else
    {
      connection = g_dbus_interface_skeleton_get_connection (
                     G_DBUS_INTERFACE_SKELETON (
                     remote_client->handover_src->interface));
      redirect_variant = g_variant_new ("(sss)",
                                        routing_token,
                                        username,
                                        password);
      g_dbus_connection_emit_signal (connection,
                                     remote_client->handover_src->sender_name,
                                     remote_client->handover_src->object_path,
                                     "org.gnome.RemoteDesktop.Rdp.Handover",
                                     "RedirectClient",
                                     redirect_variant,
                                     NULL);
    }

  sender = g_dbus_method_invocation_get_sender (invocation);
  remote_client->handover_dst->sender_name = g_strdup (sender);

  grd_dbus_remote_desktop_rdp_handover_complete_start_handover (
    remote_client->handover_dst->interface,
    invocation,
    certificate,
    key);

  if (remote_client->abort_handover_source_id == 0)
    {
      remote_client->abort_handover_source_id =
        g_timeout_add (MAX_HANDOVER_WAIT_TIME_MS, abort_handover, remote_client);
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;

err:
  g_dbus_method_invocation_return_error (invocation,
                                         G_DBUS_ERROR,
                                         G_DBUS_ERROR_UNKNOWN_OBJECT,
                                         "Failed to start handover");
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
on_handle_get_system_credentials (GrdDBusRemoteDesktopRdpHandover *interface,
                                  GDBusMethodInvocation           *invocation,
                                  GrdRemoteClient                 *remote_client)
{
  GrdDaemon *daemon = GRD_DAEMON (remote_client->daemon_system);
  GrdContext *context = grd_daemon_get_context (daemon);
  GrdSettings *settings = grd_context_get_settings (context);
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;

  g_assert (interface == remote_client->handover_dst->interface);

  g_debug ("[DaemonSystem] At: %s, received GetSystemCredentials call",
           remote_client->handover_dst->object_path);

  if (!remote_client->use_system_credentials)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_IO_ERROR,
                                             "This handover daemon isn't "
                                             "allowed to use system credentials");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!grd_settings_get_rdp_credentials (settings,
                                         &username, &password,
                                         NULL))
    g_assert_not_reached ();

  grd_dbus_remote_desktop_rdp_handover_complete_get_system_credentials (
    remote_client->handover_dst->interface,
    invocation,
    username,
    password);

  remote_client->use_system_credentials = FALSE;

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
handover_iface_free (HandoverInterface *handover)
{
  g_clear_object (&handover->skeleton);
  g_clear_object (&handover->interface);
  g_clear_pointer (&handover->object_path, g_free);
  g_clear_pointer (&handover->sender_name, g_free);
  g_free (handover);
}

static HandoverInterface *
handover_iface_new (const char      *session_id,
                    GrdRemoteClient *remote_client)
{
  HandoverInterface *handover;

  handover = g_new0 (HandoverInterface, 1);

  handover->object_path = g_strdup_printf ("%s/session%s",
                                           REMOTE_DESKTOP_HANDOVERS_OBJECT_PATH,
                                           session_id);

  handover->skeleton = g_dbus_object_skeleton_new (handover->object_path);

  handover->interface = grd_dbus_remote_desktop_rdp_handover_skeleton_new ();
  g_signal_connect (handover->interface, "g-authorize-method",
                    G_CALLBACK (on_authorize_handover_method), remote_client);
  g_signal_connect (handover->interface, "handle-start-handover",
                    G_CALLBACK (on_handle_start_handover), remote_client);
  g_signal_connect (handover->interface, "handle-take-client",
                    G_CALLBACK (on_handle_take_client), remote_client);
  g_signal_connect (handover->interface, "handle-get-system-credentials",
                    G_CALLBACK (on_handle_get_system_credentials), remote_client);

  return handover;
}

static void
register_handover_iface (GrdRemoteClient *remote_client,
                         const char      *session_id)
{
  GrdDaemonSystem *daemon_system = remote_client->daemon_system;
  HandoverInterface *handover;

  handover = handover_iface_new (session_id, remote_client);

  g_debug ("[DaemonSystem] Registering handover at: %s",
           handover->object_path);

  g_dbus_object_manager_server_export (daemon_system->handover_manager_server,
                                       handover->skeleton);

  g_dbus_object_skeleton_add_interface (
    handover->skeleton,
    G_DBUS_INTERFACE_SKELETON (handover->interface));

  g_clear_pointer (&remote_client->handover_src, handover_iface_free);
  remote_client->handover_src = remote_client->handover_dst;
  remote_client->handover_dst = handover;
}

static void
unregister_handover_iface (GrdRemoteClient   *remote_client,
                           HandoverInterface *handover)
{
  GrdDaemonSystem *daemon_system = remote_client->daemon_system;

  if (!handover)
    return;

  g_debug ("[DaemonSystem] Unregistering handover at: %s",
           handover->object_path);

  g_dbus_object_skeleton_remove_interface (
    handover->skeleton,
    G_DBUS_INTERFACE_SKELETON (handover->interface));

  g_dbus_object_manager_server_unexport (daemon_system->handover_manager_server,
                                         handover->object_path);
}

static void
grd_remote_client_free (GrdRemoteClient *remote_client)
{
  disconnect_from_remote_display (remote_client);

  g_clear_pointer (&remote_client->id, g_free);
  if (remote_client->socket_connection)
    grd_close_connection_and_notify (remote_client->socket_connection);
  g_clear_object (&remote_client->socket_connection);
  unregister_handover_iface (remote_client, remote_client->handover_src);
  unregister_handover_iface (remote_client, remote_client->handover_dst);
  g_clear_pointer (&remote_client->handover_src, handover_iface_free);
  g_clear_pointer (&remote_client->handover_dst, handover_iface_free);
  g_clear_handle_id (&remote_client->abort_handover_source_id, g_source_remove);

  g_free (remote_client);
}

static char *
get_next_available_id (GrdDaemonSystem *daemon_system)
{
  uint32_t routing_token = 0;

  while (routing_token == 0)
    {
      g_autofree char *id = NULL;

      routing_token = g_random_int ();
      id = get_id_from_routing_token (routing_token);

      if (!g_hash_table_contains (daemon_system->remote_clients, id))
        break;

      routing_token = 0;
    }

  return get_id_from_routing_token (routing_token);
}

static void
session_disposed (GrdRemoteClient *remote_client)
{
  remote_client->session = NULL;
}

static GrdRemoteClient *
remote_client_new (GrdDaemonSystem *daemon_system,
                   GrdSession      *session)
{
  GrdRemoteClient *remote_client;

  remote_client = g_new0 (GrdRemoteClient, 1);
  remote_client->id = get_next_available_id (daemon_system);
  remote_client->daemon_system = daemon_system;
  remote_client->is_client_mstsc = grd_session_rdp_is_client_mstsc (GRD_SESSION_RDP (session));
  remote_client->session = session;
  g_object_weak_ref (G_OBJECT (session),
                     (GWeakNotify) session_disposed,
                     remote_client);

  remote_client->abort_handover_source_id =
    g_timeout_add (MAX_HANDOVER_WAIT_TIME_MS, abort_handover, remote_client);

  return remote_client;
}

static void
on_create_remote_display_finished (GObject      *object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  GrdDBusGdmRemoteDisplayFactory *proxy =
    GRD_DBUS_GDM_REMOTE_DISPLAY_FACTORY (object);
  GrdRemoteClient *remote_client = user_data;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_gdm_remote_display_factory_call_create_remote_display_finish (
         proxy, result, &error))
    {
      g_warning ("[DaemonSystem] Error while calling CreateRemoteDisplay on "
                 "DisplayMananger: %s", error->message);
      abort_handover (remote_client);
    }
}

static void
on_incoming_redirected_connection (GrdRdpServer      *rdp_server,
                                   const char        *routing_token_str,
                                   gboolean           requested_rdstls,
                                   GSocketConnection *connection,
                                   GrdDaemonSystem   *daemon_system)
{
  g_autofree char *remote_id = NULL;
  GrdRemoteClient *remote_client;
  uint64_t routing_token;
  gboolean success;

  success = g_ascii_string_to_unsigned (routing_token_str, 10, 0, UINT32_MAX,
                                        &routing_token, NULL);
  if (!success)
    {
      g_autofree char *encoded_token = NULL;

      g_warning ("[DaemonSystem] Incoming client connection used "
                 "invalid routing token");

      encoded_token = g_base64_encode ((unsigned char *) routing_token_str,
                                       strlen (routing_token_str));
      g_debug ("[DaemonSystem] Invalid routing token: %s", encoded_token);
      return;
    }

  g_debug ("[DaemonSystem] Incoming connection with routing token: %u",
           (uint32_t) routing_token);

  remote_id = get_id_from_routing_token ((uint32_t) routing_token);
  if (!g_hash_table_lookup_extended (daemon_system->remote_clients,
                                     remote_id, NULL,
                                     (gpointer *) &remote_client))
    {
      g_warning ("[DaemonSystem] Could not find routing token on "
                 "remote_clients list");
      return;
    }

  remote_client->socket_connection = g_object_ref (connection);
  remote_client->use_system_credentials = remote_client->is_client_mstsc &&
                                          !requested_rdstls;

  g_debug ("[DaemonSystem] At: %s, emitting TakeClientReady signal",
           remote_client->handover_dst->object_path);

  grd_dbus_remote_desktop_rdp_handover_emit_take_client_ready (
    remote_client->handover_dst->interface,
    remote_client->use_system_credentials);
}

static void
on_incoming_new_connection (GrdRdpServer    *rdp_server,
                            GrdSession      *session,
                            GrdDaemonSystem *daemon_system)
{
  GCancellable *cancellable =
    grd_daemon_get_cancellable (GRD_DAEMON (daemon_system));
  GrdRemoteClient *remote_client;

  g_debug ("[DaemonSystem] Incoming connection without routing token");

  remote_client = remote_client_new (daemon_system, session);

  g_hash_table_insert (daemon_system->remote_clients,
                       remote_client->id,
                       remote_client);

  g_debug ("[DaemonSystem] Creating remote display with remote id: %s",
           remote_client->id);

  grd_dbus_gdm_remote_display_factory_call_create_remote_display (
    daemon_system->remote_display_factory_proxy,
    remote_client->id,
    cancellable,
    on_create_remote_display_finished,
    remote_client);
}

static void
inform_configuration_service (void)
{
  g_autoptr (GrdDBusRemoteDesktopConfigurationRdpServer) configuration = NULL;

  configuration =
    grd_dbus_remote_desktop_configuration_rdp_server_proxy_new_for_bus_sync (
      G_BUS_TYPE_SYSTEM,
      G_DBUS_PROXY_FLAGS_NONE,
      REMOTE_DESKTOP_CONFIGURATION_BUS_NAME,
      REMOTE_DESKTOP_CONFIGURATION_OBJECT_PATH,
      NULL,
      NULL);
}

static void
on_rdp_server_started (GrdDaemonSystem *daemon_system)
{
  GrdRdpServer *rdp_server =
    grd_daemon_get_rdp_server (GRD_DAEMON (daemon_system));

  g_signal_connect (rdp_server, "incoming-new-connection",
                    G_CALLBACK (on_incoming_new_connection),
                    daemon_system);
  g_signal_connect (rdp_server, "incoming-redirected-connection",
                    G_CALLBACK (on_incoming_redirected_connection),
                    daemon_system);

  inform_configuration_service ();
}

static void
on_rdp_server_stopped (GrdDaemonSystem *daemon_system)
{
  GrdRdpServer *rdp_server =
    grd_daemon_get_rdp_server (GRD_DAEMON (daemon_system));

  g_signal_handlers_disconnect_by_func (rdp_server,
                                        G_CALLBACK (on_incoming_new_connection),
                                        daemon_system);
  g_signal_handlers_disconnect_by_func (rdp_server,
                                        G_CALLBACK (on_incoming_redirected_connection),
                                        daemon_system);

  inform_configuration_service ();
}

static void
get_handover_object_path_for_call_in_thread (GTask        *task,
                                             gpointer      source_object,
                                             gpointer      task_data,
                                             GCancellable *cancellable)
{
  GDBusMethodInvocation *invocation = task_data;
  GrdDaemonSystem *daemon_system = source_object;
  GError *error = NULL;
  char *object_path;

  object_path = get_handover_object_path_for_call (daemon_system,
                                                   invocation,
                                                   &error);
  if (error)
    {
      g_task_return_error (task, error);
      return;
    }

  g_task_return_pointer (task, object_path, g_free);
}

static void
get_handover_object_path_for_call_async (gpointer            source_object,
                                         gpointer            user_data,
                                         GAsyncReadyCallback on_finished_callback)
{
  GrdDaemonSystem *daemon_system = source_object;
  GCancellable *cancellable =
    grd_daemon_get_cancellable (GRD_DAEMON (daemon_system));
  GTask *task;

  task = g_task_new (daemon_system, cancellable, on_finished_callback, user_data);
  g_task_set_task_data (task, user_data, NULL);
  g_task_run_in_thread (task, get_handover_object_path_for_call_in_thread);
  g_object_unref (task);
}

static void
on_get_handover_object_path_finished (GObject      *source_object,
                                      GAsyncResult *result,
                                      gpointer      user_data)
{
  GrdDaemonSystem *daemon_system = GRD_DAEMON_SYSTEM (source_object);
  GDBusMethodInvocation *invocation = user_data;
  g_autofree char *object_path = NULL;
  g_autoptr (GError) error = NULL;

  object_path = g_task_propagate_pointer (G_TASK (result), &error);
  if (error)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  grd_dbus_remote_desktop_rdp_dispatcher_complete_request_handover (
    daemon_system->dispatcher_skeleton,
    invocation,
    object_path);
}

static gboolean
on_handle_request_handover (GrdDBusRemoteDesktopRdpDispatcher *skeleton,
                            GDBusMethodInvocation             *invocation,
                            gpointer                           user_data)
{
  GrdDaemonSystem *daemon_system = user_data;

  get_handover_object_path_for_call_async (daemon_system,
                                           invocation,
                                           on_get_handover_object_path_finished);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_system_grd_bus_acquired (GDBusConnection *connection,
                            const char      *name,
                            gpointer         user_data)
{
  GrdDaemonSystem *daemon_system = user_data;

  g_debug ("[DaemonSystem] Now on system bus");

  g_dbus_object_manager_server_set_connection (
    daemon_system->handover_manager_server,
    connection);

  g_dbus_interface_skeleton_export (
    G_DBUS_INTERFACE_SKELETON (daemon_system->dispatcher_skeleton),
    connection,
    REMOTE_DESKTOP_DISPATCHER_OBJECT_PATH,
    NULL);

  grd_daemon_maybe_enable_services (GRD_DAEMON (daemon_system));
}

static void
on_system_grd_name_acquired (GDBusConnection *connection,
                             const char      *name,
                             gpointer         user_data)
{
  g_debug ("[DaemonSystem] Owned %s name", name);
}

static void
on_system_grd_name_lost (GDBusConnection *connection,
                         const char      *name,
                         gpointer         user_data)
{
  g_debug ("[DaemonSystem] Lost owned %s name", name);
}

static void
on_remote_display_factory_name_owner_changed (GrdDBusGdmRemoteDisplayFactory *remote_display_factory_proxy,
                                              GParamSpec                     *pspec,
                                              GrdDaemonSystem                *daemon_system)
{
  g_autofree const char *name_owner = NULL;

  g_object_get (G_OBJECT (remote_display_factory_proxy),
                "g-name-owner", &name_owner,
                NULL);
  if (!name_owner)
    {
      grd_daemon_disable_services (GRD_DAEMON (daemon_system));
      return;
    }

  grd_daemon_maybe_enable_services (GRD_DAEMON (daemon_system));
}

static void
on_remote_display_factory_proxy_acquired (GObject      *object,
                                          GAsyncResult *result,
                                          gpointer      user_data)
{
  GrdDaemonSystem *daemon_system = user_data;
  g_autoptr (GError) error = NULL;

  daemon_system->remote_display_factory_proxy =
    grd_dbus_gdm_remote_display_factory_proxy_new_for_bus_finish (result,
                                                                  &error);
  if (!daemon_system->remote_display_factory_proxy)
    {
      g_warning ("[DaemonSystem] Failed to acquire GDM remote display "
                 "factory proxy: %s", error->message);
      return;
    }

  g_signal_connect (G_OBJECT (daemon_system->remote_display_factory_proxy),
                    "notify::g-name-owner",
                    G_CALLBACK (on_remote_display_factory_name_owner_changed),
                    daemon_system);

  grd_daemon_maybe_enable_services (GRD_DAEMON (daemon_system));
}

static void
on_gdm_remote_display_session_id_changed (GrdDBusGdmRemoteDisplay *remote_display,
                                          GParamSpec              *pspec,
                                          GrdRemoteClient         *remote_client)
{
  const char *session_id;

  session_id = grd_dbus_gdm_remote_display_get_session_id (remote_display);

  g_debug ("[DaemonSystem] GDM added a new remote display with remote id: %s "
           "and session: %s",
           remote_client->id,
           session_id);

  register_handover_iface (remote_client, session_id);

  g_signal_handlers_disconnect_by_func (
    remote_display,
    G_CALLBACK (on_gdm_remote_display_session_id_changed),
    remote_client);
}

static void
on_remote_display_remote_id_changed (GrdDBusGdmRemoteDisplay *remote_display,
                                     GParamSpec              *pspec,
                                     GrdRemoteClient         *remote_client)
{
  GrdDaemonSystem *daemon_system = remote_client->daemon_system;
  GrdRemoteClient *new_remote_client;
  const char *remote_id;
  const char *session_id;

  remote_id = grd_dbus_gdm_remote_display_get_remote_id (remote_display);
  if (!g_hash_table_lookup_extended (daemon_system->remote_clients,
                                     remote_id, NULL,
                                     (gpointer *) &new_remote_client))
    {
      g_debug ("[DaemonSystem] GDM set to a remote display a remote "
               "id %s we didn't know about", remote_id);
      return;
    }

  g_debug ("[DaemonSystem] GDM updated a remote display with a new remote id: "
           "%s", remote_id);

  disconnect_from_remote_display (new_remote_client);
  new_remote_client->remote_display = g_object_ref (remote_display);
  g_signal_connect (remote_display, "notify::remote-id",
                    G_CALLBACK (on_remote_display_remote_id_changed),
                    new_remote_client);

  g_hash_table_remove (daemon_system->remote_clients, remote_client->id);

  session_id = grd_dbus_gdm_remote_display_get_session_id (remote_display);
  register_handover_iface (new_remote_client, session_id);

  grd_dbus_remote_desktop_rdp_handover_emit_restart_handover (
    new_remote_client->handover_dst->interface);
}

static void
register_handover_for_display (GrdDaemonSystem         *daemon_system,
                               GrdDBusGdmRemoteDisplay *remote_display)
{
  GrdRemoteClient *remote_client;
  const char *session_id;
  const char *remote_id;

  remote_id = grd_dbus_gdm_remote_display_get_remote_id (remote_display);
  if (!g_hash_table_lookup_extended (daemon_system->remote_clients,
                                     remote_id, NULL,
                                     (gpointer *) &remote_client))
    {
      g_debug ("[DaemonSystem] GDM added a new remote display with a remote "
               "id %s we didn't know about", remote_id);
      return;
    }

  disconnect_from_remote_display (remote_client);
  remote_client->remote_display = g_object_ref (remote_display);

  g_signal_connect (remote_display, "notify::remote-id",
                    G_CALLBACK (on_remote_display_remote_id_changed),
                    remote_client);

  session_id = grd_dbus_gdm_remote_display_get_session_id (remote_display);
  if (!session_id || strcmp (session_id, "") == 0)
    {
      g_signal_connect (remote_display, "notify::session-id",
                        G_CALLBACK (on_gdm_remote_display_session_id_changed),
                        remote_client);
      return;
    }

  g_debug ("[DaemonSystem] GDM added a new remote display with remote id: %s "
           "and session: %s",
           remote_client->id,
           session_id);

  register_handover_iface (remote_client, session_id);
}

static void
unregister_handover_for_display (GrdDaemonSystem         *daemon_system,
                                 GrdDBusGdmRemoteDisplay *remote_display)
{
  g_autofree const char *object_path = NULL;
  GrdRemoteClient *remote_client;
  const char *session_id;
  const char *remote_id;

  remote_id = grd_dbus_gdm_remote_display_get_remote_id (remote_display);
  if (!g_hash_table_lookup_extended (daemon_system->remote_clients,
                                     remote_id, NULL,
                                     (gpointer *) &remote_client))
    {
      g_debug ("[DaemonSystem] GDM removed a remote display with remote id "
               "%s we didn't know about", remote_id);
      return;
    }

  session_id = grd_dbus_gdm_remote_display_get_session_id (remote_display);
  object_path = g_strdup_printf ("%s/session%s",
                                 REMOTE_DESKTOP_HANDOVERS_OBJECT_PATH,
                                 session_id);

  g_debug ("[DaemonSystem] GDM removed a remote display with remote id: %s "
           "and session: %s", remote_client->id, session_id);

  if (remote_client->handover_src)
    {
      if (strcmp (object_path, remote_client->handover_src->object_path) == 0)
        {
          unregister_handover_iface (remote_client, remote_client->handover_src);
          g_clear_pointer (&remote_client->handover_src, handover_iface_free);
        }
    }

  if (remote_client->handover_dst)
    {
      if (strcmp (object_path, remote_client->handover_dst->object_path) == 0)
        g_hash_table_remove (daemon_system->remote_clients, remote_id);
    }
}

static void
on_gdm_object_remote_display_interface_changed (GrdDaemonSystem  *daemon_system,
                                                GParamSpec       *param_spec,
                                                GrdDBusGdmObject *object)
{
  GrdDBusGdmRemoteDisplay *remote_display;

  remote_display = grd_dbus_gdm_object_peek_remote_display (object);
  if (remote_display)
    register_handover_for_display (daemon_system, remote_display);
}

static void
on_gdm_object_added (GrdDaemonSystem  *daemon_system,
                     GrdDBusGdmObject *object)
{
  GrdDBusGdmRemoteDisplay *remote_display;

  remote_display = grd_dbus_gdm_object_peek_remote_display (object);
  if (remote_display)
    {
      register_handover_for_display (daemon_system, remote_display);
      return;
    }

  g_signal_connect_object (
    G_OBJECT (object),
    "notify::remote-display",
    G_CALLBACK (on_gdm_object_remote_display_interface_changed),
    daemon_system,
    G_CONNECT_SWAPPED);
}

static void
on_gdm_object_removed (GrdDaemonSystem  *daemon_system,
                       GrdDBusGdmObject *object)
{
  GrdDBusGdmRemoteDisplay *remote_display;

  remote_display = grd_dbus_gdm_object_peek_remote_display (object);
  if (remote_display)
    unregister_handover_for_display (daemon_system, remote_display);

  g_signal_handlers_disconnect_by_func (
    G_OBJECT (object),
    G_CALLBACK (on_gdm_object_remote_display_interface_changed),
    daemon_system);
}

static void
on_gdm_display_objects_name_owner_changed (GDBusObjectManager *display_objects,
                                           GParamSpec         *pspec,
                                           GrdDaemonSystem    *daemon_system)
{
  g_autofree const char *name_owner = NULL;

  g_object_get (G_OBJECT (display_objects), "name-owner", &name_owner, NULL);
  if (!name_owner)
    {
      grd_daemon_disable_services (GRD_DAEMON (daemon_system));
      return;
    }

  grd_daemon_maybe_enable_services (GRD_DAEMON (daemon_system));
}

static void
on_gdm_object_manager_client_acquired (GObject      *source_object,
                                       GAsyncResult *result,
                                       gpointer      user_data)
{
  GrdDaemonSystem *daemon_system = user_data;
  g_autoptr (GError) error = NULL;

  daemon_system->display_objects =
    grd_dbus_gdm_object_manager_client_new_for_bus_finish (result, &error);
  if (!daemon_system->display_objects)
    {
      g_warning ("[DaemonSystem] Error connecting to display manager: %s",
                 error->message);
      return;
    }

  g_signal_connect_object (daemon_system->display_objects,
                           "object-added",
                           G_CALLBACK (on_gdm_object_added),
                           daemon_system,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (daemon_system->display_objects,
                           "object-removed",
                           G_CALLBACK (on_gdm_object_removed),
                           daemon_system,
                           G_CONNECT_SWAPPED);

  g_signal_connect (G_OBJECT (daemon_system->display_objects),
                    "notify::name-owner",
                    G_CALLBACK (on_gdm_display_objects_name_owner_changed),
                    daemon_system);

  grd_daemon_maybe_enable_services (GRD_DAEMON (daemon_system));
}

GrdDaemonSystem *
grd_daemon_system_new (GError **error)
{
  GrdContext *context;

  context = grd_context_new (GRD_RUNTIME_MODE_SYSTEM, error);
  if (!context)
    return NULL;

  return g_object_new (GRD_TYPE_DAEMON_SYSTEM,
                       "application-id", GRD_DAEMON_SYSTEM_APPLICATION_ID,
                       "flags", G_APPLICATION_IS_SERVICE,
                       "context", context,
                       NULL);
}

static void
grd_daemon_system_init (GrdDaemonSystem *daemon_system)
{
  daemon_system->remote_clients =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           NULL, (GDestroyNotify) grd_remote_client_free);
}

static void
grd_daemon_system_startup (GApplication *app)
{
  GrdDaemonSystem *daemon_system = GRD_DAEMON_SYSTEM (app);
  GCancellable *cancellable =
    grd_daemon_get_cancellable (GRD_DAEMON (daemon_system));
  g_autoptr (GError) error = NULL;

  daemon_system->dispatcher_skeleton =
    grd_dbus_remote_desktop_rdp_dispatcher_skeleton_new ();
  g_signal_connect (daemon_system->dispatcher_skeleton,
                    "handle-request-handover",
                    G_CALLBACK (on_handle_request_handover),
                    daemon_system);

  daemon_system->handover_manager_server =
    g_dbus_object_manager_server_new (REMOTE_DESKTOP_HANDOVERS_OBJECT_PATH);

  daemon_system->system_grd_name_id =
    g_bus_own_name (G_BUS_TYPE_SYSTEM,
                    REMOTE_DESKTOP_BUS_NAME,
                    G_BUS_NAME_OWNER_FLAGS_NONE,
                    on_system_grd_bus_acquired,
                    on_system_grd_name_acquired,
                    on_system_grd_name_lost,
                    daemon_system, NULL);

  grd_dbus_gdm_remote_display_factory_proxy_new_for_bus (
    G_BUS_TYPE_SYSTEM,
    G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
    GDM_BUS_NAME,
    GDM_REMOTE_DISPLAY_FACTORY_OBJECT_PATH,
    cancellable,
    on_remote_display_factory_proxy_acquired,
    daemon_system);

  grd_dbus_gdm_object_manager_client_new_for_bus (
    G_BUS_TYPE_SYSTEM,
    G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
    GDM_BUS_NAME,
    GDM_OBJECT_MANAGER_OBJECT_PATH,
    cancellable,
    on_gdm_object_manager_client_acquired,
    daemon_system);

  g_signal_connect (daemon_system, "rdp-server-started",
                    G_CALLBACK (on_rdp_server_started), NULL);

  g_signal_connect (daemon_system, "rdp-server-stopped",
                    G_CALLBACK (on_rdp_server_stopped), NULL);

  G_APPLICATION_CLASS (grd_daemon_system_parent_class)->startup (app);
}

static void
grd_daemon_system_shutdown (GApplication *app)
{
  GrdDaemonSystem *daemon_system = GRD_DAEMON_SYSTEM (app);

  g_clear_pointer (&daemon_system->remote_clients, g_hash_table_unref);

  g_clear_object (&daemon_system->display_objects);
  g_clear_object (&daemon_system->remote_display_factory_proxy);

  g_dbus_interface_skeleton_unexport (
    G_DBUS_INTERFACE_SKELETON (daemon_system->dispatcher_skeleton));
  g_clear_object (&daemon_system->dispatcher_skeleton);

  g_clear_object (&daemon_system->handover_manager_server);
  g_clear_handle_id (&daemon_system->system_grd_name_id,
                     g_bus_unown_name);

  G_APPLICATION_CLASS (grd_daemon_system_parent_class)->shutdown (app);
}

static void
grd_daemon_system_class_init (GrdDaemonSystemClass *klass)
{
  GApplicationClass *g_application_class = G_APPLICATION_CLASS (klass);
  GrdDaemonClass *daemon_class = GRD_DAEMON_CLASS (klass);

  g_application_class->startup = grd_daemon_system_startup;
  g_application_class->shutdown = grd_daemon_system_shutdown;

  daemon_class->is_daemon_ready = grd_daemon_system_is_ready;
}
