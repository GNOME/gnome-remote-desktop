/*
 * Copyright (C) 2010 Intel Corp.
 * Copyright (C) 2014 Jonas Ådahl
 * Copyright (C) 2016-2022 Red Hat Inc.
 * Copyright (C) 2022 Pascal Nowack
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
 */

#include "config.h"

#include "grd-utils.h"

#include <systemd/sd-login.h>

typedef struct _GrdFdSource
{
  GSource source;

  GSourceFunc prepare;
  GSourceFunc dispatch;
  gpointer user_data;

  GPollFD poll_fd;
} GrdFdSource;

enum
{
  GSM_LOGOUT_MODE_NORMAL = 0,
  GSM_LOGOUT_MODE_NO_CONFIRMATION,
  GSM_LOGOUT_MODE_FORCE,
};

void
grd_sync_point_init (GrdSyncPoint *sync_point)
{
  g_cond_init (&sync_point->sync_cond);
  g_mutex_init (&sync_point->sync_mutex);
}

void
grd_sync_point_clear (GrdSyncPoint *sync_point)
{
  g_cond_clear (&sync_point->sync_cond);
  g_mutex_clear (&sync_point->sync_mutex);
}

void
grd_sync_point_complete (GrdSyncPoint *sync_point,
                         gboolean      success)
{
  g_return_if_fail (!sync_point->completed);

  g_mutex_lock (&sync_point->sync_mutex);
  sync_point->success = success;

  sync_point->completed = TRUE;
  g_cond_signal (&sync_point->sync_cond);
  g_mutex_unlock (&sync_point->sync_mutex);
}

gboolean
grd_sync_point_wait_for_completion (GrdSyncPoint *sync_point)
{
  gboolean success;

  g_mutex_lock (&sync_point->sync_mutex);
  while (!sync_point->completed)
    g_cond_wait (&sync_point->sync_cond, &sync_point->sync_mutex);

  success = sync_point->success;
  g_mutex_unlock (&sync_point->sync_mutex);

  return success;
}

static gboolean
grd_fd_source_prepare (GSource *source,
                       int     *timeout_ms)
{
  GrdFdSource *fd_source = (GrdFdSource *) source;

  *timeout_ms = -1;

  return fd_source->prepare (fd_source->user_data);
}

static gboolean
grd_fd_source_check (GSource *source)
{
  GrdFdSource *fd_source = (GrdFdSource *) source;

  return !!(fd_source->poll_fd.revents & G_IO_IN);
}

static gboolean
grd_fd_source_dispatch (GSource     *source,
                        GSourceFunc  callback,
                        gpointer     user_data)
{
  GrdFdSource *fd_source = (GrdFdSource *) source;

  return fd_source->dispatch (fd_source->user_data);
}

static void
grd_fd_source_finalize (GSource *source)
{
  GrdFdSource *fd_source = (GrdFdSource *) source;

  close (fd_source->poll_fd.fd);
}

static GSourceFuncs fd_source_funcs =
{
  .prepare = grd_fd_source_prepare,
  .check = grd_fd_source_check,
  .dispatch = grd_fd_source_dispatch,
  .finalize = grd_fd_source_finalize,
};

GSource *
grd_create_fd_source (int             fd,
                      const char     *name,
                      GSourceFunc     prepare,
                      GSourceFunc     dispatch,
                      gpointer        user_data,
                      GDestroyNotify  notify)
{
  GSource *source;
  GrdFdSource *fd_source;

  source = g_source_new (&fd_source_funcs, sizeof (GrdFdSource));
  g_source_set_name (source, name);
  fd_source = (GrdFdSource *) source;

  fd_source->poll_fd.fd = fd;
  fd_source->poll_fd.events = G_IO_IN;

  fd_source->prepare = prepare;
  fd_source->dispatch = dispatch;
  fd_source->user_data = user_data;

  g_source_set_callback (source, dispatch, user_data, notify);
  g_source_add_poll (source, &fd_source->poll_fd);

  return source;
}

