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
#include <glib/gi18n.h>
#include <linux/input.h>
#include <rfb/rfb.h>

#include "grd-clipboard-vnc.h"
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
  GrdStream *stream;

  int prev_x;
  int prev_y;
  int prev_button_mask;
  GHashTable *pressed_keys;

  GrdClipboardVnc *clipboard_vnc;
  GrdVncScreenShareMode screen_share_mode;
  gboolean is_view_only;
  GrdVncMonitorConfig *monitor_config;
};

G_DEFINE_TYPE (GrdSessionVnc, grd_session_vnc, GRD_TYPE_SESSION)

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
update_server_format (GrdSessionVnc *session_vnc)
{
  rfbScreenInfoPtr rfb_screen = session_vnc->rfb_screen;

  /*
   * Our format is hard coded to BGRX but LibVNCServer assumes it's RGBX;
   * lets override that.
   */
  swap_uint8 (&rfb_screen->serverFormat.redShift,
              &rfb_screen->serverFormat.blueShift);
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

  update_server_format (session_vnc);
  rfb_screen->setTranslateFunction (session_vnc->rfb_client);
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
grd_session_vnc_take_buffer (GrdSessionVnc *session_vnc,
                             void          *data)
{
  if (session_vnc->pending_framebuffer_resize)
    {
      free (data);
      return;
    }

  free (session_vnc->rfb_screen->frameBuffer);
  session_vnc->rfb_screen->frameBuffer = data;

  rfbMarkRectAsModified (session_vnc->rfb_screen, 0, 0,
                         session_vnc->rfb_screen->width,
                         session_vnc->rfb_screen->height);
  rfbProcessEvents (session_vnc->rfb_screen, 0);
}

void
grd_session_vnc_flush (GrdSessionVnc *session_vnc)
{
  rfbProcessEvents (session_vnc->rfb_screen, 0);
}

void
grd_session_vnc_set_cursor (GrdSessionVnc *session_vnc,
                            rfbCursorPtr   rfb_cursor)
{
  rfbSetCursor (session_vnc->rfb_screen, rfb_cursor);
}

void
grd_session_vnc_move_cursor (GrdSessionVnc *session_vnc,
                             int            x,
                             int            y)
{
  if (session_vnc->rfb_screen->cursorX == x ||
      session_vnc->rfb_screen->cursorY == y)
    return;

  LOCK (session_vnc->rfb_screen->cursorMutex);
  session_vnc->rfb_screen->cursorX = x;
  session_vnc->rfb_screen->cursorY = y;
  UNLOCK (session_vnc->rfb_screen->cursorMutex);

  session_vnc->rfb_client->cursorWasMoved = TRUE;
}

void
grd_session_vnc_set_client_clipboard_text (GrdSessionVnc *session_vnc,
                                           char          *text,
                                           int            text_length)
{
  rfbSendServerCutText(session_vnc->rfb_screen, text, text_length);
}

static void
grd_vnc_monitor_config_free (GrdVncMonitorConfig *monitor_config)
{
  g_clear_pointer (&monitor_config->connectors, g_strfreev);
  g_free (monitor_config->virtual_monitors);
  g_free (monitor_config);
}

static void
maybe_queue_close_session_idle (GrdSessionVnc *session_vnc)
{
  if (session_vnc->close_session_idle_id)
    return;

  session_vnc->close_session_idle_id =
    g_idle_add (close_session_idle, session_vnc);
}

gboolean
grd_session_vnc_is_client_gone (GrdSessionVnc *session_vnc)
{
  return !session_vnc->rfb_client;
}

static void
handle_client_gone (rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;

  g_debug ("VNC client gone");

  grd_session_vnc_detach_source (session_vnc);
  maybe_queue_close_session_idle (session_vnc);
  session_vnc->rfb_client = NULL;
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
    case GRD_PROMPT_RESPONSE_CANCEL:
      rfbRefuseOnHoldClient (session_vnc->rfb_client);
      return;
    }

  g_assert_not_reached ();
}

