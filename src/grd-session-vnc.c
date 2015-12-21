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

#include "grd-session-vnc.h"

#include <gio/gio.h>
#include <rfb/rfb.h>

#include "grd-vnc-server.h"

struct _GrdSessionVnc
{
  GrdSession parent;

  GSocketConnection *connection;
  GSource *source;
  rfbScreenInfoPtr rfb_screen;
};

G_DEFINE_TYPE (GrdSessionVnc, grd_session_vnc, GRD_TYPE_SESSION);

static void
handle_client_gone (rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;

  grd_session_stop (GRD_SESSION (session_vnc));
}

static enum rfbNewClientAction
handle_new_client (rfbClientPtr rfb_client)
{
  rfb_client->clientGoneHook = handle_client_gone;

  return RFB_CLIENT_ACCEPT;
}

static void
init_vnc_session (GrdSessionVnc *session_vnc)
{
  GSocket *socket;
  int screen_width;
  int screen_height;
  rfbScreenInfoPtr rfb_screen;

  /* Arbitrary framebuffer size, will get the porper size from the stream. */
  screen_width = 800;
  screen_height = 600;
  rfb_screen = rfbGetScreen (0, NULL,
                             screen_width, screen_height,
                             8, 3, 4);

  socket = g_socket_connection_get_socket (session_vnc->connection);
  rfb_screen->inetdSock = g_socket_get_fd (socket);
  rfb_screen->desktopName = "GNOME Remote Desktop (VNC)";
  rfb_screen->neverShared = TRUE;
  rfb_screen->newClientHook = handle_new_client;
  rfb_screen->screenData = session_vnc;

  rfb_screen->frameBuffer = g_malloc0 (screen_width * screen_height * 4);
  memset (rfb_screen->frameBuffer, 0x1f, screen_width * screen_height * 4);

  rfbInitServer (rfb_screen);
  rfbProcessEvents (rfb_screen, 0);

  session_vnc->rfb_screen = rfb_screen;
}

static gboolean
handle_socket_data (GSocket *socket,
                    GIOCondition condition,
                    gpointer user_data)
{
  GrdSessionVnc *session_vnc = user_data;

  switch (condition)
    {
    case G_IO_IN:
      if (rfbIsActive (session_vnc->rfb_screen))
        rfbProcessEvents (session_vnc->rfb_screen, 0);
      break;
    default:
      g_warning ("Unhandled socket condition %d\n", condition);
    }

  return TRUE;
}

GrdSessionVnc *
grd_session_vnc_new (GrdVncServer      *vnc_server,
                     GSocketConnection *connection)
{
  GrdSessionVnc *session_vnc;
  GrdContext *context;
  GSocket *socket;

  context = grd_vnc_server_get_context (vnc_server);
  session_vnc = g_object_new (GRD_TYPE_SESSION_VNC,
                              "context", context,
                              NULL);

  socket = g_socket_connection_get_socket (connection);
  session_vnc->connection = g_object_ref (connection);
  session_vnc->source = g_socket_create_source (socket,
                                                G_IO_IN | G_IO_PRI,
                                                NULL);
  g_source_set_callback (session_vnc->source,
                         (GSourceFunc) handle_socket_data,
                         session_vnc, NULL);
  g_source_attach (session_vnc->source, NULL);

  init_vnc_session (session_vnc);

  return session_vnc;
}

static void
grd_session_vnc_dispose (GObject *object)
{
  g_assert (!GRD_SESSION_VNC (object)->rfb_screen);

  G_OBJECT_CLASS (grd_session_vnc_parent_class)->dispose (object);
}

static void
grd_session_vnc_stop (GrdSession *session)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (session);

  g_clear_object (&session_vnc->connection);
  g_clear_pointer (&session_vnc->source, g_source_destroy);
  g_clear_pointer (&session_vnc->rfb_screen->frameBuffer, g_free);
  g_clear_pointer (&session_vnc->rfb_screen, (GDestroyNotify) rfbScreenCleanup);
}

static void
grd_session_vnc_stream_added (GrdSession *session,
                              GrdStream  *stream)
{
}

static void
grd_session_vnc_init (GrdSessionVnc *vnc)
{
}

static void
grd_session_vnc_class_init (GrdSessionVncClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdSessionClass *session_class = GRD_SESSION_CLASS (klass);

  object_class->dispose = grd_session_vnc_dispose;

  session_class->stop = grd_session_vnc_stop;
  session_class->stream_added = grd_session_vnc_stream_added;
}
