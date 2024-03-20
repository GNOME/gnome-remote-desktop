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

#include "grd-daemon.h"

#ifdef HAVE_RDP
#include <freerdp/freerdp.h>
#endif

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>

#include "grd-context.h"
#include "grd-daemon-user.h"
#include "grd-dbus-mutter-remote-desktop.h"
#include "grd-dbus-remote-desktop.h"
#include "grd-private.h"
#include "grd-rdp-server.h"
#include "grd-settings-system.h"
#include "grd-settings-user.h"
#include "grd-utils.h"
#include "grd-vnc-server.h"

#ifdef HAVE_LIBSYSTEMD
#include "grd-daemon-handover.h"
#include "grd-daemon-system.h"
#endif /* HAVE_LIBSYSTEMD */

#define RDP_SERVER_RESTART_DELAY_MS 3000
#define RDP_SERVER_USER_CERT_SUBDIR "certificates"
#define RDP_MAX_CERTIFICATE_FILE_SIZE_BYTES (50 * 1024)
#define RDP_MAX_PRIVATE_KEY_FILE_SIZE_BYTES (50 * 1024)

enum
{
  PROP_0,

  PROP_CONTEXT,
};

enum
{
  MUTTER_PROXY_ACQUIRED,
  RDP_SERVER_STARTED,
  RDP_SERVER_STOPPED,

  N_SIGNALS,
};

static guint signals[N_SIGNALS];

typedef struct _GrdDaemonPrivate
{
  GSource *sigint_source;
  GSource *sigterm_source;

  GCancellable *cancellable;
  guint mutter_remote_desktop_watch_name_id;
  guint mutter_screen_cast_watch_name_id;

  GrdContext *context;

  GDBusConnection *connection;
  GrdDBusRemoteDesktopOrgGnomeRemoteDesktop *remote_desktop_interface;

#ifdef HAVE_RDP
  GrdRdpServer *rdp_server;
  unsigned int restart_rdp_server_source_id;
#endif
#ifdef HAVE_VNC
  GrdVncServer *vnc_server;
#endif
} GrdDaemonPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdDaemon, grd_daemon, G_TYPE_APPLICATION)

#ifdef HAVE_RDP
static void maybe_start_rdp_server (GrdDaemon *daemon);
#endif

