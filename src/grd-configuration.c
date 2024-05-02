/*
 * Copyright (C) 2024 SUSE Software Solutions Germany GmbH
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

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>

#include "grd-dbus-remote-desktop.h"
#include "grd-private.h"
#include "grd-settings-system.h"

#define GRD_CONFIGURATION_TIMEOUT_S 10

struct _GrdConfiguration
{
  GApplication parent;

  GrdSettingsSystem *settings;

  GrdDBusRemoteDesktopConfigurationRdpServer *configuration_rdp_server;
  unsigned int own_name_source_id;

  unsigned int timeout_source_id;
  unsigned int sigint_source_id;
  unsigned int sigterm_source_id;
};

#define GRD_TYPE_CONFIGURATION (grd_configuration_get_type ())
G_DECLARE_FINAL_TYPE (GrdConfiguration,
                      grd_configuration,
                      GRD, CONFIGURATION,
                      GApplication)

G_DEFINE_TYPE (GrdConfiguration, grd_configuration, G_TYPE_APPLICATION)

static void
grd_configuration_init (GrdConfiguration *app)
{
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  GrdConfiguration *configuration = user_data;

  g_debug ("[Configuration] Now on system bus");

  g_dbus_interface_skeleton_export (
    G_DBUS_INTERFACE_SKELETON (configuration->configuration_rdp_server),
    connection,
    REMOTE_DESKTOP_CONFIGURATION_OBJECT_PATH,
    NULL);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  g_debug ("[Configuration] Owned %s name", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  g_debug ("[Configuration] Lost owned %s name", name);
}

static gboolean
terminate (gpointer user_data)
{
  GrdConfiguration *configuration = user_data;

  g_clear_handle_id (&configuration->timeout_source_id, g_source_remove);
  g_clear_handle_id (&configuration->sigint_source_id, g_source_remove);
  g_clear_handle_id (&configuration->sigterm_source_id, g_source_remove);

  g_application_release (G_APPLICATION (configuration));

  return G_SOURCE_REMOVE;
}

static void
register_signals (GrdConfiguration *configuration)
{
  configuration->timeout_source_id =
    g_timeout_add_seconds (GRD_CONFIGURATION_TIMEOUT_S,
                           terminate,
                           configuration);
  configuration->sigint_source_id = g_unix_signal_add (SIGINT,
                                                       terminate,
                                                       configuration);
  configuration->sigterm_source_id = g_unix_signal_add (SIGTERM,
                                                        terminate,
                                                        configuration);
}

static void
grd_configuration_startup (GApplication *application)
{
  GrdConfiguration *configuration = GRD_CONFIGURATION (application);

  configuration->settings = grd_settings_system_new ();
  grd_settings_system_use_local_state (configuration->settings);

  configuration->configuration_rdp_server =
    grd_dbus_remote_desktop_configuration_rdp_server_skeleton_new ();

  configuration->own_name_source_id =
    g_bus_own_name (G_BUS_TYPE_SYSTEM,
                    REMOTE_DESKTOP_CONFIGURATION_BUS_NAME,
                    G_BUS_NAME_OWNER_FLAGS_NONE,
                    on_bus_acquired,
                    on_name_acquired,
                    on_name_lost,
                    configuration, NULL);

  register_signals (configuration);

  g_application_hold (application);

  G_APPLICATION_CLASS (grd_configuration_parent_class)->startup (application);
}

static void
grd_configuration_shutdown (GApplication *application)
{
  GrdConfiguration *configuration = GRD_CONFIGURATION (application);

  g_clear_object (&configuration->settings);

  g_dbus_interface_skeleton_unexport (
    G_DBUS_INTERFACE_SKELETON (configuration->configuration_rdp_server));
  g_clear_object (&configuration->configuration_rdp_server);

  g_clear_handle_id (&configuration->own_name_source_id, g_bus_unown_name);

  g_clear_handle_id (&configuration->timeout_source_id, g_source_remove);
  g_clear_handle_id (&configuration->sigint_source_id, g_source_remove);
  g_clear_handle_id (&configuration->sigterm_source_id, g_source_remove);

  G_APPLICATION_CLASS (grd_configuration_parent_class)->shutdown (application);
}

static void
grd_configuration_class_init (GrdConfigurationClass *klass)
{
  GApplicationClass *g_application_class = G_APPLICATION_CLASS (klass);

  g_application_class->startup = grd_configuration_startup;
  g_application_class->shutdown = grd_configuration_shutdown;
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (GApplication) app = NULL;

  app = g_object_new (grd_configuration_get_type (),
                      "application-id", REMOTE_DESKTOP_CONFIGURATION_BUS_NAME,
                      "flags", G_APPLICATION_IS_SERVICE,
                      NULL);

  return g_application_run (app, argc, argv);
}