static void
show_sharing_desktop_prompt (GrdSessionVnc *session_vnc,
                             const char    *host)
{
  g_autoptr (GrdPromptDefinition) prompt_definition = NULL;

  session_vnc->prompt = g_object_new (GRD_TYPE_PROMPT, NULL);
  session_vnc->prompt_cancellable = g_cancellable_new ();

  prompt_definition = g_new0 (GrdPromptDefinition, 1);
  prompt_definition->summary =
    g_strdup_printf (_("Do you want to share your desktop?"));
  prompt_definition->body =
    g_strdup_printf (_("A user on the computer '%s' is trying to remotely "
                       "view or control your desktop."), host);
  prompt_definition->cancel_label = g_strdup_printf (_("Refuse"));
  prompt_definition->accept_label = g_strdup_printf (_("Accept"));

  grd_prompt_query_async (session_vnc->prompt,
                          prompt_definition,
                          session_vnc->prompt_cancellable,
                          prompt_response_callback,
                          session_vnc);
}

static enum rfbNewClientAction
handle_new_client (rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (rfb_client->screen->screenData);
  GrdContext *context = grd_session_get_context (GRD_SESSION (session_vnc));
  GrdSettings *settings = grd_context_get_settings (context);

  g_debug ("New VNC client");

  g_object_get (G_OBJECT (settings),
                "vnc-auth-method", &session_vnc->auth_method,
                NULL);

  session_vnc->rfb_client = rfb_client;
  rfb_client->clientGoneHook = handle_client_gone;

  switch (session_vnc->auth_method)
    {
    case GRD_VNC_AUTH_METHOD_PROMPT:
      show_sharing_desktop_prompt (session_vnc, rfb_client->host);
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

static void
handle_key_event (rfbBool      down,
                  rfbKeySym    keysym,
                  rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (rfb_client->screen->screenData);
  GrdSession *session = GRD_SESSION (session_vnc);

  if (session_vnc->is_view_only)
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
handle_set_clipboard_text (char         *text,
                           int           text_length,
                           rfbClientPtr  rfb_client)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;
  GrdClipboardVnc *clipboard_vnc = session_vnc->clipboard_vnc;

  grd_clipboard_vnc_set_clipboard_text (clipboard_vnc, text, text_length);
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

  if (session_vnc->is_view_only)
    return;

  if (session_vnc->stream &&
      (x != session_vnc->prev_x || y != session_vnc->prev_y))
    {
      rfbScreenInfoPtr rfb_screen = session_vnc->rfb_screen;
      GrdEventPointerMotionAbs motion_abs = {};

      motion_abs.input_rect_width = rfb_screen->width;
      motion_abs.input_rect_height = rfb_screen->height;
      motion_abs.x = x;
      motion_abs.y = y;

      grd_session_notify_pointer_motion_absolute (session, session_vnc->stream,
                                                  &motion_abs);

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

static int
handle_set_desktop_size (int                         width,
                         int                         height,
                         int                         num_screens,
                         struct rfbExtDesktopScreen *screens,
                         rfbClientPtr                rfb_client)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;
  GrdVncVirtualMonitor *monitor;

  if (!session_vnc->monitor_config->is_virtual)
    {
      g_warning ("[VNC] Ignoring SetDesktopSize request, mirror-primary mode is used");
      return rfbExtDesktopSize_ResizeProhibited;
    }

  g_debug ("[VNC] Trying to set new size %dx%d", width, height);

  g_assert (session_vnc->monitor_config->monitor_count == 1);
  g_assert (session_vnc->monitor_config->virtual_monitors);

  monitor = session_vnc->monitor_config->virtual_monitors;
  monitor->width = GRD_VNC_CLAMP_DESKTOP_SIZE (width);
  monitor->height = GRD_VNC_CLAMP_DESKTOP_SIZE (height);
  grd_vnc_pipewire_stream_resize (session_vnc->pipewire_stream, monitor);

  return rfbExtDesktopSize_Success;
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
      if (error)
        g_warning ("[VNC] Couldn't retrieve VNC password: %s", error->message);
      else
        g_message ("[VNC] Password is not set, denying client");

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
grd_session_vnc_get_stride_for_width (GrdSessionVnc *session_vnc,
                                      int            width)
{
  return width * BGRX_BYTES_PER_PIXEL;
}

static void
init_vnc_session (GrdSessionVnc *session_vnc)
{
  GSocket *socket;
  int screen_width;
  int screen_height;
  rfbScreenInfoPtr rfb_screen;
  GrdVncMonitorConfig *monitor_config;

  /* Arbitrary framebuffer size, will get the proper size from the stream. */
  screen_width = GRD_VNC_DEFAULT_WIDTH;
  screen_height = GRD_VNC_DEFAULT_HEIGHT;
  rfb_screen = rfbGetScreen (0, NULL,
                             screen_width, screen_height,
                             8, 3, 4);
  session_vnc->rfb_screen = rfb_screen;

  update_server_format (session_vnc);

  session_vnc->clipboard_vnc = grd_clipboard_vnc_new (session_vnc);

  socket = g_socket_connection_get_socket (session_vnc->connection);
  rfb_screen->inetdSock = g_socket_get_fd (socket);
  rfb_screen->desktopName = "GNOME";
  rfb_screen->versionString = "GNOME Remote Desktop (VNC)";
  rfb_screen->neverShared = TRUE;
  rfb_screen->newClientHook = handle_new_client;
  rfb_screen->screenData = session_vnc;

  /* Amount of milliseconds to wait before sending an update */
  rfb_screen->deferUpdateTime = 0;

  rfb_screen->kbdAddEvent = handle_key_event;
  rfb_screen->kbdReleaseAllKeys = handle_release_all_keys;
  rfb_screen->setXCutText = handle_set_clipboard_text;
  rfb_screen->ptrAddEvent = handle_pointer_event;
  rfb_screen->setDesktopSizeHook = handle_set_desktop_size;

  rfb_screen->frameBuffer = g_malloc0 (screen_width * screen_height * 4);
  memset (rfb_screen->frameBuffer, 0x1f, screen_width * screen_height * 4);

  session_vnc->monitor_config = g_new0 (GrdVncMonitorConfig, 1);
  monitor_config = session_vnc->monitor_config;
  monitor_config->virtual_monitors = NULL;
  /* No multi-monitor support yet */
  monitor_config->monitor_count = 1;

  if (session_vnc->screen_share_mode == GRD_VNC_SCREEN_SHARE_MODE_EXTEND)
    {
      monitor_config->is_virtual = TRUE;
      monitor_config->virtual_monitors = g_new0 (GrdVncVirtualMonitor, 1);

      monitor_config->virtual_monitors->pos_x = 0;
      monitor_config->virtual_monitors->pos_y = 0;
      monitor_config->virtual_monitors->width = GRD_VNC_CLAMP_DESKTOP_SIZE (screen_width);
      monitor_config->virtual_monitors->height = GRD_VNC_CLAMP_DESKTOP_SIZE (screen_height);
    }
  else
    {
      monitor_config->is_virtual = FALSE;
      g_autoptr (GStrvBuilder) connector_builder = NULL;
      char **connectors;

      connector_builder = g_strv_builder_new ();
      g_strv_builder_add (connector_builder, "");
      connectors = g_strv_builder_end (connector_builder);

      session_vnc->monitor_config->connectors = connectors;
    }

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

              /**
               * This is a workaround. libvncserver is unable to handle clipboard
               * changes early and either disconnects the client or crashes g-r-d
               * if it receives rfbSendServerCutText too early altough the
               * authentification process is already done.
               * Doing this after resizing the framebuffer, seems to work fine,
               * so enable the clipboard here and not when the remote desktop
               * session proxy is acquired.
               */
              grd_clipboard_vnc_maybe_enable_clipboard (session_vnc->clipboard_vnc);
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
  if (!session_vnc->source)
    return;

  g_source_destroy (session_vnc->source);
  g_clear_pointer (&session_vnc->source, g_source_unref);
}

static void
on_view_only_changed (GrdSettings   *settings,
                      GParamSpec    *pspec,
                      GrdSessionVnc *session_vnc)
{
  g_object_get (G_OBJECT (settings),
                "vnc-view-only", &session_vnc->is_view_only,
                NULL);
}

GrdSessionVnc *
grd_session_vnc_new (GrdVncServer      *vnc_server,
                     GSocketConnection *connection)
{
  GrdSessionVnc *session_vnc;
  GrdContext *context;
  GrdSettings *settings;

  context = grd_vnc_server_get_context (vnc_server);
  session_vnc = g_object_new (GRD_TYPE_SESSION_VNC,
                              "context", context,
                              NULL);

  session_vnc->connection = g_object_ref (connection);

  settings = grd_context_get_settings (context);
  g_object_get (G_OBJECT (settings),
                "vnc-screen-share-mode", &session_vnc->screen_share_mode,
                "vnc-view-only", &session_vnc->is_view_only,
                NULL);

  g_signal_connect (settings, "notify::vnc-view-only",
                    G_CALLBACK (on_view_only_changed),
                    session_vnc);

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
  if (session_vnc->stream)
    {
      grd_stream_disconnect_proxy_signals (session_vnc->stream);
      g_clear_object (&session_vnc->stream);
    }

  grd_session_vnc_detach_source (session_vnc);

  g_clear_object (&session_vnc->connection);
  g_clear_object (&session_vnc->clipboard_vnc);
  g_clear_pointer (&session_vnc->rfb_screen->frameBuffer, g_free);
  g_clear_pointer (&session_vnc->rfb_screen, rfbScreenCleanup);
  g_clear_pointer (&session_vnc->monitor_config, grd_vnc_monitor_config_free);
  if (session_vnc->prompt_cancellable)
    {
      g_cancellable_cancel (session_vnc->prompt_cancellable);
      g_clear_object (&session_vnc->prompt_cancellable);
    }
  g_clear_object (&session_vnc->prompt);

  g_clear_handle_id (&session_vnc->close_session_idle_id, g_source_remove);
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
grd_session_vnc_remote_desktop_session_started (GrdSession *session)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (session);

  if (session_vnc->monitor_config->is_virtual)
    g_debug ("[VNC] Remote Desktop session will use virtual monitors");
  else
    g_debug ("[VNC] Remote Desktop session will mirror the primary monitor");

  /* Not supporting multi-monitor at the moment */
  g_assert (session_vnc->monitor_config->monitor_count == 1);

  if (session_vnc->monitor_config->is_virtual)
    {
      grd_session_record_virtual (session, 0,
                                  GRD_SCREEN_CAST_CURSOR_MODE_METADATA,
                                  TRUE);
    }
  else
    {
      const char *connector;

      connector = session_vnc->monitor_config->connectors[0];
      grd_session_record_monitor (session, 0, connector,
                                  GRD_SCREEN_CAST_CURSOR_MODE_METADATA);
    }
}

static void
on_pipewire_stream_closed (GrdVncPipeWireStream *stream,
                           GrdSessionVnc        *session_vnc)
{
  g_warning ("PipeWire stream closed, closing client");

  maybe_queue_close_session_idle (session_vnc);
}

static void
on_stream_ready (GrdStream     *stream,
                 GrdSessionVnc *session_vnc)
{
  uint32_t pipewire_node_id;
  GrdVncVirtualMonitor *virtual_monitor;
  g_autoptr (GError) error = NULL;

  virtual_monitor = session_vnc->monitor_config->virtual_monitors;
  pipewire_node_id = grd_stream_get_pipewire_node_id (stream);
  session_vnc->pipewire_stream = grd_vnc_pipewire_stream_new (session_vnc,
                                                              pipewire_node_id,
                                                              virtual_monitor,
                                                              &error);
  if (!session_vnc->pipewire_stream)
    {
      g_warning ("Failed to establish PipeWire stream: %s", error->message);
      return;
    }

  g_signal_connect (session_vnc->pipewire_stream, "closed",
                    G_CALLBACK (on_pipewire_stream_closed),
                    session_vnc);

  if (!session_vnc->source)
    grd_session_vnc_attach_source (session_vnc);
}

static void
grd_session_vnc_on_stream_created (GrdSession *session,
                                   uint32_t    stream_id,
                                   GrdStream  *stream)
{
  GrdSessionVnc *session_vnc = GRD_SESSION_VNC (session);

  g_assert (!session_vnc->stream);

  session_vnc->stream = stream;
  g_signal_connect (stream, "ready", G_CALLBACK (on_stream_ready),
                    session_vnc);
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
  session_class->remote_desktop_session_started =
    grd_session_vnc_remote_desktop_session_started;
  session_class->on_stream_created = grd_session_vnc_on_stream_created;
}