#ifdef HAVE_RDP
G_DEFINE_AUTOPTR_CLEANUP_FUNC (rdpCertificate, freerdp_certificate_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (rdpPrivateKey, freerdp_key_free)
#endif

GCancellable *
grd_daemon_get_cancellable (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  return priv->cancellable;
}

GrdContext *
grd_daemon_get_context (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  return priv->context;
}

#ifdef HAVE_RDP
GrdRdpServer *
grd_daemon_get_rdp_server (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  return priv->rdp_server;
}
#endif /* HAVE_RDP */

static void
export_remote_desktop_interface (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdRuntimeMode runtime_mode = grd_context_get_runtime_mode (priv->context);

  priv->remote_desktop_interface =
    grd_dbus_remote_desktop_org_gnome_remote_desktop_skeleton_new ();

  switch (runtime_mode)
  {
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
      grd_dbus_remote_desktop_org_gnome_remote_desktop_set_runtime_mode (
        priv->remote_desktop_interface, "screen-share");
      break;
    case GRD_RUNTIME_MODE_HEADLESS:
      grd_dbus_remote_desktop_org_gnome_remote_desktop_set_runtime_mode (
        priv->remote_desktop_interface, "headless");
      break;
    case GRD_RUNTIME_MODE_SYSTEM:
      grd_dbus_remote_desktop_org_gnome_remote_desktop_set_runtime_mode (
        priv->remote_desktop_interface, "system");
      break;
    case GRD_RUNTIME_MODE_HANDOVER:
      grd_dbus_remote_desktop_org_gnome_remote_desktop_set_runtime_mode (
        priv->remote_desktop_interface, "handover");
      break;
  }

  g_dbus_interface_skeleton_export (
    G_DBUS_INTERFACE_SKELETON (priv->remote_desktop_interface),
    priv->connection,
    REMOTE_DESKTOP_OBJECT_PATH,
    NULL);
}

static void
unexport_remote_desktop_interface (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  g_dbus_interface_skeleton_unexport (
    G_DBUS_INTERFACE_SKELETON (priv->remote_desktop_interface));

  g_clear_object (&priv->remote_desktop_interface);
}

#ifdef HAVE_RDP
static gboolean
on_handle_enable_rdp (GrdDBusRemoteDesktopRdpServer *rdp_server_interface,
                      GDBusMethodInvocation         *invocation,
                      GrdDaemon                     *daemon)
{
  GrdContext *context = grd_daemon_get_context (daemon);
  GrdSettings *settings = grd_context_get_settings (context);

  g_object_set (G_OBJECT (settings), "rdp-enabled", TRUE, NULL);

  grd_dbus_remote_desktop_rdp_server_complete_enable (rdp_server_interface,
                                                      invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
on_handle_disable_rdp (GrdDBusRemoteDesktopRdpServer *rdp_server_interface,
                       GDBusMethodInvocation         *invocation,
                       GrdDaemon                     *daemon)
{
  GrdContext *context = grd_daemon_get_context (daemon);
  GrdSettings *settings = grd_context_get_settings (context);

  g_object_set (G_OBJECT (settings), "rdp-enabled", FALSE, NULL);

  grd_dbus_remote_desktop_rdp_server_complete_disable (rdp_server_interface,
                                                       invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
on_handle_get_rdp_credentials (GrdDBusRemoteDesktopRdpServer *rdp_server_interface,
                               GDBusMethodInvocation         *invocation,
                               GrdDaemon                     *daemon)
{
  GrdContext *context = grd_daemon_get_context (daemon);
  GrdSettings *settings = grd_context_get_settings (context);
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;
  g_autoptr (GError) error = NULL;
  GVariantBuilder credentials;

  g_variant_builder_init (&credentials, G_VARIANT_TYPE ("a{sv}"));

  if (grd_settings_get_rdp_credentials (settings,
                                        &username, &password,
                                        &error))
    {
      g_variant_builder_add (&credentials, "{sv}", "username",
                             g_variant_new_string (username));
      g_variant_builder_add (&credentials, "{sv}", "password",
                             g_variant_new_string (password));
    }

  grd_dbus_remote_desktop_rdp_server_complete_get_credentials (rdp_server_interface,
                                                               invocation,
                                                               g_variant_builder_end (&credentials));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
on_handle_set_rdp_credentials (GrdDBusRemoteDesktopRdpServer *rdp_server_interface,
                               GDBusMethodInvocation         *invocation,
                               GVariant                      *credentials,
                               GrdDaemon                     *daemon)
{
  GrdContext *context = grd_daemon_get_context (daemon);
  GrdSettings *settings = grd_context_get_settings (context);
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
      grd_settings_get_rdp_credentials (settings, &old_username, &old_password,
                                        NULL);
    }

  if (!username)
    username = g_steal_pointer (&old_username);

  if (!password)
    password = g_steal_pointer (&old_password);

  if (!grd_settings_set_rdp_credentials (settings, username, password, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed to set credentials: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_remote_desktop_rdp_server_complete_set_credentials (rdp_server_interface,
                                                               invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
on_handle_import_certificate (GrdDBusRemoteDesktopRdpServer *rdp_server_interface,
                              GDBusMethodInvocation         *invocation,
                              GUnixFDList                   *fd_list,
                              GVariant                      *certificate,
                              GVariant                      *private_key,
                              gpointer                       user_data)
{
  GrdDaemon *daemon = GRD_DAEMON (user_data);
  GrdContext *context = grd_daemon_get_context (daemon);
  GrdSettings *settings = grd_context_get_settings (context);
  GCancellable *cancellable = grd_daemon_get_cancellable (daemon);
  g_autoptr (rdpCertificate) rdp_certificate = NULL;
  g_autoptr (rdpPrivateKey) rdp_private_key = NULL;
  g_autofree char *certificate_filename = NULL;
  g_autofree char *key_filename = NULL;
  g_autoptr (GError) error = NULL;
  g_autofd int certificate_fd = -1;
  g_autofd int key_fd = -1;
  GFileTest fd_test_results;
  int certificate_fd_index;
  int key_fd_index;
  gboolean success;

  g_variant_get (certificate, "(sh)", &certificate_filename,
                 &certificate_fd_index);
  g_variant_get (private_key, "(sh)", &key_filename,
                 &key_fd_index);

  certificate_fd = g_unix_fd_list_get (fd_list, certificate_fd_index, &error);
  if (certificate_fd == -1)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  success = grd_test_fd (certificate_fd, RDP_MAX_CERTIFICATE_FILE_SIZE_BYTES,
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

  success = grd_test_fd (key_fd, RDP_MAX_PRIVATE_KEY_FILE_SIZE_BYTES,
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
                                     RDP_SERVER_USER_CERT_SUBDIR,
                                     "rdp-tls.crt");
  if (!grd_write_fd_to_file (certificate_fd, certificate_filename,
                             cancellable, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

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
                                     RDP_SERVER_USER_CERT_SUBDIR,
                                     "rdp-tls.key");
  if (!grd_write_fd_to_file (key_fd, key_filename, cancellable, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  rdp_private_key = freerdp_key_new_from_file (key_filename);
  if (!rdp_private_key)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_INVALID_DATA,
                                             "Invalid private key");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_object_set (settings,
                "rdp-server-cert-path", certificate_filename,
                "rdp-server-key-path", key_filename,
                NULL);

  grd_dbus_remote_desktop_rdp_server_complete_import_certificate (rdp_server_interface,
                                                                  invocation,
                                                                  fd_list);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
export_rdp_server_interface (GrdDaemon *daemon)
{
  GrdDBusRemoteDesktopRdpServer *rdp_server_interface;
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdSettings *settings = grd_context_get_settings (priv->context);

  rdp_server_interface =
    grd_dbus_remote_desktop_rdp_server_skeleton_new ();

  grd_dbus_remote_desktop_rdp_server_set_enabled (rdp_server_interface, FALSE);
  grd_dbus_remote_desktop_rdp_server_set_port (rdp_server_interface, -1);
  g_object_bind_property (settings, "rdp-negotiate-port",
                          rdp_server_interface, "negotiate-port",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "rdp-server-cert-path",
                          rdp_server_interface, "tls-cert",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "rdp-server-fingerprint",
                          rdp_server_interface, "tls-fingerprint",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "rdp-server-key-path",
                          rdp_server_interface, "tls-key",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "rdp-view-only",
                          rdp_server_interface, "view-only",
                          G_BINDING_SYNC_CREATE);

  g_signal_connect_object (rdp_server_interface, "handle-enable",
                           G_CALLBACK (on_handle_enable_rdp),
                           daemon, 0);
  g_signal_connect_object (rdp_server_interface, "handle-disable",
                           G_CALLBACK (on_handle_disable_rdp),
                           daemon, 0);
  g_signal_connect_object (rdp_server_interface, "handle-get-credentials",
                           G_CALLBACK (on_handle_get_rdp_credentials),
                           daemon, 0);
  g_signal_connect_object (rdp_server_interface, "handle-set-credentials",
                           G_CALLBACK (on_handle_set_rdp_credentials),
                           daemon, 0);
  g_signal_connect_object (rdp_server_interface, "handle-import-certificate",
                           G_CALLBACK (on_handle_import_certificate),
                           daemon, 0);

  g_dbus_interface_skeleton_export (
    G_DBUS_INTERFACE_SKELETON (rdp_server_interface),
    priv->connection,
    GRD_RDP_SERVER_OBJECT_PATH,
    NULL);

  grd_context_set_rdp_server_interface (priv->context, rdp_server_interface);
}

static void
unexport_rdp_server_interface (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdDBusRemoteDesktopRdpServer *rdp_server_interface =
    grd_context_get_rdp_server_interface (priv->context);

  g_dbus_interface_skeleton_unexport (
    G_DBUS_INTERFACE_SKELETON (rdp_server_interface));

  grd_context_set_rdp_server_interface (priv->context, NULL);
}

static void
start_rdp_server_when_ready (GrdDaemon *daemon,
                             gboolean   should_start_when_ready)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdSettings *settings = grd_context_get_settings (priv->context);

  g_signal_handlers_disconnect_by_func (G_OBJECT (settings),
                                        G_CALLBACK (maybe_start_rdp_server),
                                        daemon);

  if (!should_start_when_ready)
    return;

  g_signal_connect_object (G_OBJECT (settings),
                           "notify::rdp-server-cert",
                           G_CALLBACK (maybe_start_rdp_server),
                           daemon, G_CONNECT_SWAPPED);
  g_signal_connect_object (G_OBJECT (settings),
                           "notify::rdp-server-key",
                           G_CALLBACK (maybe_start_rdp_server),
                           daemon, G_CONNECT_SWAPPED);
}

static void
stop_rdp_server (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  start_rdp_server_when_ready (daemon, FALSE);

  if (!priv->rdp_server)
    return;

  g_signal_emit (daemon, signals[RDP_SERVER_STOPPED], 0);
  grd_rdp_server_stop (priv->rdp_server);
  g_clear_object (&priv->rdp_server);
  g_clear_handle_id (&priv->restart_rdp_server_source_id, g_source_remove);
  g_message ("RDP server stopped");
}

static void
on_rdp_server_binding_failed (GrdRdpServer *rdp_server,
                              GrdDaemon    *daemon)
{
  stop_rdp_server (daemon);
}

static void
start_rdp_server (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  g_autoptr (GError) error = NULL;

  g_assert (!priv->rdp_server);

  priv->rdp_server = grd_rdp_server_new (priv->context);
  g_signal_connect (priv->rdp_server, "binding-failed",
                    G_CALLBACK (on_rdp_server_binding_failed), daemon);
  if (!grd_rdp_server_start (priv->rdp_server, &error))
    {
      g_warning ("Failed to start RDP server: %s\n", error->message);
      stop_rdp_server (daemon);
    }
  else
    {
      g_signal_emit (daemon, signals[RDP_SERVER_STARTED], 0);
      g_message ("RDP server started");
    }
}

static void
maybe_start_rdp_server (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdSettings *settings = grd_context_get_settings (priv->context);
  g_autoptr (GError) error = NULL;
  g_autofree char *certificate = NULL;
  g_autofree char *key = NULL;
  gboolean rdp_enabled = FALSE;

  if (priv->rdp_server)
    return;

  if (!GRD_DAEMON_GET_CLASS (daemon)->is_daemon_ready (daemon))
    return;

  g_object_get (G_OBJECT (settings),
                "rdp-enabled", &rdp_enabled,
                "rdp-server-cert", &certificate,
                "rdp-server-key", &key,
                NULL);

  if (!rdp_enabled)
    return;

  if ((certificate && key) ||
      grd_context_get_runtime_mode (priv->context) == GRD_RUNTIME_MODE_HANDOVER)
    {
      start_rdp_server (daemon);
    }
  else
    {
      g_message ("RDP TLS certificate and key not yet configured properly");
      start_rdp_server_when_ready (daemon, TRUE);
    }
}

static gboolean
restart_rdp_server (gpointer user_data)
{
  GrdDaemon *daemon = user_data;
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  priv->restart_rdp_server_source_id = 0;

  maybe_start_rdp_server (daemon);

  return G_SOURCE_REMOVE;
}

void
grd_daemon_restart_rdp_server_with_delay (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  if (priv->restart_rdp_server_source_id)
    return;

  stop_rdp_server (daemon);

  priv->restart_rdp_server_source_id =
    g_timeout_add (RDP_SERVER_RESTART_DELAY_MS, restart_rdp_server, daemon);
}
#endif /* HAVE_RDP */

#ifdef HAVE_VNC
static gboolean
set_string_from_auth_method_enum (GBinding     *binding,
                                  const GValue *source_value,
                                  GValue       *target_value,
                                  gpointer      user_data)
{
  GrdVncAuthMethod src_auth_method = g_value_get_enum (source_value);

  switch (src_auth_method)
    {
    case GRD_VNC_AUTH_METHOD_PROMPT:
      g_value_set_string (target_value, "prompt");
      break;
    case GRD_VNC_AUTH_METHOD_PASSWORD:
      g_value_set_string (target_value, "password");
      break;
    }

  return TRUE;
}

static void
export_vnc_server_interface (GrdDaemon *daemon)
{
  GrdDBusRemoteDesktopVncServer *vnc_server_interface;
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdSettings *settings = grd_context_get_settings (priv->context);

  vnc_server_interface =
    grd_dbus_remote_desktop_vnc_server_skeleton_new ();

  grd_dbus_remote_desktop_vnc_server_set_enabled (vnc_server_interface, FALSE);
  grd_dbus_remote_desktop_vnc_server_set_port (vnc_server_interface, -1);
  g_object_bind_property (settings, "vnc-negotiate-port",
                          vnc_server_interface, "negotiate-port",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "vnc-view-only",
                          vnc_server_interface, "view-only",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property_full (settings, "vnc-auth-method",
                               vnc_server_interface, "auth-method",
                               G_BINDING_SYNC_CREATE,
                               set_string_from_auth_method_enum,
                               NULL, NULL, NULL);

  g_dbus_interface_skeleton_export (
    G_DBUS_INTERFACE_SKELETON (vnc_server_interface),
    priv->connection,
    GRD_VNC_SERVER_OBJECT_PATH,
    NULL);

  grd_context_set_vnc_server_interface (priv->context, vnc_server_interface);
}

static void
unexport_vnc_server_interface (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdDBusRemoteDesktopVncServer *vnc_server_interface =
    grd_context_get_vnc_server_interface (priv->context);

  g_dbus_interface_skeleton_unexport (
    G_DBUS_INTERFACE_SKELETON (vnc_server_interface));

  grd_context_set_vnc_server_interface (priv->context, NULL);
}

static void
stop_vnc_server (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  if (!priv->vnc_server)
    return;

  grd_vnc_server_stop (priv->vnc_server);
  g_clear_object (&priv->vnc_server);
  g_message ("VNC server stopped");
}

static void
start_vnc_server (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  g_autoptr (GError) error = NULL;

  if (priv->vnc_server)
    return;

  priv->vnc_server = grd_vnc_server_new (priv->context);
  if (!grd_vnc_server_start (priv->vnc_server, &error))
    {
      g_warning ("Failed to initialize VNC server: %s\n", error->message);
      stop_vnc_server (daemon);
    }
  else
    {
      g_message ("VNC server started");
    }
}
#endif /* HAVE_VNC */

static void
export_services_status (GrdDaemon *daemon)
{
  export_remote_desktop_interface (daemon);

#ifdef HAVE_RDP
  export_rdp_server_interface (daemon);
#endif
#ifdef HAVE_VNC
  if (GRD_IS_DAEMON_USER (daemon))
    export_vnc_server_interface (daemon);
#endif
}

static void
unexport_services_status (GrdDaemon *daemon)
{
  unexport_remote_desktop_interface (daemon);

#ifdef HAVE_RDP
  unexport_rdp_server_interface (daemon);
#endif
#ifdef HAVE_VNC
  if (GRD_IS_DAEMON_USER (daemon))
    unexport_vnc_server_interface (daemon);
#endif
}

void
grd_daemon_maybe_enable_services (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdSettings *settings = grd_context_get_settings (priv->context);
  gboolean rdp_enabled;
  gboolean vnc_enabled;

  if (!GRD_DAEMON_GET_CLASS (daemon)->is_daemon_ready (daemon))
    return;

  grd_context_notify_daemon_ready (priv->context);

  g_object_get (G_OBJECT (settings),
                "rdp-enabled", &rdp_enabled,
                "vnc-enabled", &vnc_enabled,
                NULL);

#ifdef HAVE_RDP
  maybe_start_rdp_server (daemon);
#endif

#ifdef HAVE_VNC
  if (vnc_enabled)
    start_vnc_server (daemon);
#endif
}

void
grd_daemon_disable_services (GrdDaemon *daemon)
{
#ifdef HAVE_RDP
  stop_rdp_server (daemon);
#endif
#ifdef HAVE_VNC
  stop_vnc_server (daemon);
#endif
}

static void
on_mutter_remote_desktop_proxy_acquired (GObject      *object,
                                         GAsyncResult *result,
                                         gpointer      user_data)
{
  GrdDaemon *daemon = user_data;
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdDBusMutterRemoteDesktop *proxy;
  g_autoptr (GError) error = NULL;

  proxy = grd_dbus_mutter_remote_desktop_proxy_new_finish (result, &error);
  if (!proxy)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to create remote desktop proxy: %s", error->message);
      return;
    }

  grd_context_set_mutter_remote_desktop_proxy (priv->context, proxy);

  g_signal_emit (daemon, signals[MUTTER_PROXY_ACQUIRED], 0);
}

static void
on_mutter_screen_cast_proxy_acquired (GObject      *object,
                                      GAsyncResult *result,
                                      gpointer      user_data)
{
  GrdDaemon *daemon = user_data;
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdDBusMutterScreenCast *proxy;
  g_autoptr (GError) error = NULL;

  proxy = grd_dbus_mutter_screen_cast_proxy_new_finish (result, &error);
  if (!proxy)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to create screen cast proxy: %s", error->message);
      return;
    }

  grd_context_set_mutter_screen_cast_proxy (priv->context, proxy);

  g_signal_emit (daemon, signals[MUTTER_PROXY_ACQUIRED], 0);
}

static void
on_mutter_remote_desktop_name_appeared (GDBusConnection *connection,
                                        const char      *name,
                                        const char      *name_owner,
                                        gpointer         user_data)
{
  GrdDaemon *daemon = user_data;
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  grd_dbus_mutter_remote_desktop_proxy_new (connection,
                                            G_DBUS_PROXY_FLAGS_NONE,
                                            MUTTER_REMOTE_DESKTOP_BUS_NAME,
                                            MUTTER_REMOTE_DESKTOP_OBJECT_PATH,
                                            priv->cancellable,
                                            on_mutter_remote_desktop_proxy_acquired,
                                            daemon);
}

static void
on_mutter_remote_desktop_name_vanished (GDBusConnection *connection,
                                        const char      *name,
                                        gpointer         user_data)
{
  GrdDaemon *daemon = user_data;
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  grd_daemon_disable_services (daemon);

  grd_context_set_mutter_remote_desktop_proxy (priv->context, NULL);
}

static void
on_mutter_screen_cast_name_appeared (GDBusConnection *connection,
                                     const char      *name,
                                     const char      *name_owner,
                                     gpointer         user_data)
{
  GrdDaemon *daemon = user_data;
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  grd_dbus_mutter_screen_cast_proxy_new (connection,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         MUTTER_SCREEN_CAST_BUS_NAME,
                                         MUTTER_SCREEN_CAST_OBJECT_PATH,
                                         priv->cancellable,
                                         on_mutter_screen_cast_proxy_acquired,
                                         daemon);
}

static void
on_mutter_screen_cast_name_vanished (GDBusConnection *connection,
                                     const char      *name,
                                     gpointer         user_data)
{
  GrdDaemon *daemon = user_data;
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  grd_daemon_disable_services (daemon);

  grd_context_set_mutter_screen_cast_proxy (priv->context, NULL);
}

void
grd_daemon_acquire_mutter_dbus_proxies (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  g_clear_handle_id (&priv->mutter_remote_desktop_watch_name_id, g_bus_unwatch_name);
  priv->mutter_remote_desktop_watch_name_id =
    g_bus_watch_name (G_BUS_TYPE_SESSION,
                      MUTTER_REMOTE_DESKTOP_BUS_NAME,
                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                      on_mutter_remote_desktop_name_appeared,
                      on_mutter_remote_desktop_name_vanished,
                      daemon, NULL);

  g_clear_handle_id (&priv->mutter_screen_cast_watch_name_id, g_bus_unwatch_name);
  priv->mutter_screen_cast_watch_name_id =
    g_bus_watch_name (G_BUS_TYPE_SESSION,
                      MUTTER_SCREEN_CAST_BUS_NAME,
                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                      on_mutter_screen_cast_name_appeared,
                      on_mutter_screen_cast_name_vanished,
                      daemon, NULL);
}

#ifdef HAVE_RDP
static void
on_rdp_enabled_changed (GrdSettings *settings,
                        GParamSpec  *pspec,
                        GrdDaemon   *daemon)
{
  gboolean rdp_enabled;

  if (!GRD_DAEMON_GET_CLASS (daemon)->is_daemon_ready (daemon))
    return;

  g_object_get (G_OBJECT (settings), "rdp-enabled", &rdp_enabled, NULL);
  if (rdp_enabled)
    maybe_start_rdp_server (daemon);
  else
    stop_rdp_server (daemon);
}
#endif /* HAVE_RDP */

#ifdef HAVE_VNC
static void
on_vnc_enabled_changed (GrdSettings *settings,
                        GParamSpec  *pspec,
                        GrdDaemon   *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  gboolean vnc_enabled;

  if (!GRD_DAEMON_GET_CLASS (daemon)->is_daemon_ready (daemon))
    return;

  g_object_get (G_OBJECT (settings), "vnc-enabled", &vnc_enabled, NULL);
  if (vnc_enabled)
    {
      g_return_if_fail (!priv->vnc_server);
      start_vnc_server (daemon);
    }
  else
    {
      stop_vnc_server (daemon);
    }
}
#endif /* HAVE_VNC */

static void
grd_daemon_init (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  priv->cancellable = g_cancellable_new ();
}

static GDBusConnection *
get_daemon_dbus_connection (GrdDaemon *daemon)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

#if defined(HAVE_RDP) && defined(HAVE_LIBSYSTEMD)
  if (GRD_IS_DAEMON_SYSTEM (daemon))
    connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  else
#endif /* HAVE_RDP && HAVE_LIBSYSTEMD */
    connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);

  if (!connection)
    {
      g_warning ("Failing acquiring dbus connection: %s", error->message);
      return NULL;
    }

  return g_steal_pointer (&connection);
}

static void
grd_daemon_startup (GApplication *app)
{
  GrdDaemon *daemon = GRD_DAEMON (app);
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdSettings *settings = grd_context_get_settings (priv->context);

  priv->connection = get_daemon_dbus_connection (daemon);
  export_services_status (daemon);

#ifdef HAVE_RDP
  if (GRD_IS_SETTINGS_USER (settings) ||
      GRD_IS_SETTINGS_SYSTEM (settings))
    {
      g_signal_connect (settings, "notify::rdp-enabled",
                        G_CALLBACK (on_rdp_enabled_changed),
                        daemon);
    }
#endif
#ifdef HAVE_VNC
  if (GRD_IS_SETTINGS_USER (settings))
    {
      g_signal_connect (settings, "notify::vnc-enabled",
                        G_CALLBACK (on_vnc_enabled_changed),
                        daemon);
    }
#endif

  /* Run indefinitely, until told to exit. */
  g_application_hold (app);

  G_APPLICATION_CLASS (grd_daemon_parent_class)->startup (app);
}

static void
grd_daemon_shutdown (GApplication *app)
{
  GrdDaemon *daemon = GRD_DAEMON (app);
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  g_cancellable_cancel (priv->cancellable);
  g_clear_object (&priv->cancellable);

  grd_daemon_disable_services (daemon);

  unexport_services_status (daemon);
  g_clear_object (&priv->connection);

  grd_context_set_mutter_remote_desktop_proxy (priv->context, NULL);
  g_clear_handle_id (&priv->mutter_remote_desktop_watch_name_id, g_bus_unwatch_name);

  grd_context_set_mutter_screen_cast_proxy (priv->context, NULL);
  g_clear_handle_id (&priv->mutter_screen_cast_watch_name_id, g_bus_unwatch_name);

  g_clear_object (&priv->context);

  if (priv->sigterm_source)
    {
      g_source_destroy (priv->sigterm_source);
      g_clear_pointer (&priv->sigterm_source, g_source_unref);
    }
  if (priv->sigint_source)
    {
      g_source_destroy (priv->sigint_source);
      g_clear_pointer (&priv->sigint_source, g_source_unref);
    }

  G_APPLICATION_CLASS (grd_daemon_parent_class)->shutdown (app);
}

static void
grd_daemon_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  GrdDaemon *daemon = GRD_DAEMON (object);
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, priv->context);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_daemon_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  GrdDaemon *daemon = GRD_DAEMON (object);
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      priv->context = g_value_get_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_daemon_class_init (GrdDaemonClass *klass)
{
  GApplicationClass *g_application_class = G_APPLICATION_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_application_class->startup = grd_daemon_startup;
  g_application_class->shutdown = grd_daemon_shutdown;

  object_class->get_property = grd_daemon_get_property;
  object_class->set_property = grd_daemon_set_property;

  g_object_class_install_property (object_class,
                                   PROP_CONTEXT,
                                   g_param_spec_object ("context",
                                                        "GrdContext",
                                                        "The GrdContext instance",
                                                        GRD_TYPE_CONTEXT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  signals[MUTTER_PROXY_ACQUIRED] = g_signal_new ("mutter-proxy-acquired",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0,
                                                 NULL, NULL, NULL,
                                                 G_TYPE_NONE, 0);
  signals[RDP_SERVER_STARTED] = g_signal_new ("rdp-server-started",
                                              G_TYPE_FROM_CLASS (klass),
                                              G_SIGNAL_RUN_LAST,
                                              0,
                                              NULL, NULL, NULL,
                                              G_TYPE_NONE, 0);
  signals[RDP_SERVER_STOPPED] = g_signal_new ("rdp-server-stopped",
                                              G_TYPE_FROM_CLASS (klass),
                                              G_SIGNAL_RUN_LAST,
                                              0,
                                              NULL, NULL, NULL,
                                              G_TYPE_NONE, 0);
}

static void
activate_terminate (GAction   *action,
                    GVariant  *parameter,
                    GrdDaemon *daemon)
{
  g_application_release (G_APPLICATION (daemon));
}

static void
add_actions (GApplication *app)
{
  g_autoptr(GSimpleAction) action = NULL;

  action = g_simple_action_new ("terminate", NULL);
  g_signal_connect (action, "activate", G_CALLBACK (activate_terminate), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (action));
}

static gboolean
sigint_terminate_daemon (gpointer user_data)
{
  GrdDaemon *daemon = user_data;
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  g_debug ("Received SIGINT signal. Exiting...");
  g_clear_pointer (&priv->sigint_source, g_source_unref);
  g_application_release (G_APPLICATION (daemon));

  return G_SOURCE_REMOVE;
}

static gboolean
sigterm_terminate_daemon (gpointer user_data)
{
  GrdDaemon *daemon = user_data;
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  g_debug ("Received SIGTERM signal. Exiting...");
  g_clear_pointer (&priv->sigterm_source, g_source_unref);
  g_application_release (G_APPLICATION (daemon));

  return G_SOURCE_REMOVE;
}

static void
register_signals (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  priv->sigint_source = g_unix_signal_source_new (SIGINT);
  g_source_set_callback (priv->sigint_source, sigint_terminate_daemon,
                         daemon, NULL);
  g_source_attach (priv->sigint_source, NULL);

  priv->sigterm_source = g_unix_signal_source_new (SIGTERM);
  g_source_set_callback (priv->sigterm_source, sigterm_terminate_daemon,
                         daemon, NULL);
  g_source_attach (priv->sigterm_source, NULL);
}

static int
count_trues (int n_args, ...)
{
  va_list booleans;
  int booleans_count = 0;

  va_start (booleans, n_args);

  for (int i = 0; i < n_args; ++i)
    booleans_count += va_arg (booleans, gboolean) ? 1 : 0;

  return booleans_count;
}

int
main (int argc, char **argv)
{
  GrdContext *context;
  GrdSettings *settings;
  gboolean print_version = FALSE;
  gboolean headless = FALSE;
  gboolean system = FALSE;
  gboolean handover = FALSE;
  int rdp_port = -1;
  int vnc_port = -1;

  GOptionEntry entries[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, &print_version,
      "Print version", NULL },
    { "headless", 0, 0, G_OPTION_ARG_NONE, &headless,
      "Run in headless mode", NULL },
#if defined(HAVE_RDP) && defined(HAVE_LIBSYSTEMD)
    { "system", 0, 0, G_OPTION_ARG_NONE, &system,
      "Run in headless mode as a system g-r-d service", NULL },
    { "handover", 0, 0, G_OPTION_ARG_NONE, &handover,
      "Run in headless mode taking a connection from system g-r-d service", NULL },
#endif /* HAVE_RDP && HAVE_LIBSYSTEMD */
    { "rdp-port", 0, 0, G_OPTION_ARG_INT, &rdp_port,
      "RDP port", NULL },
    { "vnc-port", 0, 0, G_OPTION_ARG_INT, &vnc_port,
      "VNC port", NULL },
    { NULL }
  };
  g_autoptr (GOptionContext) option_context = NULL;
  g_autoptr (GrdDaemon) daemon = NULL;
  GError *error = NULL;
  GrdRuntimeMode runtime_mode;

  g_set_application_name (_("GNOME Remote Desktop"));

  option_context = g_option_context_new (NULL);
  g_option_context_add_main_entries (option_context, entries, GETTEXT_PACKAGE);
  if (!g_option_context_parse (option_context, &argc, &argv, &error))
    {
      g_printerr ("Invalid option: %s\n", error->message);
      g_error_free (error);
      return EXIT_FAILURE;
    }

  if (print_version)
    {
      g_print ("GNOME Remote Desktop %s\n", VERSION);
      return EXIT_SUCCESS;
    }

  if (count_trues (3, headless, system, handover) > 1)
    {
      g_printerr ("Invalid option: More than one runtime mode specified");
      return EXIT_FAILURE;
    }

  if (headless)
    runtime_mode = GRD_RUNTIME_MODE_HEADLESS;
  else if (system)
    runtime_mode = GRD_RUNTIME_MODE_SYSTEM;
  else if (handover)
    runtime_mode = GRD_RUNTIME_MODE_HANDOVER;
  else
    runtime_mode = GRD_RUNTIME_MODE_SCREEN_SHARE;

  switch (runtime_mode)
    {
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
    case GRD_RUNTIME_MODE_HEADLESS:
      daemon = GRD_DAEMON (grd_daemon_user_new (runtime_mode, &error));
      break;
#ifdef HAVE_LIBSYSTEMD
    case GRD_RUNTIME_MODE_SYSTEM:
      daemon = GRD_DAEMON (grd_daemon_system_new (&error));
      break;
    case GRD_RUNTIME_MODE_HANDOVER:
      daemon = GRD_DAEMON (grd_daemon_handover_new (&error));
      break;
#else
    case GRD_RUNTIME_MODE_SYSTEM:
    case GRD_RUNTIME_MODE_HANDOVER:
      g_assert_not_reached ();
      break;
#endif /* HAVE_LIBSYSTEMD */
    }

  if (!daemon)
    {
      g_printerr ("Failed to initialize: %s\n", error->message);
      g_error_free (error);
      return EXIT_FAILURE;
    }

  add_actions (G_APPLICATION (daemon));
  register_signals (daemon);

  context = grd_daemon_get_context (daemon);
  settings = grd_context_get_settings (context);
  if (rdp_port != -1)
    grd_settings_override_rdp_port (settings, rdp_port);
  if (vnc_port != -1)
    grd_settings_override_vnc_port (settings, vnc_port);

  return g_application_run (G_APPLICATION (daemon), argc, argv);
}
