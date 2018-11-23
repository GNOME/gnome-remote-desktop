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
#include <linux/input.h>
#include <rfb/rfb.h>

#include "grd-context.h"
#include "grd-prompt.h"
#include "grd-settings.h"
#include "grd-stream.h"
#include "grd-vnc-server.h"
#include "grd-vnc-pipewire-stream.h"

/* BGRx */
#define BGRX_BITS_PER_SAMPLE 8
#define BGRX_SAMPLES_PER_PIXEL 3
#define BGRX_BYTES_PER_PIXEL 4

struct _GrdSessionVnc
{
  GrdSession parent;

  GSocketConnection *connection;
  GSource *source;
  rfbScreenInfoPtr rfb_screen;
  rfbClientPtr rfb_client;

  gboolean pending_framebuffer_resize;
  int pending_framebuffer_width;
  int pending_framebuffer_height;

  guint close_session_idle_id;

  GrdVncAuthMethod auth_method;

  GrdPrompt *prompt;
  GCancellable *prompt_cancellable;

  GrdVncPipeWireStream *pipewire_stream;

  int prev_x;
  int prev_y;
  int prev_button_mask;
  GHashTable *pressed_keys;
};

G_DEFINE_TYPE (GrdSessionVnc, grd_session_vnc, GRD_TYPE_SESSION);

static void
grd_session_vnc_detach_source (GrdSessionVnc *session_vnc);

static gboolean
close_session_idle (gpointer user_data);

static rfbBool
check_rfb_password (rfbClientPtr  rfb_client,
                    const char   *response_encrypted,
                    int           len);

static void
swap_uint8 (uint8_t *a,
            uint8_t *b)
{
  uint8_t tmp;

  tmp = *a;
  *a = *b;
  *b = tmp;
}

static void
resize_vnc_framebuffer (GrdSessionVnc *session_vnc,
                        int            width,
                        int            height)
{
  rfbScreenInfoPtr rfb_screen = session_vnc->rfb_screen;
  uint8_t *framebuffer;

  if (!session_vnc->rfb_client->useNewFBSize)
    g_warning ("Client does not support NewFBSize");

  g_free (rfb_screen->frameBuffer);
  framebuffer = g_malloc0 (width * height * BGRX_BYTES_PER_PIXEL);
  rfbNewFramebuffer (rfb_screen,
                     (char *) framebuffer,
                     width, height,
                     BGRX_BITS_PER_SAMPLE,
                     BGRX_SAMPLES_PER_PIXEL,
                     BGRX_BYTES_PER_PIXEL);

  /*
   * Our format is hard coded to BGRX but LibVNCServer asusumes it's RGBX;
   * lets override that.
   */
  swap_uint8 (&session_vnc->rfb_screen->serverFormat.redShift,
              &session_vnc->rfb_screen->serverFormat.blueShift);
}

void
grd_session_vnc_queue_resize_framebuffer (GrdSessionVnc *session_vnc,
                                          int            width,
                                          int            height)
{
  rfbScreenInfoPtr rfb_screen = session_vnc->rfb_screen;

  if (rfb_screen->width == width && rfb_screen->height == height)
    return;

  if (session_vnc->rfb_client->preferredEncoding == -1)
    {
      session_vnc->pending_framebuffer_resize = TRUE;
      session_vnc->pending_framebuffer_width = width;
      session_vnc->pending_framebuffer_height = height;
      return;
    }

  resize_vnc_framebuffer (session_vnc, width, height);
}

void
grd_session_vnc_draw_buffer (GrdSessionVnc *session_vnc,
                             void          *data)
{
  size_t size;

  if (session_vnc->pending_framebuffer_resize)
    return;

  size = (session_vnc->rfb_screen->height *
          grd_session_vnc_get_framebuffer_stride (session_vnc));

  memcpy (session_vnc->rfb_screen->frameBuffer,
          data,
          size);

  rfbMarkRectAsModified (session_vnc->rfb_screen, 0, 0,
                         session_vnc->rfb_screen->width,
                         session_vnc->rfb_screen->height);
  rfbProcessEvents (session_vnc->rfb_screen, 0);
}

static void
maybe_queue_close_session_idle (GrdSessionVnc *session_vnc)
{
  if (session_vnc->close_session_idle_id)
    return;

  session_vnc->close_session_idle_id =
    g_idle_add (close_session_idle, session_vnc);
}

