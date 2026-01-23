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
#include <glib/gstdio.h>
#include <glib.h>
#include <polkit/polkit.h>

#ifdef HAVE_RDP
#include <freerdp/freerdp.h>
#endif

#include "grd-dbus-remote-desktop.h"
#include "grd-private.h"
#include "grd-settings-system.h"
#include "grd-utils.h"

#define GRD_CONFIGURATION_TIMEOUT_S 10
#define GRD_SYSTEMD_SERVICE "gnome-remote-desktop.service"
#define GRD_SERVER_USER_CERT_SUBDIR "certificates"
#define GRD_CONFIGURE_SYSTEM_DAEMON_POLKIT_ACTION "org.gnome.remotedesktop.configure-system-daemon"
#define GRD_MAX_CERTIFICATE_FILE_SIZE_BYTES (50 * 1024)
#define GRD_MAX_PRIVATE_KEY_FILE_SIZE_BYTES (50 * 1024)

typedef struct
{
  unsigned int *timeout_source_id;
  GSourceFunc function;
  gpointer data;
} TimeoutLocker;

struct _GrdConfiguration
{
  GApplication parent;

  PolkitAuthority *authority;

  GrdSettingsSystem *settings;

  GrdDBusRemoteDesktopConfigurationRdpServer *configuration_rdp_server;
  unsigned int own_name_source_id;

