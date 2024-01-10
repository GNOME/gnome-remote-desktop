/*
 * Copyright (C) 2022 Red Hat Inc.
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

#include "grd-daemon-user.h"

#include <glib/gi18n.h>

#include "grd-private.h"
#include "grd-prompt.h"

struct _GrdDaemonUser
{
  GrdDaemon parent;

  GrdDBusRemoteDesktopRdpServer *system_rdp_server;

  GrdPrompt *prompt;
  GCancellable *prompt_cancellable;
};

G_DEFINE_TYPE (GrdDaemonUser, grd_daemon_user, GRD_TYPE_DAEMON)

static void
prompt_response_callback (GObject      *source_object,
                          GAsyncResult *async_result,
                          gpointer      user_data)
{
  GrdDaemonUser *daemon_user = GRD_DAEMON_USER (user_data);
  g_autoptr (GError) error = NULL;
  GrdPromptResponse response;

  if (!grd_prompt_query_finish (daemon_user->prompt,
                                async_result,
                                &response,
                                &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("Failed to query user about port change: %s",
                     error->message);
        }
    }

  g_clear_object (&daemon_user->prompt);
  g_clear_object (&daemon_user->prompt_cancellable);
}

static void
show_port_changed_prompt (GrdDaemonUser *daemon_user,
                          const int      system_rdp_port,
                          const int      selected_rdp_port)
{
  g_autoptr (GrdPromptDefinition) prompt_definition = NULL;

  if (daemon_user->prompt)
    return;

  daemon_user->prompt = g_object_new (GRD_TYPE_PROMPT, NULL);
  daemon_user->prompt_cancellable = g_cancellable_new ();

  prompt_definition = g_new0 (GrdPromptDefinition, 1);
  prompt_definition->summary =
    g_strdup_printf (_("Desktop Sharing port changed"));
  prompt_definition->body =
    g_strdup_printf (_("The Desktop Sharing port has changed from <b>%i</b> to "
                       "<b>%i</b>.\n"
                       "This is because the Remote Sessions service has been "
                       "activated and it has a higher priority to use port "
                       "<b>%i</b>."),
                       system_rdp_port, selected_rdp_port, system_rdp_port);
  prompt_definition->accept_label = g_strdup_printf (_("Accept"));

  grd_prompt_query_async (daemon_user->prompt,
                          prompt_definition,
                          daemon_user->prompt_cancellable,
                          prompt_response_callback,
                          daemon_user);
}

static void
on_rdp_server_started (GrdDaemonUser *daemon_user)
{
  GrdContext *context = grd_daemon_get_context (GRD_DAEMON (daemon_user));
  GrdDBusRemoteDesktopRdpServer *rdp_server_iface =
    grd_context_get_rdp_server_interface (context);
  int selected_rdp_port =
    grd_dbus_remote_desktop_rdp_server_get_port (rdp_server_iface);
  int system_rdp_port =
    grd_dbus_remote_desktop_rdp_server_get_port (daemon_user->system_rdp_server);

  g_assert (selected_rdp_port != system_rdp_port);

  show_port_changed_prompt (daemon_user, system_rdp_port, selected_rdp_port);

  g_signal_handlers_disconnect_by_func (daemon_user,
                                        G_CALLBACK (on_rdp_server_started),
                                        NULL);
}

static void
on_system_rdp_server_binding (GrdDBusRemoteDesktopRdpServer *system_rdp_server,
                              int                            system_rdp_port,
                              GrdDaemonUser                 *daemon_user)
{
  GrdContext *context = grd_daemon_get_context (GRD_DAEMON (daemon_user));
  GrdDBusRemoteDesktopRdpServer *user_rdp_server =
    grd_context_get_rdp_server_interface (context);
  int user_rdp_port =
    grd_dbus_remote_desktop_rdp_server_get_port (user_rdp_server);

  if (system_rdp_port != user_rdp_port)
    return;

  g_debug ("[DaemonUser] Restarting RDP server due to port conflict with the "
           "system daemon");

  grd_daemon_restart_rdp_server_with_delay (GRD_DAEMON (daemon_user));

  g_signal_connect (daemon_user, "rdp-server-started",
                    G_CALLBACK (on_rdp_server_started), NULL);
}

static void
on_remote_desktop_rdp_server_proxy_acquired (GObject      *object,
                                             GAsyncResult *result,
                                             gpointer      user_data)
{
  GrdDaemonUser *daemon_user = user_data;
  g_autoptr (GError) error = NULL;

  daemon_user->system_rdp_server =
    grd_dbus_remote_desktop_rdp_server_proxy_new_for_bus_finish (result, &error);
  if (!daemon_user->system_rdp_server)
    {
      g_warning ("[DaemonUser] Failed to create system rdp server proxy: "
                 "%s", error->message);
      return;
    }

  g_signal_connect (daemon_user->system_rdp_server, "binding",
                    G_CALLBACK (on_system_rdp_server_binding),
                    daemon_user);
}

GrdDaemonUser *
grd_daemon_user_new (GrdRuntimeMode   runtime_mode,
                     GError         **error)
{
  GrdContext *context;
  GrdDaemonUser *daemon_user;
  const char *application_id = NULL;

  context = grd_context_new (runtime_mode, error);
  if (!context)
    return NULL;

  switch (runtime_mode)
    {
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
      application_id = GRD_DAEMON_USER_APPLICATION_ID;
      break;
    case GRD_RUNTIME_MODE_HEADLESS:
      application_id = GRD_DAEMON_HEADLESS_APPLICATION_ID;
      break;
    case GRD_RUNTIME_MODE_SYSTEM:
    case GRD_RUNTIME_MODE_HANDOVER:
      g_assert_not_reached ();
    }

  daemon_user = g_object_new (GRD_TYPE_DAEMON_USER,
                              "application-id", application_id,
                              "flags", G_APPLICATION_IS_SERVICE,
                              "context", context,
                              NULL);

  return daemon_user;
}

static void
grd_daemon_user_init (GrdDaemonUser *daemon_user)
{
}

static void
grd_daemon_user_startup (GApplication *app)
{
  GrdDaemonUser *daemon_user = GRD_DAEMON_USER (app);
  GCancellable *cancellable =
    grd_daemon_get_cancellable (GRD_DAEMON (daemon_user));

  grd_daemon_acquire_mutter_dbus_proxies (GRD_DAEMON (daemon_user));

  g_signal_connect (daemon_user, "mutter-proxy-acquired",
                    G_CALLBACK (grd_daemon_maybe_enable_services), NULL);

  grd_dbus_remote_desktop_rdp_server_proxy_new_for_bus (
    G_BUS_TYPE_SYSTEM,
    G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
    REMOTE_DESKTOP_BUS_NAME,
    GRD_RDP_SERVER_OBJECT_PATH,
    cancellable,
    on_remote_desktop_rdp_server_proxy_acquired,
    daemon_user);

  G_APPLICATION_CLASS (grd_daemon_user_parent_class)->startup (app);
}

static void
grd_daemon_user_shutdown (GApplication *app)
{
  GrdDaemonUser *daemon_user = GRD_DAEMON_USER (app);

  g_clear_object (&daemon_user->system_rdp_server);
  if (daemon_user->prompt_cancellable)
    {
      g_cancellable_cancel (daemon_user->prompt_cancellable);
      g_clear_object (&daemon_user->prompt_cancellable);
    }
  g_clear_object (&daemon_user->prompt);

  G_APPLICATION_CLASS (grd_daemon_user_parent_class)->shutdown (app);
}

static gboolean
grd_daemon_user_is_ready (GrdDaemon *daemon)
{
  GrdContext *context = grd_daemon_get_context (daemon);

  if (!grd_context_get_mutter_remote_desktop_proxy (context) ||
      !grd_context_get_mutter_screen_cast_proxy (context))
    return FALSE;

  return TRUE;
}

static void
grd_daemon_user_class_init (GrdDaemonUserClass *klass)
{
  GApplicationClass *g_application_class = G_APPLICATION_CLASS (klass);
  GrdDaemonClass *daemon_class = GRD_DAEMON_CLASS (klass);

  g_application_class->startup = grd_daemon_user_startup;
  g_application_class->shutdown = grd_daemon_user_shutdown;

  daemon_class->is_daemon_ready = grd_daemon_user_is_ready;
}
