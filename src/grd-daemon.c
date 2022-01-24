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
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>

#include "grd-context.h"
#include "grd-dbus-remote-desktop.h"
#include "grd-private.h"
#include "grd-rdp-server.h"
#include "grd-session.h"
#include "grd-vnc-server.h"

struct _GrdDaemon
{
  GApplication parent;

  GCancellable *cancellable;
  guint remote_desktop_watch_name_id;
  guint screen_cast_watch_name_id;

  GrdContext *context;

#ifdef HAVE_RDP
  GrdRdpServer *rdp_server;
#endif
#ifdef HAVE_VNC
  GrdVncServer *vnc_server;
#endif
};

G_DEFINE_TYPE (GrdDaemon, grd_daemon, G_TYPE_APPLICATION)

static gboolean
is_daemon_ready (GrdDaemon *daemon)
{
  if (!grd_context_get_remote_desktop_proxy (daemon->context) ||
      !grd_context_get_screen_cast_proxy (daemon->context))
    return FALSE;

  return TRUE;
}

#ifdef HAVE_RDP
static void
start_rdp_server (GrdDaemon *daemon)
{
  GrdSettings *settings = grd_context_get_settings (daemon->context);
  g_autoptr (GError) error = NULL;

  g_assert (!daemon->rdp_server);

  if (!g_access (grd_settings_get_rdp_server_cert (settings), F_OK) &&
      !g_access (grd_settings_get_rdp_server_key (settings), F_OK))
    {
      daemon->rdp_server = grd_rdp_server_new (daemon->context);
      if (!grd_rdp_server_start (daemon->rdp_server, &error))
        g_warning ("Failed to start RDP server: %s\n", error->message);
      else
        g_message ("RDP server started");
    }
  else
    {
      g_warning ("RDP TLS certificate and key not configured properly");
    }
}

static void
stop_rdp_server (GrdDaemon *daemon)
{
  if (!daemon->rdp_server)
    return;

  grd_rdp_server_stop (daemon->rdp_server);
  g_clear_object (&daemon->rdp_server);
  g_message ("RDP server stopped");
}
#endif /* HAVE_RDP */

#ifdef HAVE_VNC
static void
start_vnc_server (GrdDaemon *daemon)
{
  g_autoptr (GError) error = NULL;

  g_assert (!daemon->vnc_server);

  daemon->vnc_server = grd_vnc_server_new (daemon->context);
  if (!grd_vnc_server_start (daemon->vnc_server, &error))
    g_warning ("Failed to initialize VNC server: %s\n", error->message);
  else
    g_message ("VNC server started");
}

static void
stop_vnc_server (GrdDaemon *daemon)
{
  if (!daemon->vnc_server)
    return;

  grd_vnc_server_stop (daemon->vnc_server);
  g_clear_object (&daemon->vnc_server);
  g_message ("VNC server stopped");
}
#endif /* HAVE_VNC */

static void
maybe_enable_services (GrdDaemon *daemon)
{
  GrdSettings *settings = grd_context_get_settings (daemon->context);

  if (!is_daemon_ready (daemon))
    return;

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
on_remote_desktop_proxy_acquired (GObject      *object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  GrdDaemon *daemon = user_data;
  GrdDBusRemoteDesktop *proxy;
  GError *error = NULL;

  proxy = grd_dbus_remote_desktop_proxy_new_for_bus_finish (result, &error);
  if (!proxy)
    {
      g_warning ("Failed to create remote desktop proxy: %s", error->message);
      g_error_free (error);
      return;
    }

  grd_context_set_remote_desktop_proxy (daemon->context, proxy);

  maybe_enable_services (daemon);
}

static void
on_screen_cast_proxy_acquired (GObject      *object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  GrdDaemon *daemon = user_data;
  GrdDBusScreenCast *proxy;
  GError *error = NULL;

  proxy = grd_dbus_screen_cast_proxy_new_for_bus_finish (result, &error);
  if (!proxy)
    {
      g_warning ("Failed to create screen cast proxy: %s", error->message);
      return;
    }

  grd_context_set_screen_cast_proxy (daemon->context, proxy);

  maybe_enable_services (daemon);
}

static void
on_remote_desktop_name_appeared (GDBusConnection *connection,
                                 const char      *name,
                                 const char      *name_owner,
                                 gpointer         user_data)
{
  GrdDaemon *daemon = user_data;

  grd_dbus_remote_desktop_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                             G_DBUS_PROXY_FLAGS_NONE,
                                             MUTTER_REMOTE_DESKTOP_BUS_NAME,
                                             MUTTER_REMOTE_DESKTOP_OBJECT_PATH,
                                             daemon->cancellable,
                                             on_remote_desktop_proxy_acquired,
                                             daemon);
}

static void
on_remote_desktop_name_vanished (GDBusConnection *connection,
                                 const char      *name,
                                 gpointer         user_data)
{
  GrdDaemon *daemon = user_data;

  disable_services (daemon);
  grd_context_set_remote_desktop_proxy (daemon->context, NULL);
}