  GDBusProxy *unit_proxy;
  GrdSystemdUnitActiveState unit_state;

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
timeout_locker_free (TimeoutLocker *locker);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (TimeoutLocker, timeout_locker_free)

#ifdef HAVE_RDP
G_DEFINE_AUTOPTR_CLEANUP_FUNC (rdpCertificate, freerdp_certificate_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (rdpPrivateKey, freerdp_key_free)
#endif

static TimeoutLocker *
timeout_locker_new (unsigned int *timeout_source_id,
                    GSourceFunc   function,
                    gpointer      data)
{
  TimeoutLocker *locker = g_new0 (TimeoutLocker, 1);

  locker->timeout_source_id = timeout_source_id;
  locker->function = function;
  locker->data = data;

  g_clear_handle_id (locker->timeout_source_id, g_source_remove);

  return locker;
}

static void
timeout_locker_free (TimeoutLocker *locker)
{
  if (*locker->timeout_source_id == 0)
    {
      *locker->timeout_source_id =
        g_timeout_add_seconds (GRD_CONFIGURATION_TIMEOUT_S,
                               locker->function,
                               locker->data);
    }

  g_free (locker);
}

static void
grd_configuration_init (GrdConfiguration *app)
{
}

static void
on_unit_active_state_changed (GrdConfiguration *configuration)
{
  if (!grd_systemd_unit_get_active_state (configuration->unit_proxy,
                                          &configuration->unit_state,
                                          NULL))
    return;

  g_object_notify (G_OBJECT (configuration->settings), "rdp-enabled");
}

static void
watch_grd_system_unit_active_state (GrdConfiguration *configuration)
{
  GDBusProxy *unit_proxy = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_systemd_get_unit (G_BUS_TYPE_SYSTEM,
                             GRD_SYSTEMD_SERVICE,
                             &unit_proxy,
                             &error))
    {
      g_warning ("Could not load %s: %s",
                 GRD_SYSTEMD_SERVICE, error->message);
      return;
    }

  grd_systemd_unit_get_active_state (unit_proxy,
                                     &configuration->unit_state,
                                     NULL);

  g_signal_connect_object (G_OBJECT (unit_proxy),
                           "g-properties-changed",
                           G_CALLBACK (on_unit_active_state_changed),
                           configuration,
                           G_CONNECT_SWAPPED);

  configuration->unit_proxy = unit_proxy;
}

static gboolean
transform_enabled (GBinding     *binding,
                   const GValue *from_value,
                   GValue       *to_value,
                   gpointer      user_data)
{
  GrdConfiguration *configuration = user_data;
  gboolean enabled;
  GrdSystemdUnitActiveState active_state;

  enabled = g_value_get_boolean (from_value);
  active_state = configuration->unit_state;

  g_value_set_boolean (to_value,
                       enabled &&
                       (active_state == GRD_SYSTEMD_UNIT_ACTIVE_STATE_ACTIVE ||
                        active_state == GRD_SYSTEMD_UNIT_ACTIVE_STATE_ACTIVATING));

  return TRUE;
}

static gboolean
on_handle_enable (GrdDBusRemoteDesktopConfigurationRdpServer *configuration_rdp_server,
                  GDBusMethodInvocation                      *invocation,
                  GrdConfiguration                           *configuration)
{
  g_autoptr (GError) error = NULL;

  if (!grd_toggle_systemd_unit (GRD_RUNTIME_MODE_SYSTEM, TRUE, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed enabling %s: %s",
                                             GRD_SYSTEMD_SERVICE,
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set (G_OBJECT (configuration->settings), "rdp-enabled", TRUE, NULL);

  grd_dbus_remote_desktop_configuration_rdp_server_complete_enable (
    configuration_rdp_server,
    invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
on_handle_disable (GrdDBusRemoteDesktopConfigurationRdpServer *configuration_rdp_server,
                   GDBusMethodInvocation                      *invocation,
                   GrdConfiguration                           *configuration)
{
  g_autoptr (GError) error = NULL;

  if (!grd_toggle_systemd_unit (GRD_RUNTIME_MODE_SYSTEM, FALSE, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed disabling %s: %s",
                                             GRD_SYSTEMD_SERVICE,
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set (G_OBJECT (configuration->settings), "rdp-enabled", FALSE, NULL);

  grd_dbus_remote_desktop_configuration_rdp_server_complete_disable (
    configuration_rdp_server,
    invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
on_handle_get_credentials (GrdDBusRemoteDesktopConfigurationRdpServer *configuration_rdp_server,
                           GDBusMethodInvocation                      *invocation,
                           GrdConfiguration                           *configuration)
{
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;
  g_autoptr (GError) error = NULL;
  GVariantBuilder credentials;

  g_variant_builder_init (&credentials, G_VARIANT_TYPE ("a{sv}"));

  grd_settings_get_rdp_credentials (GRD_SETTINGS (configuration->settings),
                                    &username, &password,
                                    &error);
  if (error)
    g_warning ("[Configuration] Failed to get credentials: %s", error->message);

  if (!username)
    username = g_strdup ("");

  if (!password)
    password = g_strdup ("");

  g_variant_builder_add (&credentials, "{sv}", "username",
                         g_variant_new_string (username));
  g_variant_builder_add (&credentials, "{sv}", "password",
                         g_variant_new_string (password));

  grd_dbus_remote_desktop_configuration_rdp_server_complete_get_credentials (
    configuration_rdp_server,
    invocation,
    g_variant_builder_end (&credentials));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
on_handle_set_credentials (GrdDBusRemoteDesktopConfigurationRdpServer *configuration_rdp_server,
                           GDBusMethodInvocation                      *invocation,
                           GVariant                                   *credentials,
                           GrdConfiguration                           *configuration)
{
  g_autofree char *old_username = NULL;
  g_autofree char *old_password = NULL;
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;
  g_autoptr (GError) error = NULL;

  g_variant_lookup (credentials, "username", "s", &username);
  g_variant_lookup (credentials, "password", "s", &password);

  if (!username && !password)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Username or password expected "
                                             "in credentials");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!username || !password)
    {
      grd_settings_get_rdp_credentials (GRD_SETTINGS (configuration->settings),
                                        &old_username, &old_password,
                                        NULL);
    }

  if (!username)
    username = g_steal_pointer (&old_username);

  if (!password)
    password = g_steal_pointer (&old_password);

  if (!grd_settings_set_rdp_credentials (GRD_SETTINGS (configuration->settings),
                                         username, password,
                                         &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed to set credentials: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_remote_desktop_configuration_rdp_server_complete_set_credentials (
    configuration_rdp_server,
    invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
on_handle_import_certificate (GrdDBusRemoteDesktopConfigurationRdpServer *configuration_rdp_server,
                              GDBusMethodInvocation                      *invocation,
                              GUnixFDList                                *fd_list,
                              GVariant                                   *certificate,
                              GVariant                                   *private_key,
                              GrdConfiguration                           *configuration)
{
  g_autoptr (rdpCertificate) rdp_certificate = NULL;
  g_autoptr (rdpPrivateKey) rdp_private_key = NULL;
  g_autofree char *certificate_filename = NULL;
  g_autofree char *key_filename = NULL;
  g_autoptr (GError) error = NULL;
  g_autofd int certificate_fd = -1;
  g_autofd int key_fd = -1;
  GFileTest fd_test_results;
  int certificate_fd_index = -1;
  int key_fd_index = -1;
  gboolean success;

  g_variant_get (certificate, "(sh)", &certificate_filename,
                 &certificate_fd_index);
  g_variant_get (private_key, "(sh)", &key_filename,
                 &key_fd_index);

  if (!G_IS_UNIX_FD_LIST (fd_list))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to acquire "
                   "file descriptor for certificate and private key: "
                   "The sender supplied an invalid fd list");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (certificate_fd_index < 0 ||
      certificate_fd_index >= g_unix_fd_list_get_length (fd_list))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to acquire "
                   "file descriptor for certificate: "
                   "The sender supplied an invalid fd index");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  if (key_fd_index < 0 ||
      key_fd_index >= g_unix_fd_list_get_length (fd_list))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to acquire "
                   "file descriptor for private key: "
                   "The sender supplied an invalid fd index");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  certificate_fd = g_unix_fd_list_get (fd_list, certificate_fd_index, &error);
  if (certificate_fd == -1)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  success = grd_test_fd (certificate_fd, GRD_MAX_CERTIFICATE_FILE_SIZE_BYTES,
                         &fd_test_results, &error);
  if (!success)
    {
      g_prefix_error (&error, "Could not inspect certificate file descriptor");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!(fd_test_results & G_FILE_TEST_IS_REGULAR))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_REGULAR_FILE,
                                             "Invalid certificate file");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  key_fd = g_unix_fd_list_get (fd_list, key_fd_index, &error);
  if (key_fd == -1)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  success = grd_test_fd (key_fd, GRD_MAX_PRIVATE_KEY_FILE_SIZE_BYTES,
                         &fd_test_results, &error);
  if (!success)
    {
      g_prefix_error (&error, "Could not inspect private key file descriptor");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!(fd_test_results & G_FILE_TEST_IS_REGULAR))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_REGULAR_FILE,
                                             "Invalid private key file");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_rewrite_path_to_user_data_dir (&certificate_filename,
                                     GRD_SERVER_USER_CERT_SUBDIR,
                                     "rdp-tls.crt");
  if (!grd_write_fd_to_file (certificate_fd, certificate_filename,
                             NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (certificate_filename)
    rdp_certificate = freerdp_certificate_new_from_file (certificate_filename);

  if (!rdp_certificate)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_INVALID_DATA,
                                             "Invalid certificate");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_rewrite_path_to_user_data_dir (&key_filename,
                                     GRD_SERVER_USER_CERT_SUBDIR,
                                     "rdp-tls.key");
  if (!grd_write_fd_to_file (key_fd, key_filename, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (key_filename)
    rdp_private_key = freerdp_key_new_from_file (key_filename);

  if (!rdp_private_key)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_INVALID_DATA,
                                             "Invalid private key");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set (configuration->settings,
                "rdp-server-cert-path", certificate_filename,
                "rdp-server-key-path", key_filename,
                NULL);

  grd_dbus_remote_desktop_configuration_rdp_server_complete_import_certificate (
    configuration_rdp_server,
    invocation,
    fd_list);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
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

static gboolean
ensure_polkit_authority (GrdConfiguration  *configuration,
                         GError           **error)
{
  if (configuration->authority)
    return TRUE;

  configuration->authority = polkit_authority_get_sync (NULL, error);

  return configuration->authority != NULL;
}

static gboolean
on_authorize_method (GrdDBusRemoteDesktopConfigurationRdpServer *configuration_rdp_server,
                     GDBusMethodInvocation                      *invocation,
                     GrdConfiguration                           *configuration)
{
  g_autoptr (PolkitAuthorizationResult) result = NULL;
  g_autoptr (PolkitSubject) subject = NULL;
  g_autoptr (TimeoutLocker) locker = NULL;
  PolkitCheckAuthorizationFlags flags;
  g_autoptr (GError) error = NULL;
  const char *sender = NULL;
  const char *action = NULL;

  locker = timeout_locker_new (&configuration->timeout_source_id, terminate, configuration);

  if (!ensure_polkit_authority (configuration, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Couldn't get polkit authority: %s",
                                             error->message);
      return FALSE;
    }

  sender = g_dbus_method_invocation_get_sender (invocation);
  subject = polkit_system_bus_name_new (sender);
  action = GRD_CONFIGURE_SYSTEM_DAEMON_POLKIT_ACTION;
  flags = POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION;
  result = polkit_authority_check_authorization_sync (configuration->authority,
                                                      subject, action,
                                                      NULL, flags, NULL,
                                                      &error);
  if (!result)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed to check authorization: %s",
                                             error->message);
      return FALSE;
    }

  if (!polkit_authorization_result_get_is_authorized (result))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Not authorized for action %s",
                                             action);
      return FALSE;
    }

  return TRUE;
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

  watch_grd_system_unit_active_state (configuration);

  g_object_bind_property_full (configuration->settings, "rdp-enabled",
                               configuration->configuration_rdp_server, "enabled",
                               G_BINDING_SYNC_CREATE,
                               transform_enabled,
                               NULL,
                               configuration,
                               NULL);
  g_object_bind_property (configuration->settings, "rdp-port",
                          configuration->configuration_rdp_server, "port",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (configuration->settings, "rdp-server-cert-path",
                          configuration->configuration_rdp_server, "tls-cert",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (configuration->settings, "rdp-server-fingerprint",
                          configuration->configuration_rdp_server, "tls-fingerprint",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (configuration->settings, "rdp-server-key-path",
                          configuration->configuration_rdp_server, "tls-key",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (configuration->settings, "rdp-auth-methods",
                          configuration->configuration_rdp_server, "auth-methods",
                          G_BINDING_SYNC_CREATE);
  g_signal_connect_object (configuration->configuration_rdp_server, "handle-enable",
                           G_CALLBACK (on_handle_enable),
                           configuration, 0);
  g_signal_connect_object (configuration->configuration_rdp_server, "handle-disable",
                           G_CALLBACK (on_handle_disable),
                           configuration, 0);
  g_signal_connect_object (configuration->configuration_rdp_server, "handle-get-credentials",
                           G_CALLBACK (on_handle_get_credentials),
                           configuration, 0);
  g_signal_connect_object (configuration->configuration_rdp_server, "handle-set-credentials",
                           G_CALLBACK (on_handle_set_credentials),
                           configuration, 0);
  g_signal_connect_object (configuration->configuration_rdp_server, "handle-import-certificate",
                           G_CALLBACK (on_handle_import_certificate),
                           configuration, 0);
  g_signal_connect_object (configuration->configuration_rdp_server, "g-authorize-method",
                           G_CALLBACK (on_authorize_method),
                           configuration, 0);

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

  g_clear_object (&configuration->authority);

  g_clear_object (&configuration->settings);
  g_clear_object (&configuration->unit_proxy);

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
