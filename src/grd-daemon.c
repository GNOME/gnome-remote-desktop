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
#include <stdio.h>
#include <stdlib.h>

#include "grd-context.h"
#include "grd-dbus-remote-desktop.h"
#include "grd-private.h"
#include "grd-session.h"
#include "grd-session-dummy.h"
#include "grd-vnc-server.h"

struct _GrdDaemon
{
  GApplication parent;

  GCancellable *cancellable;
  guint remote_desktop_watch_name_id;
  guint screen_cast_watch_name_id;

  GrdContext *context;

  GrdVncServer *vnc_server;
  GrdSessionDummy *dummy_session;
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

static void
maybe_enable_services (GrdDaemon *daemon)
{
  GError *error = NULL;

  if (!is_daemon_ready (daemon))
    return;

  daemon->vnc_server = grd_vnc_server_new (daemon->context);
  if (!grd_vnc_server_start (daemon->vnc_server, &error))
    {
      g_warning ("Failed to initialize VNC server: %s\n", error->message);
    }
}

static void
close_all_sessions (GrdDaemon *daemon)
{
  GList *l;

  while ((l = grd_context_get_sessions (daemon->context)))
    {
      GrdSession *session = l->data;

      grd_session_stop (session);
    }
}

static void
disable_services (GrdDaemon *daemon)
{
  close_all_sessions (daemon);
  g_clear_object (&daemon->vnc_server);
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

static void
grd_daemon_init (GrdDaemon *daemon)
{
}

static void
grd_daemon_startup (GApplication *app)
{
  GrdDaemon *daemon = GRD_DAEMON (app);

  daemon->context = g_object_new (GRD_TYPE_CONTEXT, NULL);

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
on_dummy_session_stopped (GrdSession *session,
                          GrdDaemon  *daemon)
{
  g_print ("Dummy remote desktop stopped\n");
  g_clear_object (&daemon->dummy_session);
}

static void
activate_toggle_record (GAction   *action,
                        GVariant  *parameter,
                        GrdDaemon *daemon)
{
  if (!daemon->dummy_session)
    {
      if (!is_daemon_ready (daemon))
        {
          g_print ("Can't start dummy remote desktop, not ready.\n");
          return;
        }

      g_print ("Start dummy remote desktop\n");

      daemon->dummy_session = g_object_new (GRD_TYPE_SESSION_DUMMY,
                                            "context", daemon->context,
                                            NULL);
      g_signal_connect (daemon->dummy_session, "stopped",
                        G_CALLBACK (on_dummy_session_stopped),
                        daemon);
      grd_context_add_session (daemon->context,
                               GRD_SESSION (daemon->dummy_session));
    }
  else
    {

      if (daemon->dummy_session)
        {
          g_print ("Stop dummy remote desktop\n");
          grd_session_stop (GRD_SESSION (daemon->dummy_session));
        }

      g_assert (!daemon->dummy_session);
    }
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
  g_autoptr(GSimpleAction) action;

  action = g_simple_action_new ("toggle-record", NULL);
  g_signal_connect (action, "activate", G_CALLBACK (activate_toggle_record), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (action));

  action = g_simple_action_new ("terminate", NULL);
  g_signal_connect (action, "activate", G_CALLBACK (activate_terminate), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (action));
}

int
main (int argc, char **argv)
{
  gboolean print_version = FALSE;
  GOptionEntry entries[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, &print_version,
      "Print version", NULL },
    { NULL }
  };
  g_autoptr(GOptionContext) context;
  g_autoptr(GApplication) app = NULL;
  GError *error = NULL;

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

  return g_application_run (app, argc, argv);
}
