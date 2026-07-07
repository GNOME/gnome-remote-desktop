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

#include "grd-pcscd-session.h"

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib/gstdio.h>

#include "grd-dbus-pcscd.h"

struct _GrdPcscdSession
{
  GObject parent;

  char *session_id;

  GCancellable *cancellable;

  GrdDBusPcscdSession *private_proxy;
};

enum
{
  CLOSED,

  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE (GrdPcscdSession, grd_pcscd_session, G_TYPE_OBJECT)

static void
on_private_connection_closed (GDBusConnection *connection,
                              gboolean         remote_peer_vanished,
                              GError          *error,
                              gpointer         user_data)
{
  GrdPcscdSession *session = user_data;

  g_debug ("[PCSCD.SESSION %s] Private connection closed", session->session_id);

  g_signal_emit (session, signals[CLOSED], 0);
}

static void
on_private_proxy_ready (GObject      *source_object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  g_autoptr (GrdDBusPcscdSession) private_proxy = NULL;
  g_autoptr (GError) error = NULL;
  GrdPcscdSession *session = user_data;
  GDBusConnection *private_connection;

  private_proxy = grd_dbus_pcscd_session_proxy_new_finish (result, &error);
  if (!private_proxy)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("[PCSCD.SESSION %s] Failed to create private proxy: %s",
                     session->session_id, error->message);
          g_signal_emit (session, signals[CLOSED], 0);
        }
      return;
    }

  private_connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (private_proxy));
  g_signal_connect_object (private_connection, "closed",
                           G_CALLBACK (on_private_connection_closed),
                           session, G_CONNECT_DEFAULT);

  session->private_proxy = g_steal_pointer (&private_proxy);

  g_debug ("[PCSCD.SESSION %s] Private D-Bus connection established",
           session->session_id);
}

static void
on_private_connection_ready (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  GrdPcscdSession *session = user_data;
  g_autoptr (GDBusConnection) private_connection = NULL;
  g_autoptr (GError) error = NULL;

  private_connection = g_dbus_connection_new_finish (result, &error);
  if (!private_connection)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("[PCSCD.SESSION %s] Failed to create private connection: %s",
                     session->session_id, error->message);
          g_signal_emit (session, signals[CLOSED], 0);
        }
      return;
    }

  grd_dbus_pcscd_session_proxy_new (private_connection,
                                    G_DBUS_PROXY_FLAGS_NONE,
                                    NULL,
                                    "/",
                                    session->cancellable,
                                    on_private_proxy_ready,
                                    session);
}

static void
create_private_proxy (GrdPcscdSession *session,
                      int              fd)
{
  g_autoptr (GInputStream) input_stream = NULL;
  g_autoptr (GOutputStream) output_stream = NULL;
  g_autoptr (GIOStream) io_stream = NULL;
  g_autofree char *guid = NULL;

  /* fd ownership: fd -> input_stream -> io_stream -> private_connection -> proxy */
  input_stream = g_unix_input_stream_new (fd, TRUE);
  output_stream = g_unix_output_stream_new (fd, FALSE);
  io_stream = g_simple_io_stream_new (input_stream, output_stream);

  guid = g_dbus_generate_guid ();
  g_dbus_connection_new (io_stream,
                         guid,
                         G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER |
                         G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_ALLOW_ANONYMOUS,
                         NULL,
                         session->cancellable,
                         on_private_connection_ready,
                         session);
}

const char *
grd_pcscd_session_get_session_id (GrdPcscdSession *session)
{
  return session->session_id;
}

GrdPcscdSession *
grd_pcscd_session_new (const char      *session_id,
                       int              fd,
                       GDBusConnection *system_connection)
{
  g_autoptr (GrdPcscdSession) session = NULL;

  session = g_object_new (GRD_TYPE_PCSCD_SESSION, NULL);
  session->session_id = g_strdup (session_id);
  session->cancellable = g_cancellable_new ();

  create_private_proxy (session, fd);

  return g_steal_pointer (&session);
}

static void
grd_pcscd_session_dispose (GObject *object)
{
  GrdPcscdSession *session = GRD_PCSCD_SESSION (object);

  g_cancellable_cancel (session->cancellable);
  g_clear_object (&session->cancellable);

  if (session->private_proxy)
    {
      GDBusConnection *private_connection =
        g_dbus_proxy_get_connection (G_DBUS_PROXY (session->private_proxy));

      if (!g_dbus_connection_is_closed (private_connection))
        g_dbus_connection_close (private_connection, NULL, NULL, NULL);

      g_clear_object (&session->private_proxy);
    }

  g_clear_pointer (&session->session_id, g_free);

  G_OBJECT_CLASS (grd_pcscd_session_parent_class)->dispose (object);
}

static void
grd_pcscd_session_init (GrdPcscdSession *session)
{
}

static void
grd_pcscd_session_class_init (GrdPcscdSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_pcscd_session_dispose;

  signals[CLOSED] =
    g_signal_new ("closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}