gboolean
grd_get_pid_of_sender_sync (GDBusConnection  *connection,
                            const char       *name,
                            pid_t            *out_pid,
                            GCancellable     *cancellable,
                            GError          **error)
{
  g_autoptr (GVariant) result = NULL;
  uint32_t pid;

  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  g_assert (out_pid);

  result = g_dbus_connection_call_sync (connection,
                                        "org.freedesktop.DBus",
                                        "/org/freedesktop/DBus",
                                        "org.freedesktop.DBus",
                                        "GetConnectionUnixProcessID",
                                        g_variant_new ("(s)", name),
                                        G_VARIANT_TYPE ("(u)"),
                                        G_DBUS_CALL_FLAGS_NONE,
                                        -1,
                                        cancellable, error);
  if (!result)
    return FALSE;

  g_variant_get (result, "(u)", &pid);

  *out_pid = (pid_t) pid;

  return TRUE;
}

gboolean
grd_get_uid_of_sender_sync (GDBusConnection  *connection,
                            const char       *name,
                            uid_t            *out_uid,
                            GCancellable     *cancellable,
                            GError          **error)
{
  g_autoptr (GVariant) result = NULL;
  uint32_t uid;

  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  g_assert (out_uid);

  result = g_dbus_connection_call_sync (connection,
                                        "org.freedesktop.DBus",
                                        "/org/freedesktop/DBus",
                                        "org.freedesktop.DBus",
                                        "GetConnectionUnixUser",
                                        g_variant_new ("(s)", name),
                                        G_VARIANT_TYPE ("(u)"),
                                        G_DBUS_CALL_FLAGS_NONE,
                                        -1,
                                        cancellable, error);
  if (!result)
    return FALSE;

  g_variant_get (result, "(u)", &uid);

  *out_uid = (uid_t) uid;

  return TRUE;
}

char *
grd_get_session_id_from_pid (pid_t pid)
{
  char *session_id = NULL;
  int res;

  res = sd_pid_get_session (pid, &session_id);
  if (res < 0 && res != -ENODATA)
    {
      g_warning ("Failed to retrieve session information for "
                 "pid %d: %s", (int) pid, strerror (-res));
    }

  return g_steal_pointer (&session_id);
}

static gboolean
grd_sd_session_is_graphical (const char *session_id)
{
  const char * const graphical_session_types[] = { "wayland", "x11",  NULL };
  int res;
  g_autofree char *type = NULL;

  res = sd_session_get_type (session_id, &type);
  if (res < 0)
    return FALSE;

  return g_strv_contains (graphical_session_types, type);
}

static gboolean
grd_sd_session_is_active (const char *session_id)
{
  const char * const active_states[] = { "active", "online", NULL };
  int res;
  g_autofree char *state = NULL;

  res = sd_session_get_state (session_id, &state);
  if (res < 0)
    return FALSE;

  return g_strv_contains (active_states, state);
}

char *
grd_get_session_id_from_uid (uid_t uid)
{
  g_auto (GStrv) sessions = NULL;
  char *session_id = NULL;
  int n_sessions;
  int i;

  n_sessions = sd_uid_get_sessions (uid, 0, &sessions);

  for (i = n_sessions; i >= 0; i--)
    {
      if (!grd_sd_session_is_graphical (sessions[i]))
        continue;

      if (!grd_sd_session_is_active (sessions[i]))
        continue;

      session_id = sessions[i];
      break;
    }

  return g_strdup (session_id);
}

void
grd_session_manager_call_logout_sync (void)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!connection)
    {
      g_warning ("Couldn't get session bus to logout: %s", error->message);
      return;
    }

  g_dbus_connection_call_sync (connection,
                               "org.gnome.SessionManager",
                               "/org/gnome/SessionManager",
                               "org.gnome.SessionManager",
                               "Logout",
                               g_variant_new ("(u)",
                               GSM_LOGOUT_MODE_NO_CONFIRMATION),
                               NULL,
                               G_DBUS_CALL_FLAGS_NONE,
                               -1,
                               NULL, &error);
  if (error)
    g_warning ("Couldn't logout of session: %s", error->message);
}

gboolean
grd_binding_set_target_if_source_greater_than_zero (GBinding     *binding,
                                                    const GValue *source_value,
                                                    GValue       *target_value,
                                                    gpointer      user_data)
{
  int source_int = g_value_get_int (source_value);
  g_value_set_boolean (target_value, source_int >= 0);
  return TRUE;
}
