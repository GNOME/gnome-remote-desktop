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
#include <gst/gst.h>
#include <linux/input.h>
#include <pinos/client/stream.h>
#include <rfb/rfb.h>

#include "grd-stream.h"
#include "grd-vnc-server.h"
#include "grd-vnc-sink.h"

struct _GrdSessionVnc
{
  GrdSession parent;

  GSocketConnection *connection;
  GSource *source;
  rfbScreenInfoPtr rfb_screen;
  rfbClientPtr rfb_client;

  gboolean proxy_ready;

  GstElement *pipeline;
  GstElement *vnc_sink;
  GstElement *pinos_src;

  int prev_x;
  int prev_y;
  int prev_button_mask;
  GHashTable *pressed_keys;
};

G_DEFINE_TYPE (GrdSessionVnc, grd_session_vnc, GRD_TYPE_SESSION);

static void
grd_session_vnc_detach_source (GrdSessionVnc *session_vnc);

void
grd_session_vnc_resize_framebuffer (GrdSessionVnc *session_vnc,
                                    int            width,
                                    int            height)
{
  rfbScreenInfoPtr rfb_screen = session_vnc->rfb_screen;
  uint8_t *framebuffer;

  if (rfb_screen->width == width && rfb_screen->height == height)
    return;

  g_free (session_vnc->rfb_screen->frameBuffer);
  framebuffer = g_malloc0 (width * height * 4);
  rfbNewFramebuffer (session_vnc->rfb_screen,
                     (char *) framebuffer,
                     width, height,
                     8, 3, 4);
}

static size_t
grd_session_vnc_get_framebuffer_size (GrdSessionVnc *session_vnc)
{
  return session_vnc->rfb_screen->width * session_vnc->rfb_screen->height * 4;
}

void
grd_session_vnc_draw_buffer (GrdSessionVnc *session_vnc,
                             GstBuffer     *buffer)
{
  GstMapInfo map_info = { 0 };
  size_t framebuffer_size = grd_session_vnc_get_framebuffer_size (session_vnc);

  if (gst_buffer_get_size (buffer) != framebuffer_size)
    {
      g_warning ("VNC sink buffer size %lu differs from VNC framebuffer size\n",
                 gst_buffer_get_size (buffer));
      return;
    }

  if (!gst_buffer_map (buffer, &map_info, GST_MAP_READ))
    {
      g_warning ("Failed to map VNC sink buffer\n");
      return;
    }

  g_assert (map_info.size == framebuffer_size);

  memcpy (session_vnc->rfb_screen->frameBuffer,
          map_info.data,
          framebuffer_size);

  gst_buffer_unmap (buffer, &map_info);

  rfbMarkRectAsModified (session_vnc->rfb_screen, 0, 0,
                         session_vnc->rfb_screen->width,
                         session_vnc->rfb_screen->height);
  rfbProcessEvents (session_vnc->rfb_screen, 0);
}

static void
handle_client_gone (rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;

  grd_session_stop (GRD_SESSION (session_vnc));
}

static enum rfbNewClientAction
handle_new_client (rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (rfb_client->screen->screenData);

  session_vnc->rfb_client = rfb_client;
  rfb_client->clientGoneHook = handle_client_gone;

  if (!session_vnc->proxy_ready)
    grd_session_vnc_detach_source (session_vnc);

  return RFB_CLIENT_ACCEPT;
}

static void
handle_key_event (rfbBool      down,
                  rfbKeySym    keysym,
                  rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (rfb_client->screen->screenData);
  GrdSession *session = GRD_SESSION (session_vnc);

  if (down)
    {
      if (g_hash_table_add (session_vnc->pressed_keys,
                            GUINT_TO_POINTER (keysym)))
        {
          grd_session_notify_keyboard_keysym (session, keysym,
                                              GRD_KEY_STATE_PRESSED);
        }
    }
  else
    {
      if (g_hash_table_remove (session_vnc->pressed_keys,
                               GUINT_TO_POINTER (keysym)))
        {
          grd_session_notify_keyboard_keysym (session, keysym,
                                              GRD_KEY_STATE_RELEASED);
        }
    }
}

