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

#include "grd-context.h"
#include "grd-daemon.h"
#include "grd-dbus-gdm.h"
#include "grd-dbus-remote-desktop.h"
#include "grd-private.h"
#include "grd-rdp-server.h"
#include "grd-session-rdp.h"

typedef struct
{
  GrdDaemonSystem *daemon_system;

  char *id;

  GrdSession *session;

  GDBusObjectSkeleton *handover_skeleton;
  GrdDBusRemoteDesktopRdpHandover *handover_interface;
  char *dbus_object_path;

  gboolean is_registered;
} GrdRemoteClient;

struct _GrdDaemonSystem
{
  GrdDaemon parent;

  GrdDBusGdmRemoteDisplayFactory *remote_display_factory_proxy;
  GDBusObjectManager *display_objects;

  unsigned int system_grd_name_id;
  GDBusObjectManagerServer *handover_manager_server;

  GHashTable *remote_clients;
};

G_DEFINE_TYPE (GrdDaemonSystem, grd_daemon_system, GRD_TYPE_DAEMON)

static gboolean
grd_daemon_system_is_ready (GrdDaemon *daemon)
{
  GrdDaemonSystem *daemon_system = GRD_DAEMON_SYSTEM (daemon);
  GDBusProxy *remote_display_factory_proxy =
    G_DBUS_PROXY (daemon_system->remote_display_factory_proxy);
  GDBusObjectManagerClient *display_objects_manager =
    G_DBUS_OBJECT_MANAGER_CLIENT (daemon_system->display_objects);
  g_autofree const char *gdm_remote_display_factory_name_owner = NULL;
  g_autofree const char *gdm_display_objects_name_owner = NULL;
  g_autoptr (GDBusConnection) manager_connection = NULL;

  if (!daemon_system->remote_display_factory_proxy ||
      !daemon_system->display_objects ||
      !daemon_system->handover_manager_server)
    return FALSE;

  gdm_remote_display_factory_name_owner = g_dbus_proxy_get_name_owner (
                                            remote_display_factory_proxy);
  gdm_display_objects_name_owner = g_dbus_object_manager_client_get_name_owner (
                                     display_objects_manager);
  manager_connection = g_dbus_object_manager_server_get_connection (
                         daemon_system->handover_manager_server);

  if (!gdm_remote_display_factory_name_owner ||
      !gdm_display_objects_name_owner ||
      !manager_connection)
    return FALSE;

  return TRUE;
}

static char *
get_id_from_routing_token (uint32_t routing_token)
{
  return g_strdup_printf (REMOTE_DESKTOP_CLIENT_OBJECT_PATH "/%u",
                          routing_token);
}

static void
register_handover_iface (GrdRemoteClient *remote_client,
                         const char      *session_id)
{
  GrdDaemonSystem *daemon_system = remote_client->daemon_system;

  if (remote_client->is_registered)
    return;

  remote_client->dbus_object_path =
    g_strdup_printf ("%s/session%s",
                     REMOTE_DESKTOP_HANDOVERS_OBJECT_PATH,
                     session_id);

  remote_client->handover_skeleton =
    g_dbus_object_skeleton_new (remote_client->dbus_object_path);

  g_dbus_object_manager_server_export (daemon_system->handover_manager_server,
                                       remote_client->handover_skeleton);

  g_dbus_object_skeleton_add_interface (
    remote_client->handover_skeleton,
    G_DBUS_INTERFACE_SKELETON (remote_client->handover_interface));

  remote_client->is_registered = TRUE;

  g_debug ("[DaemonSystem] Registered handover on path %s",
           remote_client->dbus_object_path);
}

static void
unregister_handover_iface (GrdRemoteClient *remote_client)
{
  GrdDaemonSystem *daemon_system = remote_client->daemon_system;

  if (!remote_client->is_registered)
    return;

  g_dbus_object_skeleton_remove_interface (
    remote_client->handover_skeleton,
    G_DBUS_INTERFACE_SKELETON (remote_client->handover_interface));

  g_dbus_object_manager_server_unexport (
    daemon_system->handover_manager_server,
    remote_client->dbus_object_path);

  g_clear_object (&remote_client->handover_skeleton);
  g_clear_pointer (&remote_client->dbus_object_path, g_free);

  remote_client->is_registered = FALSE;
}

static void
grd_remote_client_free (GrdRemoteClient *remote_client)
{
  unregister_handover_iface (remote_client);

  g_clear_pointer (&remote_client->id, g_free);
  g_clear_object (&remote_client->handover_skeleton);
  g_clear_object (&remote_client->handover_interface);
  g_clear_pointer (&remote_client->dbus_object_path, g_free);

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
  remote_client->is_registered = FALSE;
  remote_client->session = session;
  g_object_weak_ref (G_OBJECT (session),
                     (GWeakNotify) session_disposed,
                     remote_client);

  remote_client->handover_interface =
    grd_dbus_remote_desktop_rdp_handover_skeleton_new ();

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
      if (remote_client->session)
        {
          grd_session_rdp_notify_error (
            GRD_SESSION_RDP (remote_client->session),
            GRD_SESSION_RDP_ERROR_SERVER_REDIRECTION);
        }
      g_hash_table_remove (remote_client->daemon_system->remote_clients,
                           remote_client->id);
    }
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
on_rdp_server_started (GrdDaemonSystem *daemon_system)
{
  GrdRdpServer *rdp_server =
    grd_daemon_get_rdp_server (GRD_DAEMON (daemon_system));

  g_signal_connect (rdp_server, "incoming-new-connection",
                    G_CALLBACK (on_incoming_new_connection),
                    daemon_system);
}

static void
on_rdp_server_stopped (GrdDaemonSystem *daemon_system)
{
  GrdRdpServer *rdp_server =
    grd_daemon_get_rdp_server (GRD_DAEMON (daemon_system));

  g_signal_handlers_disconnect_by_func (rdp_server,
                                        G_CALLBACK (on_incoming_new_connection),
                                        daemon_system);
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

  session_id = grd_dbus_gdm_remote_display_get_session_id (remote_display);
  if (!session_id || strcmp (session_id, "") == 0)
    {
      g_signal_connect (G_OBJECT (remote_display),
                        "notify::session-id",
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
  GrdRemoteClient *remote_client;
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

  unregister_handover_iface (remote_client);
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
