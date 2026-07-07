/*
 * Copyright (C) 2026 Red Hat Inc.
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
 *     Joan Torres Lopez <joantolo@redhat.com>
 */

#include "config.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <stdint.h>

#include "grd-daemon-utils.h"
#include "grd-dbus-pcscd.h"
#include "grd-pcscd-session.h"

#define PCSCD_BUS_NAME "org.gnome.RemoteDesktop.Pcscd"
#define PCSCD_OBJECT_PATH "/org/gnome/RemoteDesktop/Pcscd"

typedef struct _GrdPcscd
{
  GMainLoop *loop;
  guint sigint_source_id;
  guint sigterm_source_id;

  guint own_name_id;
  GrdDBusPcscd *skeleton;
  GDBusConnection *connection;

  GHashTable *sessions;
} GrdPcscd;

static void
grd_pcscd_free (GrdPcscd *pcscd)
{
  g_clear_pointer (&pcscd->sessions, g_hash_table_unref);

  g_clear_handle_id (&pcscd->sigint_source_id, g_source_remove);
  g_clear_handle_id (&pcscd->sigterm_source_id, g_source_remove);

  g_clear_handle_id (&pcscd->own_name_id, g_bus_unown_name);

  if (pcscd->skeleton)
    {
      g_dbus_interface_skeleton_unexport (
        G_DBUS_INTERFACE_SKELETON (pcscd->skeleton));
      g_clear_object (&pcscd->skeleton);
    }

  g_clear_object (&pcscd->connection);
  g_clear_pointer (&pcscd->loop, g_main_loop_unref);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (GrdPcscd, grd_pcscd_free)

static void
on_session_closed (GrdPcscdSession *session,
                   gpointer         user_data)
{
  GrdPcscd *pcscd = user_data;
  const char *session_id;

  session_id = grd_pcscd_session_get_session_id (session);

  g_message ("Closing smartcard session %s", session_id);

  g_hash_table_remove (pcscd->sessions, session_id);
}

static gboolean
on_handle_connect (GrdDBusPcscd          *skeleton,
                   GDBusMethodInvocation *invocation,
                   GUnixFDList           *fd_list,
                   GVariant              *arg_rdp_fd,
                   gpointer               user_data)
{
  g_autofree char *session_id = NULL;
  g_autoptr (GError) error = NULL;
  g_autofd int fd = -1;
  GrdPcscd *pcscd = user_data;
  GDBusConnection *connection;
  GrdPcscdSession *session;
  const char *sender;
  int fd_idx;

  if (!G_IS_UNIX_FD_LIST (fd_list))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "No fd list received");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  fd_idx = g_variant_get_handle (arg_rdp_fd);
  fd = g_unix_fd_list_get (fd_list, fd_idx, &error);
  if (fd < 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Failed to get fd: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  connection = g_dbus_method_invocation_get_connection (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);

  session_id = grd_get_session_id_of_sender (connection, sender, NULL, &error);
  if (!session_id)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed to get session of "
                                             "sender: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (g_hash_table_contains (pcscd->sessions, session_id))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Session %s already exists",
                                             session_id);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = grd_pcscd_session_new (session_id,
                                   g_steal_fd (&fd),
                                   pcscd->connection);

  g_signal_connect (session, "closed",
                    G_CALLBACK (on_session_closed),
                    pcscd);

  g_hash_table_insert (pcscd->sessions, g_strdup (session_id), session);

  g_message ("New smartcard session %s", session_id);

  grd_dbus_pcscd_complete_connect (skeleton, invocation, NULL);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  g_autoptr (GrdDBusPcscd) skeleton = NULL;
  g_autoptr (GError) error = NULL;
  GrdPcscd *pcscd = user_data;

  skeleton = grd_dbus_pcscd_skeleton_new ();
  g_signal_connect (skeleton, "handle-connect",
                    G_CALLBACK (on_handle_connect),
                    pcscd);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (skeleton),
                                         connection,
                                         PCSCD_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Failed to export Pcscd interface: %s", error->message);
      g_main_loop_quit (pcscd->loop);
      return;
    }

  g_set_object (&pcscd->skeleton, skeleton);
  g_set_object (&pcscd->connection, connection);
}

static void
on_name_lost (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  GrdPcscd *pcscd = user_data;

  g_warning ("Lost bus name %s", name);

  g_main_loop_quit (pcscd->loop);
}

static gboolean
on_signal_quit (gpointer user_data)
{
  GrdPcscd *pcscd = user_data;

  g_message ("Received signal, shutting down");

  g_clear_handle_id (&pcscd->sigint_source_id, g_source_remove);
  g_clear_handle_id (&pcscd->sigterm_source_id, g_source_remove);

  g_main_loop_quit (pcscd->loop);

  return G_SOURCE_REMOVE;
}

int
main (int    argc,
      char **argv)
{
  g_auto (GrdPcscd) pcscd = {};

  pcscd.loop = g_main_loop_new (NULL, FALSE);
  pcscd.sessions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, g_object_unref);

  pcscd.own_name_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                                      PCSCD_BUS_NAME,
                                      G_BUS_NAME_OWNER_FLAGS_NONE,
                                      on_bus_acquired,
                                      NULL,
                                      on_name_lost,
                                      &pcscd,
                                      NULL);

  pcscd.sigint_source_id = g_unix_signal_add (SIGINT, on_signal_quit, &pcscd);
  pcscd.sigterm_source_id = g_unix_signal_add (SIGTERM, on_signal_quit, &pcscd);

  g_main_loop_run (pcscd.loop);

  return EXIT_SUCCESS;
}
