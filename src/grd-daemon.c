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

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>

#include "grd-context.h"
#include "grd-daemon-user.h"
#include "grd-dbus-mutter-remote-desktop.h"
#include "grd-private.h"
#include "grd-rdp-server.h"
#include "grd-session.h"
#include "grd-vnc-server.h"

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

#ifdef HAVE_RDP
  GrdRdpServer *rdp_server;
#endif
#ifdef HAVE_VNC
  GrdVncServer *vnc_server;
#endif
} GrdDaemonPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdDaemon, grd_daemon, G_TYPE_APPLICATION)

GrdContext *
grd_daemon_get_context (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  return priv->context;
}

#ifdef HAVE_RDP
static void
stop_rdp_server (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  if (!priv->rdp_server)
    return;

  g_signal_emit (daemon, signals[RDP_SERVER_STOPPED], 0);
  grd_rdp_server_stop (priv->rdp_server);
  g_clear_object (&priv->rdp_server);
  g_message ("RDP server stopped");
}

static void
start_rdp_server (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdSettings *settings = grd_context_get_settings (priv->context);
  g_autoptr (GError) error = NULL;

  if (priv->rdp_server)
    return;

  if (!g_access (grd_settings_get_rdp_server_cert (settings), F_OK) &&
      !g_access (grd_settings_get_rdp_server_key (settings), F_OK))
    {
      priv->rdp_server = grd_rdp_server_new (priv->context);
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
  else
    {
      g_warning ("RDP TLS certificate and key not configured properly");
    }
}
#endif /* HAVE_RDP */

#ifdef HAVE_VNC
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

void
grd_daemon_maybe_enable_services (GrdDaemon *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdSettings *settings = grd_context_get_settings (priv->context);

  if (!GRD_DAEMON_GET_CLASS (daemon)->is_daemon_ready (daemon))
    return;

  grd_context_notify_daemon_ready (priv->context);

#ifdef HAVE_RDP
  if (grd_settings_is_rdp_enabled (settings))
    start_rdp_server (daemon);
#endif

#ifdef HAVE_VNC
  if (grd_settings_is_vnc_enabled (settings))
    start_vnc_server (daemon);
#endif
}

static void
disable_services (GrdDaemon *daemon)
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
  GError *error = NULL;

  proxy = grd_dbus_mutter_remote_desktop_proxy_new_finish (result, &error);
  if (!proxy)
    {
      g_warning ("Failed to create remote desktop proxy: %s", error->message);
      g_error_free (error);
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
  GError *error = NULL;

  proxy = grd_dbus_mutter_screen_cast_proxy_new_finish (result, &error);
  if (!proxy)
    {
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

  disable_services (daemon);

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

  disable_services (daemon);

  grd_context_set_mutter_screen_cast_proxy (priv->context, NULL);
}

void
grd_daemon_acquire_mutter_dbus_proxies (GrdDaemon       *daemon,
                                        GDBusConnection *connection)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  g_clear_handle_id (&priv->mutter_remote_desktop_watch_name_id, g_bus_unwatch_name);
  priv->mutter_remote_desktop_watch_name_id =
    g_bus_watch_name_on_connection (connection,
                                    MUTTER_REMOTE_DESKTOP_BUS_NAME,
                                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    on_mutter_remote_desktop_name_appeared,
                                    on_mutter_remote_desktop_name_vanished,
                                    daemon, NULL);

  g_clear_handle_id (&priv->mutter_screen_cast_watch_name_id, g_bus_unwatch_name);
  priv->mutter_screen_cast_watch_name_id =
    g_bus_watch_name_on_connection (connection,
                                    MUTTER_SCREEN_CAST_BUS_NAME,
                                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    on_mutter_screen_cast_name_appeared,
                                    on_mutter_screen_cast_name_vanished,
                                    daemon, NULL);
}

#ifdef HAVE_RDP
static void
on_rdp_enabled_changed (GrdSettings *settings,
                        GrdDaemon   *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  if (!GRD_DAEMON_GET_CLASS (daemon)->is_daemon_ready (daemon))
    return;

  if (grd_settings_is_rdp_enabled (settings))
    {
      g_return_if_fail (!priv->rdp_server);
      start_rdp_server (daemon);
    }
  else
    {
      stop_rdp_server (daemon);
    }
}
#endif /* HAVE_RDP */

#ifdef HAVE_VNC
static void
on_vnc_enabled_changed (GrdSettings *settings,
                        GrdDaemon   *daemon)
{
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);

  if (!GRD_DAEMON_GET_CLASS (daemon)->is_daemon_ready (daemon))
    return;

  if (grd_settings_is_vnc_enabled (settings))
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

static void
grd_daemon_startup (GApplication *app)
{
  GrdDaemon *daemon = GRD_DAEMON (app);
  GrdDaemonPrivate *priv = grd_daemon_get_instance_private (daemon);
  GrdSettings *settings = grd_context_get_settings (priv->context);

#ifdef HAVE_RDP
  g_signal_connect (settings, "rdp-enabled-changed",
                    G_CALLBACK (on_rdp_enabled_changed),
                    daemon);
#endif
#ifdef HAVE_VNC
  g_signal_connect (settings, "vnc-enabled-changed",
                    G_CALLBACK (on_vnc_enabled_changed),
                    daemon);
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

  disable_services (daemon);

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

int
main (int argc, char **argv)
{
  GrdContext *context;
  GrdSettings *settings;
  gboolean print_version = FALSE;
  gboolean headless = FALSE;
  int rdp_port = -1;
  int vnc_port = -1;

  GOptionEntry entries[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, &print_version,
      "Print version", NULL },
    { "headless", 0, 0, G_OPTION_ARG_NONE, &headless,
      "Run in headless mode", NULL },
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

  if (headless)
    runtime_mode = GRD_RUNTIME_MODE_HEADLESS;
  else
    runtime_mode = GRD_RUNTIME_MODE_SCREEN_SHARE;

  switch (runtime_mode)
    {
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
    case GRD_RUNTIME_MODE_HEADLESS:
      daemon = GRD_DAEMON (grd_daemon_user_new (runtime_mode, &error));
      break;
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