static gboolean
notify_keyboard_key_released (gpointer key,
                              gpointer value,
                              gpointer user_data)
{
  GrdSession *session = user_data;
  uint32_t keysym = GPOINTER_TO_UINT (key);

  grd_session_notify_keyboard_keysym (session, keysym,
                                      GRD_KEY_STATE_RELEASED);

  return TRUE;
}

static void
handle_release_all_keys (rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;

  g_hash_table_foreach_remove (session_vnc->pressed_keys,
                               notify_keyboard_key_released,
                               session_vnc);
}

static void
grd_session_vnc_notify_axis (GrdSessionVnc *session_vnc,
                             int            button_mask_bit_index)
{
  GrdSession *session = GRD_SESSION (session_vnc);
  GrdPointerAxis axis;
  int steps;

  switch (button_mask_bit_index)
    {
    case 4:
      axis = GRD_POINTER_AXIS_VERTICAL;
      steps = -1;
      break;

    case 5:
      axis = GRD_POINTER_AXIS_VERTICAL;
      steps = 1;
      break;

    case 6:
      axis = GRD_POINTER_AXIS_HORIZONTAL;
      steps = -1;
      break;

    case 7:
      axis = GRD_POINTER_AXIS_HORIZONTAL;
      steps = 1;
      break;

    default:
      return;
    }

  grd_session_notify_pointer_axis_discrete (session, axis, steps);
}

static void
handle_pointer_event (int          button_mask,
                      int          x,
                      int          y,
                      rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;
  GrdSession *session = GRD_SESSION (session_vnc);

  if (x != session_vnc->prev_x || y != session_vnc->prev_y)
    {
      grd_session_notify_pointer_motion_absolute (session, x, y);

      session_vnc->prev_x = x;
      session_vnc->prev_y = y;
    }

  if (button_mask != session_vnc->prev_button_mask)
    {
      unsigned int i;
      int buttons[] = {
        BTN_LEFT,   /* 1 */
        BTN_MIDDLE, /* 2 */
        BTN_RIGHT,  /* 3 */
        0,          /* 4 - vertical scroll: up */
        0,          /* 5 - vertical scroll: down */
        0,          /* 6 - horizontal scroll: left */
        0,          /* 7 - horizontal scroll: right */
        BTN_SIDE,   /* 8 */
        BTN_EXTRA,  /* 9 */
      };

      for (i = 0; i < G_N_ELEMENTS (buttons); i++)
        {
          int button = buttons[i];
          int prev_button_state = (session_vnc->prev_button_mask >> i) & 0x01;
          int cur_button_state = (button_mask >> i) & 0x01;

          if (prev_button_state != cur_button_state)
            {
              if (button)
                {
                  grd_session_notify_pointer_button (session,
                                                     button,
                                                     cur_button_state);
                }
              else
                {
                  grd_session_vnc_notify_axis (session_vnc, i);
                }
            }
        }

      session_vnc->prev_button_mask = button_mask;
    }
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

  rfb_screen->kbdAddEvent = handle_key_event;
  rfb_screen->kbdReleaseAllKeys = handle_release_all_keys;
  rfb_screen->ptrAddEvent = handle_pointer_event;

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

  if (condition & G_IO_IN)
    {
      if (rfbIsActive (session_vnc->rfb_screen))
        rfbProcessEvents (session_vnc->rfb_screen, 0);
    }
  else
    {
      g_warning ("Unhandled socket condition %d\n", condition);
    }

  return TRUE;
}

static void
grd_session_vnc_attach_source (GrdSessionVnc *session_vnc)
{
  GSocket *socket;

  socket = g_socket_connection_get_socket (session_vnc->connection);
  session_vnc->source = g_socket_create_source (socket,
                                                G_IO_IN | G_IO_PRI,
                                                NULL);
  g_source_set_callback (session_vnc->source,
                         (GSourceFunc) handle_socket_data,
                         session_vnc, NULL);
  g_source_attach (session_vnc->source, NULL);
}

static void
grd_session_vnc_detach_source (GrdSessionVnc *session_vnc)
{
  g_clear_pointer (&session_vnc->source, g_source_destroy);
}