static void
handle_client_gone (rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;

  g_debug ("VNC client gone");

  grd_session_vnc_detach_source (session_vnc);
  maybe_queue_close_session_idle (session_vnc);
}

static void
prompt_response_callback (GObject      *source_object,
                          GAsyncResult *async_result,
                          gpointer      user_data)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (user_data);
  g_autoptr(GError) error = NULL;
  GrdPromptResponse response;

  if (!grd_prompt_query_finish (session_vnc->prompt,
                                async_result,
                                &response,
                                &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("Failed to query user about session: %s", error->message);
          rfbRefuseOnHoldClient (session_vnc->rfb_client);
          return;
        }
      else
        {
          return;
        }
    }

  switch (response)
    {
    case GRD_PROMPT_RESPONSE_ACCEPT:
      grd_session_start (GRD_SESSION (session_vnc));
      rfbStartOnHoldClient (session_vnc->rfb_client);
      return;
    case GRD_PROMPT_RESPONSE_REFUSE:
      rfbRefuseOnHoldClient (session_vnc->rfb_client);
      return;
    }

  g_assert_not_reached ();
}

static enum rfbNewClientAction
handle_new_client (rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (rfb_client->screen->screenData);
  GrdContext *context = grd_session_get_context (GRD_SESSION (session_vnc));
  GrdSettings *settings = grd_context_get_settings (context);

  g_debug ("New VNC client");

  session_vnc->auth_method = grd_settings_get_vnc_auth_method (settings);

  session_vnc->rfb_client = rfb_client;
  rfb_client->clientGoneHook = handle_client_gone;

  switch (session_vnc->auth_method)
    {
    case GRD_VNC_AUTH_METHOD_PROMPT:
      session_vnc->prompt = g_object_new (GRD_TYPE_PROMPT, NULL);
      session_vnc->prompt_cancellable = g_cancellable_new ();
      grd_prompt_query_async (session_vnc->prompt,
                              rfb_client->host,
                              session_vnc->prompt_cancellable,
                              prompt_response_callback,
                              session_vnc);
      grd_session_vnc_detach_source (session_vnc);
      return RFB_CLIENT_ON_HOLD;
    case GRD_VNC_AUTH_METHOD_PASSWORD:
      session_vnc->rfb_screen->passwordCheck = check_rfb_password;
      /*
       * authPasswdData needs to be non NULL in libvncserver to trigger
       * password authentication.
       */
      session_vnc->rfb_screen->authPasswdData = (gpointer) 1;

      return RFB_CLIENT_ACCEPT;
    }

  g_assert_not_reached ();
}

static gboolean
is_view_only (GrdSessionVnc *session_vnc)
{
  GrdContext *context = grd_session_get_context (GRD_SESSION (session_vnc));
  GrdSettings *settings = grd_context_get_settings (context);

  return grd_settings_get_vnc_view_only (settings);
}

static void
handle_key_event (rfbBool      down,
                  rfbKeySym    keysym,
                  rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (rfb_client->screen->screenData);
  GrdSession *session = GRD_SESSION (session_vnc);

  if (is_view_only (session_vnc))
    return;

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
    case 3:
      axis = GRD_POINTER_AXIS_VERTICAL;
      steps = -1;
      break;

    case 4:
      axis = GRD_POINTER_AXIS_VERTICAL;
      steps = 1;
      break;

    case 5:
      axis = GRD_POINTER_AXIS_HORIZONTAL;
      steps = -1;
      break;

    case 6:
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

  if (is_view_only (session_vnc))
    return;

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
        BTN_LEFT,   /* 0 */
        BTN_MIDDLE, /* 1 */
        BTN_RIGHT,  /* 2 */
        0,          /* 3 - vertical scroll: up */
        0,          /* 4 - vertical scroll: down */
        0,          /* 5 - horizontal scroll: left */
        0,          /* 6 - horizontal scroll: right */
        BTN_SIDE,   /* 7 */
        BTN_EXTRA,  /* 8 */
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

  rfbDefaultPtrAddEvent (button_mask, x, y, rfb_client);
}

