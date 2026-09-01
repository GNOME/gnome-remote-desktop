/*
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
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

#include "grd-daemon-utils.h"

#include <glib/gstdio.h>
#include <systemd/sd-login.h>

enum
{
  GSM_LOGOUT_MODE_NORMAL = 0,
  GSM_LOGOUT_MODE_NO_CONFIRMATION,
  GSM_LOGOUT_MODE_FORCE,
};

gboolean
grd_get_pidfd_of_sender_sync (GDBusConnection  *connection,
                              const char       *name,
                              int              *out_pidfd,
                              GCancellable     *cancellable,
                              GError          **error)
{
  g_autoptr (GUnixFDList) fd_list = NULL;
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GVariant) credentials = NULL;
  int pidfd_idx;
  g_autofd int pidfd = -1;

  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  g_assert (out_pidfd);

  result =
    g_dbus_connection_call_with_unix_fd_list_sync (connection,
                                                   "org.freedesktop.DBus",
                                                   "/org/freedesktop/DBus",
                                                   "org.freedesktop.DBus",
                                                   "GetConnectionCredentials",
                                                   g_variant_new ("(s)", name),
                                                   G_VARIANT_TYPE ("(a{sv})"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   -1,
                                                   NULL,
                                                   &fd_list,
                                                   cancellable, error);
  if (!result)
    return FALSE;

  g_variant_get (result, "(@a{sv})", &credentials);
  if (!g_variant_lookup (credentials, "ProcessFD", "h", &pidfd_idx))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "D-Bus broker doesn't support pidfd");
      return FALSE;
    }

  if (!fd_list)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No file descriptors passed");
      return FALSE;
    }

  if (pidfd_idx < 0 || pidfd_idx >= g_unix_fd_list_get_length (fd_list))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid file descriptor idx");
      return FALSE;
    }

  pidfd = g_unix_fd_list_get (fd_list, pidfd_idx, error);
  if (pidfd == -1)
    return FALSE;

  *out_pidfd = g_steal_fd (&pidfd);

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

  return session_id;
}

char *
grd_get_session_id_from_pidfd (int pidfd)
{
  char *session_id = NULL;
  int res;

  res = sd_pidfd_get_session (pidfd, &session_id);
  if (res < 0 && res != -ENODATA)
    {
      g_warning ("Failed to retrieve session information for "
                 "pidfd %d: %s", pidfd, strerror (-res));
    }

  return session_id;
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

  for (i = n_sessions - 1; i >= 0; i--)
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

char *
grd_get_session_id_of_sender (GDBusConnection  *connection,
                              const char       *name,
                              GCancellable     *cancellable,
                              GError          **error)
{
  char *session_id = NULL;
  gboolean success;
  g_autofd int pidfd = -1;
  uid_t uid = 0;

  success = grd_get_pidfd_of_sender_sync (connection,
                                          name,
                                          &pidfd,
                                          cancellable,
                                          error);
  if (!success)
    return NULL;

  session_id = grd_get_session_id_from_pidfd (pidfd);
  if (session_id)
    return session_id;

  success = grd_get_uid_of_sender_sync (connection,
                                        name,
                                        &uid,
                                        cancellable,
                                        error);
  if (!success)
    return NULL;

  session_id = grd_get_session_id_from_uid (uid);
  if (!session_id)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_NOT_FOUND,
                   "Could not find a session for user %d",
                   (int) uid);
    }

  return session_id;
}

gboolean
grd_is_remote_login (void)
{
  g_autofree char *session_id = NULL;
  g_autofree char *class = NULL;
  pid_t pid;
  uid_t uid;
  int ret;

  pid = getpid ();
  session_id = grd_get_session_id_from_pid (pid);
  if (!session_id)
    {
      uid = getuid ();
      session_id = grd_get_session_id_from_uid (uid);
    }

  if (!session_id)
    {
      g_warning ("Couldn't get session id");
      return FALSE;
    }

  ret = sd_session_get_class (session_id, &class);
  if (ret < 0)
    {
      g_warning ("Couldn't determine session class: %s", g_strerror (-ret));
      return FALSE;
    }

  return g_strcmp0 (class, "greeter") == 0 &&
         sd_session_is_remote (session_id) == 1;
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