GrdSessionVnc *
grd_session_vnc_new (GrdVncServer      *vnc_server,
                     GSocketConnection *connection)
{
  GrdSessionVnc *session_vnc;
  GrdContext *context;

  context = grd_vnc_server_get_context (vnc_server);
  session_vnc = g_object_new (GRD_TYPE_SESSION_VNC,
                              "context", context,
                              NULL);

  session_vnc->connection = g_object_ref (connection);

  grd_session_vnc_attach_source (session_vnc);

  init_vnc_session (session_vnc);

  return session_vnc;
}

static void
grd_session_vnc_dispose (GObject *object)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (object);

  g_assert (!session_vnc->rfb_screen);

  g_clear_pointer (&session_vnc->pressed_keys, g_hash_table_unref);

  G_OBJECT_CLASS (grd_session_vnc_parent_class)->dispose (object);
}

static void
grd_session_vnc_proxy_ready (GrdSession *session)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (session);

  session_vnc->proxy_ready = TRUE;

  if (session_vnc->rfb_client)
    grd_session_vnc_attach_source (session_vnc);
}

static void
grd_session_vnc_stop (GrdSession *session)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (session);

  if (session_vnc->pipeline)
    {
      gst_element_set_state (session_vnc->pipeline, GST_STATE_NULL);
      g_clear_pointer (&session_vnc->pipeline, gst_object_unref);
    }

  grd_session_vnc_detach_source (session_vnc);

  g_clear_object (&session_vnc->connection);
  g_clear_pointer (&session_vnc->rfb_screen->frameBuffer, g_free);
  g_clear_pointer (&session_vnc->rfb_screen, (GDestroyNotify) rfbScreenCleanup);
}

static void
grd_session_vnc_stream_added (GrdSession *session,
                              GrdStream  *stream)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (session);
  uint32_t pinos_node_id;
  g_autofree char *pipeline_str = NULL;
  GError *error = NULL;
  g_autoptr(GstElement) pipeline = NULL;
  g_autoptr(GstElement) vnc_sink = NULL;
  g_autoptr(GstPad) sink_pad = NULL;
  g_autoptr(GstPad) src_pad = NULL;

  pinos_node_id = grd_stream_get_pinos_node_id (stream);
  pipeline_str =
    g_strdup_printf ("pinossrc name=pinossrc path=%d ! videoconvert",
                     pinos_node_id);

  pipeline = gst_parse_launch (pipeline_str, &error);
  if (!pipeline)
    {
      g_warning ("Failed to start VNC pipeline: %s\n", error->message);
      return;
    }

  vnc_sink = grd_vnc_sink_new (session_vnc);
  if (!vnc_sink)
    {
      g_warning ("Failed to create VNC sink\n");
      return;
    }
  gst_bin_add (GST_BIN (pipeline), vnc_sink);

  sink_pad = gst_element_get_static_pad (vnc_sink, "sink");
  src_pad = gst_bin_find_unlinked_pad (GST_BIN (pipeline), GST_PAD_SRC);

  if (gst_pad_link (src_pad, sink_pad) != GST_PAD_LINK_OK)
    {
      g_warning ("Failed to link VNC sink to pipeline\n");
      return;
    }

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  session_vnc->pinos_src = gst_bin_get_by_name (GST_BIN (pipeline), "pinossrc");
  session_vnc->vnc_sink = g_steal_pointer (&vnc_sink);
  session_vnc->pipeline = g_steal_pointer (&pipeline);
}

static void
grd_session_vnc_init (GrdSessionVnc *session_vnc)
{
  session_vnc->pressed_keys = g_hash_table_new (NULL, NULL);
}

static void
grd_session_vnc_class_init (GrdSessionVncClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdSessionClass *session_class = GRD_SESSION_CLASS (klass);

  object_class->dispose = grd_session_vnc_dispose;

  session_class->proxy_ready = grd_session_vnc_proxy_ready;
  session_class->stop = grd_session_vnc_stop;
  session_class->stream_added = grd_session_vnc_stream_added;
}
