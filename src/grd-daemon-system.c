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
#include "grd-private.h"
#include "grd-rdp-server.h"
#include "grd-session-rdp.h"

typedef struct
{
  GrdDaemonSystem *daemon_system;

  char *id;

  GrdSession *session;
} GrdRemoteClient;

struct _GrdDaemonSystem
{
  GrdDaemon parent;

  GrdDBusGdmRemoteDisplayFactory *remote_display_factory_proxy;

  GHashTable *remote_clients;
};

G_DEFINE_TYPE (GrdDaemonSystem, grd_daemon_system, GRD_TYPE_DAEMON)

static gboolean
grd_daemon_system_is_ready (GrdDaemon *daemon)
{
  GrdDaemonSystem *daemon_system = GRD_DAEMON_SYSTEM (daemon);
  g_autofree const char *gdm_remote_display_factory_name_owner = NULL;
  GDBusProxy *remote_display_factory_proxy =
    G_DBUS_PROXY (daemon_system->remote_display_factory_proxy);

  if (!daemon_system->remote_display_factory_proxy)
    return FALSE;

  gdm_remote_display_factory_name_owner = g_dbus_proxy_get_name_owner (
                                            remote_display_factory_proxy);

 if (!gdm_remote_display_factory_name_owner)
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
grd_remote_client_free (GrdRemoteClient *remote_client)
{
  g_clear_pointer (&remote_client->id, g_free);

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
  remote_client->session = session;
  g_object_weak_ref (G_OBJECT (session),
                     (GWeakNotify) session_disposed,
                     remote_client);

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

  grd_dbus_gdm_remote_display_factory_proxy_new_for_bus (
    G_BUS_TYPE_SYSTEM,
    G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
    GDM_BUS_NAME,
    GDM_REMOTE_DISPLAY_FACTORY_OBJECT_PATH,
    cancellable,
    on_remote_display_factory_proxy_acquired,
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

  g_clear_object (&daemon_system->remote_display_factory_proxy);

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