static void
on_screen_cast_name_appeared (GDBusConnection *connection,
                              const char      *name,
                              const char      *name_owner,
                              gpointer         user_data)
{
  GrdDaemon *daemon = user_data;

  grd_dbus_screen_cast_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          MUTTER_SCREEN_CAST_BUS_NAME,
                                          MUTTER_SCREEN_CAST_OBJECT_PATH,
                                          daemon->cancellable,
                                          on_screen_cast_proxy_acquired,
                                          daemon);
}

static void
on_screen_cast_name_vanished (GDBusConnection *connection,
                              const char      *name,
                              gpointer         user_data)
{
  GrdDaemon *daemon = user_data;

  disable_services (daemon);
  grd_context_set_screen_cast_proxy (daemon->context, NULL);
}

#ifdef HAVE_RDP
static void
on_rdp_enabled_changed (GrdSettings *settings,
                        GrdDaemon   *daemon)
{
  if (!is_daemon_ready (daemon))
    return;

  if (grd_settings_is_rdp_enabled (settings))
    {
      g_return_if_fail (!daemon->rdp_server);
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
  if (!is_daemon_ready (daemon))
    return;

  if (grd_settings_is_vnc_enabled (settings))
    {
      g_return_if_fail (!daemon->vnc_server);
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
  GrdSettings *settings;

  daemon->context = g_object_new (GRD_TYPE_CONTEXT, NULL);

  settings = grd_context_get_settings (daemon->context);

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
}

static void
grd_daemon_startup (GApplication *app)
{
  GrdDaemon *daemon = GRD_DAEMON (app);

  daemon->remote_desktop_watch_name_id =
    g_bus_watch_name (G_BUS_TYPE_SESSION,
                      MUTTER_REMOTE_DESKTOP_BUS_NAME,
                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                      on_remote_desktop_name_appeared,
                      on_remote_desktop_name_vanished,
                      daemon, NULL);

  daemon->screen_cast_watch_name_id =
    g_bus_watch_name (G_BUS_TYPE_SESSION,
                      MUTTER_SCREEN_CAST_BUS_NAME,
                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                      on_screen_cast_name_appeared,
                      on_screen_cast_name_vanished,
                      daemon, NULL);

  daemon->cancellable = g_cancellable_new ();

  /* Run indefinitely, until told to exit. */
  g_application_hold (app);

  G_APPLICATION_CLASS (grd_daemon_parent_class)->startup (app);
}

static void
grd_daemon_shutdown (GApplication *app)
{
  GrdDaemon *daemon = GRD_DAEMON (app);

  g_cancellable_cancel (daemon->cancellable);
  g_clear_object (&daemon->cancellable);

  disable_services (daemon);

  grd_context_set_remote_desktop_proxy (daemon->context, NULL);
  g_bus_unwatch_name (daemon->remote_desktop_watch_name_id);
  daemon->remote_desktop_watch_name_id = 0;

  grd_context_set_screen_cast_proxy (daemon->context, NULL);
  g_bus_unwatch_name (daemon->screen_cast_watch_name_id);
  daemon->screen_cast_watch_name_id = 0;

  g_clear_object (&daemon->context);

  G_APPLICATION_CLASS (grd_daemon_parent_class)->shutdown (app);
}

static void
grd_daemon_class_init (GrdDaemonClass *klass)
{
  GApplicationClass *g_application_class = G_APPLICATION_CLASS (klass);

  g_application_class->startup = grd_daemon_startup;
  g_application_class->shutdown = grd_daemon_shutdown;
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

int
main (int argc, char **argv)
{
  GrdSettings *settings;
  gboolean print_version = FALSE;
  int rdp_port = -1;
  int vnc_port = -1;

  GOptionEntry entries[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, &print_version,
      "Print version", NULL },
    { "rdp-port", 0, 0, G_OPTION_ARG_INT, &rdp_port,
      "RDP port", NULL },
    { "vnc-port", 0, 0, G_OPTION_ARG_INT, &vnc_port,
      "VNC port", NULL },
    { NULL }
  };
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GApplication) app = NULL;
  GError *error = NULL;

  g_set_application_name (_("GNOME Remote Desktop"));

  context = g_option_context_new (NULL);
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  if (!g_option_context_parse (context, &argc, &argv, &error))
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

  app = g_object_new (GRD_TYPE_DAEMON,
                      "application-id", GRD_DAEMON_APPLICATION_ID,
                      "flags", G_APPLICATION_IS_SERVICE,
                      NULL);

  add_actions (app);

  settings = grd_context_get_settings (GRD_DAEMON (app)->context);
  if (rdp_port != -1)
    grd_settings_override_rdp_port (settings, rdp_port);
  if (vnc_port != -1)
    grd_settings_override_vnc_port (settings, vnc_port);

  return g_application_run (app, argc, argv);
}