static rfbBool
check_rfb_password (rfbClientPtr  rfb_client,
                    const char   *response_encrypted,
                    int           len)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;
  GrdContext *context = grd_session_get_context (GRD_SESSION (session_vnc));
  GrdSettings *settings = grd_context_get_settings (context);
  g_autofree char *password = NULL;
  g_autoptr(GError) error = NULL;
  uint8_t challenge_encrypted[CHALLENGESIZE];

  switch (session_vnc->auth_method)
    {
    case GRD_VNC_AUTH_METHOD_PROMPT:
      return TRUE;
    case GRD_VNC_AUTH_METHOD_PASSWORD:
      break;
    }

  password = grd_settings_get_vnc_password (settings, &error);
  if (!password)
    {
      g_warning ("Couldn't retrieve VNC password: %s", error->message);
      return FALSE;
    }

  if (strlen (password) == 0)
    {
      g_warning ("VNC password was empty, denying");
      return FALSE;
    }

  memcpy(challenge_encrypted, rfb_client->authChallenge, CHALLENGESIZE);
  rfbEncryptBytes(challenge_encrypted, password);

  if (memcmp (challenge_encrypted, response_encrypted, len) == 0)
    {
      grd_session_start (GRD_SESSION (session_vnc));
      grd_session_vnc_detach_source (session_vnc);
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

int
grd_session_vnc_get_framebuffer_stride (GrdSessionVnc *session_vnc)
{
  return session_vnc->rfb_screen->paddedWidthInBytes;
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

  /* Amount of milliseconds to wait before sending an update */
  rfb_screen->deferUpdateTime = 0;

  rfb_screen->kbdAddEvent = handle_key_event;
  rfb_screen->kbdReleaseAllKeys = handle_release_all_keys;
  rfb_screen->ptrAddEvent = handle_pointer_event;

  rfb_screen->frameBuffer = g_malloc0 (screen_width * screen_height * 4);
  memset (rfb_screen->frameBuffer, 0x1f, screen_width * screen_height * 4);

  session_vnc->rfb_screen = rfb_screen;

  rfbInitServer (rfb_screen);
  rfbProcessEvents (rfb_screen, 0);
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
        {
          rfbProcessEvents (session_vnc->rfb_screen, 0);

          if (session_vnc->pending_framebuffer_resize &&
              session_vnc->rfb_client->preferredEncoding != -1)
            {
              resize_vnc_framebuffer (session_vnc,
                                      session_vnc->pending_framebuffer_width,
                                      session_vnc->pending_framebuffer_height);
              session_vnc->pending_framebuffer_resize = FALSE;
            }
        }
    }
  else
    {
      g_debug ("Unhandled socket condition %d\n", condition);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
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
grd_session_vnc_stop (GrdSession *session)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (session);

  g_debug ("Stopping VNC session");

  g_clear_object (&session_vnc->pipewire_stream);

  grd_session_vnc_detach_source (session_vnc);

  g_clear_object (&session_vnc->connection);
  g_clear_pointer (&session_vnc->rfb_screen->frameBuffer, g_free);
  g_clear_pointer (&session_vnc->rfb_screen, rfbScreenCleanup);

  if (session_vnc->close_session_idle_id)
    {
      g_source_remove (session_vnc->close_session_idle_id);
      session_vnc->close_session_idle_id = 0;
    }
}

static gboolean
close_session_idle (gpointer user_data)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (user_data);

  grd_session_stop (GRD_SESSION (session_vnc));

  session_vnc->close_session_idle_id = 0;

  return G_SOURCE_REMOVE;
}

static void
on_pipwire_stream_closed (GrdVncPipeWireStream *stream,
                          GrdSessionVnc        *session_vnc)
{
  g_warning ("PipeWire stream closed, closing client");

  maybe_queue_close_session_idle (session_vnc);
}

static void
grd_session_vnc_stream_ready (GrdSession *session,
                              GrdStream  *stream)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (session);
  uint32_t pipewire_node_id;
  g_autoptr (GError) error = NULL;

  pipewire_node_id = grd_stream_get_pipewire_node_id (stream);
  session_vnc->pipewire_stream = grd_vnc_pipewire_stream_new (session_vnc,
                                                              pipewire_node_id,
                                                              &error);
  if (!session_vnc->pipewire_stream)
    {
      g_warning ("Failed to establish PipeWire stream: %s", error->message);
      return;
    }

  g_signal_connect (session_vnc->pipewire_stream, "closed",
                    G_CALLBACK (on_pipwire_stream_closed),
                    session_vnc);

  if (!session_vnc->source)
    grd_session_vnc_attach_source (session_vnc);
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

  session_class->stop = grd_session_vnc_stop;
  session_class->stream_ready = grd_session_vnc_stream_ready;
}
