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
  guint watch_name_id;

  GrdContext *context;

  GrdVncServer *vnc_server;
  GrdSessionDummy *dummy_session;
};

static void grd_daemon_async_initable_init (GAsyncInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (GrdDaemon,
                         grd_daemon,
                         G_TYPE_APPLICATION,
                         G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE,
                                                grd_daemon_async_initable_init));

static void
on_proxy_acquired (GObject      *object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  GTask *task = user_data;
  GrdDaemon *daemon = g_task_get_source_object (task);
  GrdDBusRemoteDesktop *proxy;
  GError *error = NULL;

  proxy = grd_dbus_remote_desktop_proxy_new_for_bus_finish (result, &error);
  if (!proxy)
    return g_task_return_error (task, error);

  grd_context_set_dbus_proxy (daemon->context, proxy);

  g_task_return_boolean (task, TRUE);
}

static void
on_name_appeared (GDBusConnection *connection,
                  const char      *name,
                  const char      *name_owner,
                  gpointer         user_data)
{
  GTask *task = user_data;
  GrdDaemon *daemon = g_task_get_source_object (task);

  grd_dbus_remote_desktop_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                             G_DBUS_PROXY_FLAGS_NONE,
                                             MUTTER_REMOTE_DESKTOP_BUS_NAME,
                                             MUTTER_REMOTE_DESKTOP_OBJECT_PATH,
                                             g_task_get_cancellable (task),
                                             on_proxy_acquired, g_object_ref (task));
  g_bus_unwatch_name (daemon->watch_name_id);
  daemon->watch_name_id = 0;
}

static void
grd_daemon_async_initable_init_async (GAsyncInitable      *initable,
                                      int                  io_priority,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
  GrdDaemon *daemon = (GRD_DAEMON (initable));
  GTask *task;

  task = g_task_new (daemon, cancellable, callback, user_data);
  daemon->watch_name_id = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                            MUTTER_REMOTE_DESKTOP_BUS_NAME,
                                            G_BUS_NAME_WATCHER_FLAGS_NONE,
                                            on_name_appeared,
                                            NULL,
                                            task, g_object_unref);
}

static gboolean
grd_daemon_async_initable_init_finish (GAsyncInitable    *initable,
                                       GAsyncResult      *result,
                                       GError           **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
grd_daemon_async_initable_init (GAsyncInitableIface *iface)
{
  iface->init_async = grd_daemon_async_initable_init_async;
  iface->init_finish = grd_daemon_async_initable_init_finish;
}

static void
grd_daemon_async_initable_ready (GObject *source_object,
                                 GAsyncResult *result,
                                 gpointer user_data)
{
  GrdDaemon *daemon = user_data;
  GError *error = NULL;

  if (!g_async_initable_new_finish (G_ASYNC_INITABLE (source_object),
                                    result,
                                    &error))
    {
      g_warning ("Failed to initialize remote desktop daemon: %s\n", error->message);
      return;
    }

  daemon->vnc_server = grd_vnc_server_new (daemon->context);
  if (!grd_vnc_server_start (daemon->vnc_server, &error))
    {
      g_warning ("Failed to initialize VNC server: %s\n", error->message);
    }
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

  daemon->cancellable = g_cancellable_new ();
  g_async_initable_init_async (G_ASYNC_INITABLE (daemon),
                               G_PRIORITY_DEFAULT,
                               daemon->cancellable,
                               grd_daemon_async_initable_ready,
                               daemon);

  /* Run indefinitely, until told to exit. */
  g_application_hold (app);

  G_APPLICATION_CLASS (grd_daemon_parent_class)->startup (app);
}

static void
grd_daemon_class_init (GrdDaemonClass *klass)
{
  GApplicationClass *g_application_class = G_APPLICATION_CLASS (klass);

  g_application_class->startup = grd_daemon_startup;
}

static void
activate_toggle_record (GAction   *action,
                        GVariant  *parameter,
                        GrdDaemon *daemon)
{
  GVariant *state;
  gboolean b;

  state = g_action_get_state (action);
  b = g_variant_get_boolean (state);
  g_variant_unref (state);
  g_action_change_state (action, g_variant_new_boolean (!b));

  if (!b)
    {
      g_assert (!daemon->dummy_session);

      g_print ("Start dummy remote desktop\n");

      daemon->dummy_session = g_object_new (GRD_TYPE_SESSION_DUMMY,
                                            "context", daemon->context,
                                            NULL);
    }
  else
    {
      g_assert (daemon->dummy_session);

      g_print ("Stop dummy remote desktop\n");

      grd_session_stop (GRD_SESSION (daemon->dummy_session));
      g_clear_object (&daemon->dummy_session);
    }
}

static void
add_actions (GApplication *app)
{
  g_autoptr(GSimpleAction) action;

  action = g_simple_action_new_stateful ("toggle-record", NULL,
                                         g_variant_new_boolean (FALSE));
  g_signal_connect (action, "activate", G_CALLBACK (activate_toggle_record), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (action));
}

int
main (int argc, char **argv)
{
  g_autoptr(GApplication) app = NULL;

  app = g_object_new (GRD_TYPE_DAEMON,
                      "application-id", GRD_DAEMON_APPLICATION_ID,
                      "flags", G_APPLICATION_IS_SERVICE,
                      NULL);

  add_actions (app);

  return g_application_run (app, argc, argv);
}
