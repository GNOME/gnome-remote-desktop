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

#include "grd-daemon.h"
#include "grd-private.h"
#include "grd-types.h"

struct _GrdDaemonUser
{
  GrdDaemon parent;
};

G_DEFINE_TYPE (GrdDaemonUser, grd_daemon_user, GRD_TYPE_DAEMON)

GrdDaemonUser *
grd_daemon_user_new (GrdRuntimeMode   runtime_mode,
                     GError         **error)
{
  GrdContext *context;
  GrdDaemonUser *daemon;

  context = grd_context_new (runtime_mode, error);
  if (!context)
    return NULL;

  daemon = g_object_new (GRD_TYPE_DAEMON_USER,
                         "application-id", GRD_DAEMON_USER_APPLICATION_ID,
                         "flags", G_APPLICATION_IS_SERVICE,
                         "context", context,
                         NULL);

  return daemon;
}

static void
grd_daemon_user_init (GrdDaemonUser *daemon)
{
}

static void
grd_daemon_user_startup (GApplication *app)
{
  GrdDaemon *daemon = GRD_DAEMON (app);
  g_autoptr (GDBusConnection) connection = NULL;

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  grd_daemon_acquire_mutter_dbus_proxies (daemon, connection);

  g_signal_connect (daemon, "mutter-proxy-acquired",
                    G_CALLBACK (grd_daemon_maybe_enable_services), NULL);

  G_APPLICATION_CLASS (grd_daemon_user_parent_class)->startup (app);
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

  daemon_class->is_daemon_ready = grd_daemon_user_is_ready;
}
