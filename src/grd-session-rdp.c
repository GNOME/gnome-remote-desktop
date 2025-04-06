/*
 * Copyright (C) 2020-2023 Pascal Nowack
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

#include "grd-session-rdp.h"

#include <freerdp/channels/drdynvc.h>
#include <freerdp/crypto/crypto.h>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "grd-clipboard-rdp.h"
#include "grd-context.h"
#include "grd-rdp-cursor-renderer.h"
#include "grd-rdp-dvc-audio-input.h"
#include "grd-rdp-dvc-audio-playback.h"
#include "grd-rdp-dvc-display-control.h"
#include "grd-rdp-dvc-graphics-pipeline.h"
#include "grd-rdp-dvc-handler.h"
#include "grd-rdp-dvc-input.h"
#include "grd-rdp-dvc-telemetry.h"
#include "grd-rdp-event-queue.h"
#include "grd-rdp-layout-manager.h"
#include "grd-rdp-network-autodetection.h"
#include "grd-rdp-private.h"
#include "grd-rdp-renderer.h"
#include "grd-rdp-sam.h"
#include "grd-rdp-server.h"
#include "grd-rdp-session-metrics.h"
#include "grd-settings.h"

#define MAX_MONITOR_COUNT_HEADLESS 16
#define MAX_MONITOR_COUNT_SCREEN_SHARE 1
#define DISCRETE_SCROLL_STEP 10.0
#define ELEMENT_TYPE_CERTIFICATE 32

enum
{
  POST_CONNECTED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef enum _RdpPeerFlag
{
  RDP_PEER_ACTIVATED = 1 << 0,
} RdpPeerFlag;

typedef enum _PauseKeyState
{
  PAUSE_KEY_STATE_NONE,
  PAUSE_KEY_STATE_CTRL_DOWN,
  PAUSE_KEY_STATE_NUMLOCK_DOWN,
  PAUSE_KEY_STATE_CTRL_UP,
} PauseKeyState;

struct _GrdSessionRdp
{
  GrdSession parent;

  GSocketConnection *connection;
  freerdp_peer *peer;
  GrdRdpSAMFile *sam_file;
  uint32_t rdp_error_info;
  GrdRdpScreenShareMode screen_share_mode;
  gboolean is_view_only;
  gboolean session_should_stop;

  GrdRdpSessionMetrics *session_metrics;

  GMutex rdp_flags_mutex;
  RdpPeerFlag rdp_flags;

  GThread *socket_thread;
  HANDLE stop_event;

  GrdRdpRenderer *renderer;
  GrdRdpCursorRenderer *cursor_renderer;

  GHashTable *pressed_keys;
  GHashTable *pressed_unicode_keys;
  PauseKeyState pause_key_state;

  GrdRdpEventQueue *rdp_event_queue;

  GrdHwAccelVulkan *hwaccel_vulkan;
  GrdHwAccelNvidia *hwaccel_nvidia;

  GrdRdpLayoutManager *layout_manager;

  GHashTable *stream_table;

  GMutex close_session_mutex;
  unsigned int close_session_idle_id;

  GMutex notify_post_connected_mutex;
  unsigned int notify_post_connected_source_id;

  uint32_t next_stream_id;
};

G_DEFINE_TYPE (GrdSessionRdp, grd_session_rdp, GRD_TYPE_SESSION)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (rdpCertificate, freerdp_certificate_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (rdpRedirection, redirection_free)

static gboolean
close_session_idle (gpointer user_data);

static gboolean
is_rdp_peer_flag_set (GrdSessionRdp *session_rdp,
                      RdpPeerFlag    flag)
{
  gboolean state;

  g_mutex_lock (&session_rdp->rdp_flags_mutex);
  state = !!(session_rdp->rdp_flags & flag);
  g_mutex_unlock (&session_rdp->rdp_flags_mutex);

  return state;
}

static void
set_rdp_peer_flag (GrdSessionRdp *session_rdp,
                   RdpPeerFlag    flag)
{
  g_mutex_lock (&session_rdp->rdp_flags_mutex);
  session_rdp->rdp_flags |= flag;
  g_mutex_unlock (&session_rdp->rdp_flags_mutex);
}

static void
unset_rdp_peer_flag (GrdSessionRdp *session_rdp,
                     RdpPeerFlag    flag)
{
  g_mutex_lock (&session_rdp->rdp_flags_mutex);
  session_rdp->rdp_flags &= ~flag;
  g_mutex_unlock (&session_rdp->rdp_flags_mutex);
}

GrdRdpSessionMetrics *
grd_session_rdp_get_session_metrics (GrdSessionRdp *session_rdp)
{
  return session_rdp->session_metrics;
}

static uint32_t
get_next_free_stream_id (GrdSessionRdp *session_rdp)
{
  uint32_t stream_id = session_rdp->next_stream_id;

  while (g_hash_table_contains (session_rdp->stream_table,
                                GUINT_TO_POINTER (stream_id)))
    ++stream_id;

  session_rdp->next_stream_id = stream_id + 1;

  return stream_id;
}

uint32_t
grd_session_rdp_acquire_stream_id (GrdSessionRdp     *session_rdp,
                                   GrdRdpStreamOwner *stream_owner)
{
  uint32_t stream_id;

  stream_id = get_next_free_stream_id (session_rdp);
  g_hash_table_insert (session_rdp->stream_table,
                       GUINT_TO_POINTER (stream_id), stream_owner);

  return stream_id;
}

void
grd_session_rdp_release_stream_id (GrdSessionRdp *session_rdp,
                                   uint32_t       stream_id)
{
  g_hash_table_remove (session_rdp->stream_table, GUINT_TO_POINTER (stream_id));
}

gboolean
grd_session_rdp_is_client_mstsc (GrdSessionRdp *session_rdp)
{
  rdpContext *rdp_context = session_rdp->peer->context;
  rdpSettings *rdp_settings = rdp_context->settings;

  return freerdp_settings_get_uint32 (rdp_settings, FreeRDP_OsMajorType) ==
         OSMAJORTYPE_WINDOWS &&
         freerdp_settings_get_uint32 (rdp_settings, FreeRDP_OsMinorType) ==
         OSMINORTYPE_WINDOWS_NT;
}

static WCHAR *
get_utf16_string (const char *str,
                  size_t     *size)
{
  WCHAR *utf16_string;

  *size = 0;

  utf16_string = ConvertUtf8ToWCharAlloc (str, size);
  if (!utf16_string)
    return NULL;

  *size = (*size + 1) * sizeof (WCHAR);

  return utf16_string;
}

static WCHAR *
generate_encoded_redirection_guid (size_t *size)
{
  BYTE redirection_guid[16] = {};
  g_autofree char *redirection_guid_base_64 = NULL;
  WCHAR *encoded_redirection_guid;

  *size = 0;

  if (winpr_RAND (redirection_guid, 16) == -1)
    return NULL;

  redirection_guid_base_64 = crypto_base64_encode (redirection_guid, 16);
  if (!redirection_guid_base_64)
    return NULL;

  encoded_redirection_guid = get_utf16_string (redirection_guid_base_64, size);
  if (!encoded_redirection_guid)
    return NULL;

  return encoded_redirection_guid;
}

static BYTE *
get_certificate_container (const char *certificate,
                           size_t     *size)
{
  g_autofree BYTE *der_certificate = NULL;
  g_autoptr (rdpCertificate) rdp_certificate = NULL;
  BYTE *certificate_container = NULL;
  size_t der_certificate_len = 0;
  wStream *s;

  *size = 0;

  rdp_certificate = freerdp_certificate_new_from_pem (certificate);
  if (!rdp_certificate)
    return NULL;

  der_certificate = freerdp_certificate_get_der (rdp_certificate,
                                                 &der_certificate_len);
  if (!der_certificate)
    return NULL;

  s = Stream_New (NULL, 2048);
  g_assert (s);

  if (!Stream_EnsureRemainingCapacity (s, 12))
    g_assert_not_reached ();

  Stream_Write_UINT32 (s, ELEMENT_TYPE_CERTIFICATE);
  Stream_Write_UINT32 (s, ENCODING_TYPE_ASN1_DER);
  Stream_Write_UINT32 (s, der_certificate_len);

  if (!Stream_EnsureRemainingCapacity (s, der_certificate_len))
    g_assert_not_reached ();

  Stream_Write (s, der_certificate, der_certificate_len);

  *size = Stream_GetPosition (s);
  certificate_container = Stream_Buffer (s);

  Stream_Free (s, FALSE);

  return certificate_container;
}

gboolean
grd_session_rdp_send_server_redirection (GrdSessionRdp *session_rdp,
                                         const char    *routing_token,
                                         const char    *username,
                                         const char    *password,
                                         const char    *certificate)
{
  freerdp_peer *peer = session_rdp->peer;
  rdpSettings *rdp_settings = peer->context->settings;
  uint32_t os_major_type =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_OsMajorType);
  g_autoptr (rdpRedirection) redirection = NULL;
  g_autofree BYTE *certificate_container = NULL;
  g_autofree WCHAR *utf16_password = NULL;
  g_autofree WCHAR *utf16_encoded_redirection_guid = NULL;
  size_t size = 0;
  uint32_t redirection_flags = 0;
  uint32_t redirection_incorrect_flags = 0;

  g_assert (routing_token);
  g_assert (username);
  g_assert (password);
  g_assert (certificate);

  redirection = redirection_new ();
  g_assert (redirection);

  /* Load Balance Info */
  redirection_flags |= LB_LOAD_BALANCE_INFO;
  redirection_set_byte_option (redirection, LB_LOAD_BALANCE_INFO,
                               (BYTE *) routing_token,
                               strlen (routing_token));

  /* Username */
  redirection_flags |= LB_USERNAME;
  redirection_set_string_option (redirection, LB_USERNAME,
                                 username);

  /* Password */
  redirection_flags |= LB_PASSWORD;
  if (os_major_type != OSMAJORTYPE_IOS && os_major_type != OSMAJORTYPE_ANDROID)
    redirection_flags |= LB_PASSWORD_IS_PK_ENCRYPTED;
  utf16_password = get_utf16_string (password, &size);
  g_assert (utf16_password);
  redirection_set_byte_option (redirection, LB_PASSWORD,
                               (BYTE *) utf16_password,
                               size);

  /* Redirection GUID */
  redirection_flags |= LB_REDIRECTION_GUID;
  utf16_encoded_redirection_guid = generate_encoded_redirection_guid (&size);
  g_assert (utf16_encoded_redirection_guid);
  redirection_set_byte_option (redirection, LB_REDIRECTION_GUID,
                               (BYTE *) utf16_encoded_redirection_guid,
                               size);

  /* Target Certificate */
  redirection_flags |= LB_TARGET_CERTIFICATE;
  certificate_container = get_certificate_container (certificate, &size);
  g_assert (certificate_container);
  redirection_set_byte_option (redirection,
                               LB_TARGET_CERTIFICATE,
                               certificate_container,
                               size);

  redirection_set_flags (redirection, redirection_flags);

  if (!redirection_settings_are_valid (redirection, &redirection_incorrect_flags))
    {
      g_warning ("[RDP] Something went wrong sending Server Redirection PDU. "
                 "Incorrect flag/s: 0x%08x", redirection_incorrect_flags);
      return FALSE;
    }

  g_message ("[RDP] Sending server redirection");
  if (!peer->SendServerRedirection (peer, redirection))
    {
      g_warning ("[RDP] Error sending server Redirection");
      return FALSE;
    }

  return TRUE;
}

static void
maybe_queue_close_session_idle (GrdSessionRdp *session_rdp)
{
  g_mutex_lock (&session_rdp->close_session_mutex);
  if (session_rdp->close_session_idle_id)
    {
      g_mutex_unlock (&session_rdp->close_session_mutex);
      return;
    }

  session_rdp->close_session_idle_id =
    g_idle_add (close_session_idle, session_rdp);
  g_mutex_unlock (&session_rdp->close_session_mutex);

  session_rdp->session_should_stop = TRUE;
  SetEvent (session_rdp->stop_event);
}

void
grd_session_rdp_notify_error (GrdSessionRdp      *session_rdp,
                              GrdSessionRdpError  error_info)
{
  switch (error_info)
    {
    case GRD_SESSION_RDP_ERROR_NONE:
      g_assert_not_reached ();
      break;
    case GRD_SESSION_RDP_ERROR_BAD_CAPS:
      session_rdp->rdp_error_info = ERRINFO_BAD_CAPABILITIES;
      break;
    case GRD_SESSION_RDP_ERROR_BAD_MONITOR_DATA:
      session_rdp->rdp_error_info = ERRINFO_BAD_MONITOR_DATA;
      break;
    case GRD_SESSION_RDP_ERROR_CLOSE_STACK_ON_DRIVER_FAILURE:
      session_rdp->rdp_error_info = ERRINFO_CLOSE_STACK_ON_DRIVER_FAILURE;
      break;
    case GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED:
      session_rdp->rdp_error_info = ERRINFO_GRAPHICS_SUBSYSTEM_FAILED;
      break;
    case GRD_SESSION_RDP_ERROR_SERVER_REDIRECTION:
      session_rdp->rdp_error_info = ERRINFO_CB_CONNECTION_CANCELLED;
      break;
    }

  unset_rdp_peer_flag (session_rdp, RDP_PEER_ACTIVATED);
  maybe_queue_close_session_idle (session_rdp);
}

void
grd_session_rdp_tear_down_channel (GrdSessionRdp *session_rdp,
                                   GrdRdpChannel  channel)
{
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;

  g_mutex_lock (&rdp_peer_context->channel_mutex);
  switch (channel)
    {
    case GRD_RDP_CHANNEL_NONE:
      g_assert_not_reached ();
      break;
    case GRD_RDP_CHANNEL_AUDIO_INPUT:
      g_clear_object (&rdp_peer_context->audio_input);
      break;
    case GRD_RDP_CHANNEL_AUDIO_PLAYBACK:
      g_clear_object (&rdp_peer_context->audio_playback);
      break;
    case GRD_RDP_CHANNEL_DISPLAY_CONTROL:
      g_clear_object (&rdp_peer_context->display_control);
      break;
    case GRD_RDP_CHANNEL_GRAPHICS_PIPELINE:
      g_assert_not_reached ();
      break;
    case GRD_RDP_CHANNEL_INPUT:
      g_clear_object (&rdp_peer_context->input);
      break;
    case GRD_RDP_CHANNEL_TELEMETRY:
      g_clear_object (&rdp_peer_context->telemetry);
      break;
    }
  g_mutex_unlock (&rdp_peer_context->channel_mutex);
}

static void
handle_client_gone (GrdSessionRdp *session_rdp)
{
  g_debug ("RDP client gone");

  unset_rdp_peer_flag (session_rdp, RDP_PEER_ACTIVATED);
  maybe_queue_close_session_idle (session_rdp);
}

static gboolean
notify_keycode_released (gpointer key,
                         gpointer value,
                         gpointer user_data)
{
  GrdSessionRdp *session_rdp = user_data;
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;
  uint32_t keycode = GPOINTER_TO_UINT (key);

  grd_rdp_event_queue_add_input_event_keyboard_keycode (rdp_event_queue,
                                                        keycode,
                                                        GRD_KEY_STATE_RELEASED);

  return TRUE;
}

static gboolean
notify_keysym_released (gpointer key,
                        gpointer value,
                        gpointer user_data)
{
  GrdSessionRdp *session_rdp = user_data;
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;
  xkb_keysym_t keysym = GPOINTER_TO_UINT (key);

  grd_rdp_event_queue_add_input_event_keyboard_keysym (rdp_event_queue,
                                                       keysym,
                                                       GRD_KEY_STATE_RELEASED);

  return TRUE;
}

static BOOL
rdp_input_synchronize_event (rdpInput *rdp_input,
                             uint32_t  flags)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;

  g_debug ("[RDP] Received Synchronize event with flags 0x%08X", flags);

  if (!is_rdp_peer_flag_set (session_rdp, RDP_PEER_ACTIVATED) ||
      session_rdp->is_view_only)
    return TRUE;

  g_hash_table_foreach_remove (session_rdp->pressed_keys,
                               notify_keycode_released,
                               session_rdp);

  g_hash_table_foreach_remove (session_rdp->pressed_unicode_keys,
                               notify_keysym_released,
                               session_rdp);

  grd_rdp_event_queue_add_synchronization_event (rdp_event_queue,
                                                 !!(flags & KBD_SYNC_CAPS_LOCK),
                                                 !!(flags & KBD_SYNC_NUM_LOCK));

  return TRUE;
}

static BOOL
rdp_input_mouse_event (rdpInput *rdp_input,
                       uint16_t  flags,
                       uint16_t  x,
                       uint16_t  y)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;
  GrdEventMotionAbs motion_abs = {};
  GrdStream *stream = NULL;
  GrdButtonState button_state;
  int32_t button = 0;
  uint16_t axis_value;
  double axis_step;

  if (!is_rdp_peer_flag_set (session_rdp, RDP_PEER_ACTIVATED) ||
      session_rdp->is_view_only)
    return TRUE;

  if (!(flags & PTR_FLAGS_WHEEL) && !(flags & PTR_FLAGS_HWHEEL) &&
      grd_rdp_layout_manager_transform_position (session_rdp->layout_manager,
                                                 x, y,
                                                 &stream, &motion_abs))
    {
      grd_rdp_event_queue_add_input_event_pointer_motion_abs (rdp_event_queue,
                                                              stream,
                                                              &motion_abs);
    }

  button_state = flags & PTR_FLAGS_DOWN ? GRD_BUTTON_STATE_PRESSED
                                        : GRD_BUTTON_STATE_RELEASED;

  if (flags & PTR_FLAGS_BUTTON1)
    button = BTN_LEFT;
  else if (flags & PTR_FLAGS_BUTTON2)
    button = BTN_RIGHT;
  else if (flags & PTR_FLAGS_BUTTON3)
    button = BTN_MIDDLE;

  if (button)
    {
      grd_rdp_event_queue_add_input_event_pointer_button (rdp_event_queue,
                                                          button, button_state);
    }

  if (!(flags & PTR_FLAGS_WHEEL) && !(flags & PTR_FLAGS_HWHEEL))
    return TRUE;

  axis_value = flags & WheelRotationMask;
  if (axis_value & PTR_FLAGS_WHEEL_NEGATIVE)
    {
      axis_value = ~axis_value & WheelRotationMask;
      ++axis_value;
    }

  axis_step = -axis_value / 120.0;
  if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
    axis_step = -axis_step;

  if (flags & PTR_FLAGS_WHEEL)
    {
      grd_rdp_event_queue_add_input_event_pointer_axis (
        rdp_event_queue, 0, axis_step * DISCRETE_SCROLL_STEP,
        GRD_POINTER_AXIS_FLAGS_SOURCE_WHEEL);
    }
  if (flags & PTR_FLAGS_HWHEEL)
    {
      grd_rdp_event_queue_add_input_event_pointer_axis (
        rdp_event_queue, -axis_step * DISCRETE_SCROLL_STEP, 0,
        GRD_POINTER_AXIS_FLAGS_SOURCE_WHEEL);
    }

  return TRUE;
}

static BOOL
rdp_input_extended_mouse_event (rdpInput *rdp_input,
                                uint16_t  flags,
                                uint16_t  x,
                                uint16_t  y)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;
  GrdButtonState button_state;
  int32_t button = 0;

  if (!is_rdp_peer_flag_set (session_rdp, RDP_PEER_ACTIVATED) ||
      session_rdp->is_view_only)
    return TRUE;

  rdp_input_mouse_event (rdp_input, PTR_FLAGS_MOVE, x, y);

  button_state = flags & PTR_XFLAGS_DOWN ? GRD_BUTTON_STATE_PRESSED
                                         : GRD_BUTTON_STATE_RELEASED;

  if (flags & PTR_XFLAGS_BUTTON1)
    button = BTN_SIDE;
  else if (flags & PTR_XFLAGS_BUTTON2)
    button = BTN_EXTRA;

  if (button)
    {
      grd_rdp_event_queue_add_input_event_pointer_button (rdp_event_queue,
                                                          button, button_state);
    }

  return TRUE;
}

static BOOL
rdp_input_rel_mouse_event (rdpInput *rdp_input,
                           uint16_t  flags,
                           int16_t   dx,
                           int16_t   dy)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;
  GrdButtonState button_state;
  int32_t button = 0;

  grd_rdp_event_queue_add_input_event_pointer_motion (rdp_event_queue, dx, dy);

  button_state = flags & PTR_FLAGS_DOWN ? GRD_BUTTON_STATE_PRESSED
                                        : GRD_BUTTON_STATE_RELEASED;

  if (flags & PTR_FLAGS_BUTTON1)
    button = BTN_LEFT;
  else if (flags & PTR_FLAGS_BUTTON2)
    button = BTN_RIGHT;
  else if (flags & PTR_FLAGS_BUTTON3)
    button = BTN_MIDDLE;
  else if (flags & PTR_XFLAGS_BUTTON1)
    button = BTN_SIDE;
  else if (flags & PTR_XFLAGS_BUTTON2)
    button = BTN_EXTRA;

  if (button)
    {
      grd_rdp_event_queue_add_input_event_pointer_button (rdp_event_queue,
                                                          button, button_state);
    }

  return TRUE;
}

static gboolean
is_pause_key_sequence (GrdSessionRdp *session_rdp,
                       uint16_t       vkcode,
                       uint16_t       flags)
{
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;

  switch (session_rdp->pause_key_state)
    {
    case PAUSE_KEY_STATE_NONE:
      if (vkcode == VK_LCONTROL &&
          !(flags & KBD_FLAGS_RELEASE) &&
          flags & KBD_FLAGS_EXTENDED1)
        {
          session_rdp->pause_key_state = PAUSE_KEY_STATE_CTRL_DOWN;
          return TRUE;
        }
      return FALSE;
    case PAUSE_KEY_STATE_CTRL_DOWN:
      if (vkcode == VK_NUMLOCK &&
          !(flags & KBD_FLAGS_RELEASE))
        {
          session_rdp->pause_key_state = PAUSE_KEY_STATE_NUMLOCK_DOWN;
          return TRUE;
        }
      break;
    case PAUSE_KEY_STATE_NUMLOCK_DOWN:
      if (vkcode == VK_LCONTROL &&
          flags & KBD_FLAGS_RELEASE &&
          flags & KBD_FLAGS_EXTENDED1)
        {
          session_rdp->pause_key_state = PAUSE_KEY_STATE_CTRL_UP;
          return TRUE;
        }
      break;
    case PAUSE_KEY_STATE_CTRL_UP:
      if (vkcode == VK_NUMLOCK &&
          flags & KBD_FLAGS_RELEASE)
        {
          session_rdp->pause_key_state = PAUSE_KEY_STATE_NONE;
          grd_rdp_event_queue_add_input_event_keyboard_keysym (
            rdp_event_queue, XKB_KEY_Pause, GRD_KEY_STATE_PRESSED);
          grd_rdp_event_queue_add_input_event_keyboard_keysym (
            rdp_event_queue, XKB_KEY_Pause, GRD_KEY_STATE_RELEASED);

          return TRUE;
        }
      break;
    }

  g_warning ("Received invalid pause key sequence");
  session_rdp->pause_key_state = PAUSE_KEY_STATE_NONE;

  return FALSE;
}

static BOOL
rdp_input_keyboard_event (rdpInput *rdp_input,
                          uint16_t  flags,
                          uint8_t   code)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;
  rdpSettings *rdp_settings = rdp_input->context->settings;
  uint32_t keyboard_type =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_KeyboardType);
  GrdKeyState key_state;
  uint16_t scancode;
  uint16_t fullcode;
  uint16_t vkcode;
  uint16_t keycode;

  if (!is_rdp_peer_flag_set (session_rdp, RDP_PEER_ACTIVATED) ||
      session_rdp->is_view_only)
    return TRUE;

  scancode = code;
  fullcode = flags & KBD_FLAGS_EXTENDED ? scancode | KBDEXT : scancode;
  vkcode = GetVirtualKeyCodeFromVirtualScanCode (fullcode, keyboard_type);
  vkcode = flags & KBD_FLAGS_EXTENDED ? vkcode | KBDEXT : vkcode;
  keycode = GetKeycodeFromVirtualKeyCode (vkcode, WINPR_KEYCODE_TYPE_EVDEV);

  key_state = flags & KBD_FLAGS_RELEASE ? GRD_KEY_STATE_RELEASED
                                        : GRD_KEY_STATE_PRESSED;

  if (is_pause_key_sequence (session_rdp, vkcode, flags))
    return TRUE;

  if (key_state == GRD_KEY_STATE_PRESSED)
    {
      if (!g_hash_table_add (session_rdp->pressed_keys,
                             GUINT_TO_POINTER (keycode)))
        return TRUE;
    }
  else
    {
      if (!g_hash_table_remove (session_rdp->pressed_keys,
                                GUINT_TO_POINTER (keycode)))
        return TRUE;
    }

  grd_rdp_event_queue_add_input_event_keyboard_keycode (rdp_event_queue,
                                                        keycode, key_state);

  return TRUE;
}

static BOOL
rdp_input_unicode_keyboard_event (rdpInput *rdp_input,
                                  uint16_t  flags,
                                  uint16_t  code_utf16)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;
  uint32_t *code_utf32;
  xkb_keysym_t keysym;
  GrdKeyState key_state;

  if (!is_rdp_peer_flag_set (session_rdp, RDP_PEER_ACTIVATED) ||
      session_rdp->is_view_only)
    return TRUE;

  code_utf32 = g_utf16_to_ucs4 (&code_utf16, 1, NULL, NULL, NULL);
  if (!code_utf32)
    return TRUE;

  keysym = xkb_utf32_to_keysym (*code_utf32);
  g_free (code_utf32);

  key_state = flags & KBD_FLAGS_RELEASE ? GRD_KEY_STATE_RELEASED
                                        : GRD_KEY_STATE_PRESSED;

  if (key_state == GRD_KEY_STATE_PRESSED)
    {
      if (!g_hash_table_add (session_rdp->pressed_unicode_keys,
                             GUINT_TO_POINTER (keysym)))
        return TRUE;
    }
  else
    {
      if (!g_hash_table_remove (session_rdp->pressed_unicode_keys,
                                GUINT_TO_POINTER (keysym)))
        return TRUE;
    }

  grd_rdp_event_queue_add_input_event_keyboard_keysym (rdp_event_queue,
                                                       keysym, key_state);

  return TRUE;
}

static BOOL
rdp_suppress_output (rdpContext         *rdp_context,
                     uint8_t             allow,
                     const RECTANGLE_16 *area)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  rdpSettings *rdp_settings = rdp_context->settings;

  if (!is_rdp_peer_flag_set (session_rdp, RDP_PEER_ACTIVATED))
    return TRUE;

  grd_rdp_renderer_update_output_suppression_state (session_rdp->renderer,
                                                    !allow);

  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline) &&
      rdp_peer_context->network_autodetection)
    {
      if (allow)
        {
          grd_rdp_network_autodetection_ensure_rtt_consumer (
            rdp_peer_context->network_autodetection,
            GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_RDPGFX);
        }
      else
        {
          grd_rdp_network_autodetection_remove_rtt_consumer (
            rdp_peer_context->network_autodetection,
            GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_RDPGFX);
        }
    }

  return TRUE;
}

static FREERDP_AUTODETECT_STATE
rdp_autodetect_on_connect_time_autodetect_begin (rdpAutoDetect *rdp_autodetect)
{
  rdpContext *rdp_context = rdp_autodetect->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  rdpSettings *rdp_settings = rdp_context->settings;
  GrdRdpNetworkAutodetection *network_autodetection;

  g_assert (freerdp_settings_get_bool (rdp_settings, FreeRDP_NetworkAutoDetect));
  g_assert (!rdp_peer_context->network_autodetection);

  network_autodetection = grd_rdp_network_autodetection_new (rdp_context);
  rdp_peer_context->network_autodetection = network_autodetection;

  grd_rdp_network_autodetection_start_connect_time_autodetection (network_autodetection);

  return FREERDP_AUTODETECT_STATE_REQUEST;
}

static uint32_t
get_max_monitor_count (GrdSessionRdp *session_rdp)
{
  GrdContext *context = grd_session_get_context (GRD_SESSION (session_rdp));

  switch (grd_context_get_runtime_mode (context))
    {
    case GRD_RUNTIME_MODE_HEADLESS:
    case GRD_RUNTIME_MODE_SYSTEM:
    case GRD_RUNTIME_MODE_HANDOVER:
      return MAX_MONITOR_COUNT_HEADLESS;
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
      return MAX_MONITOR_COUNT_SCREEN_SHARE;
    }

  g_assert_not_reached ();
}

static BOOL
rdp_peer_capabilities (freerdp_peer *peer)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  rdpSettings *rdp_settings = peer->context->settings;
  HANDLE vcm = rdp_peer_context->vcm;
  GrdRdpMonitorConfig *monitor_config;
  g_autoptr (GError) error = NULL;

  if (!freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline))
    {
      g_warning ("[RDP] Client did not advertise support for the Graphics "
                 "Pipeline, closing connection");
      return FALSE;
    }

  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline) ||
      freerdp_settings_get_bool (rdp_settings, FreeRDP_RemoteFxCodec) ||
      freerdp_settings_get_bool (rdp_settings, FreeRDP_RemoteFxImageCodec) ||
      freerdp_settings_get_bool (rdp_settings, FreeRDP_NSCodec))
    {
      uint16_t supported_color_depths =
        freerdp_settings_get_uint16 (rdp_settings, FreeRDP_SupportedColorDepths);

      if (!(supported_color_depths & RNS_UD_32BPP_SUPPORT))
        {
          g_warning ("[RDP] Protocol violation: Client advertised support for "
                     "codecs or the Graphics Pipeline, but does not support "
                     "32-bit colour depth");
          return FALSE;
        }
      if (freerdp_settings_get_uint32 (rdp_settings, FreeRDP_ColorDepth) != 32)
        {
          g_debug ("[RDP] Client prefers colour depth %u, invalidated by support "
                   "of codecs or graphics pipeline",
                   freerdp_settings_get_uint32 (rdp_settings, FreeRDP_ColorDepth));
          freerdp_settings_set_uint32 (rdp_settings, FreeRDP_ColorDepth, 32);
        }
    }

  if (!freerdp_settings_get_bool (rdp_settings, FreeRDP_DesktopResize))
    {
      g_warning ("Client doesn't support desktop resizing, closing connection");
      return FALSE;
    }

  if (!WTSVirtualChannelManagerIsChannelJoined (vcm, DRDYNVC_SVC_CHANNEL_NAME))
    {
      g_warning ("[RDP] Client doesn't support the DRDYNVC SVC, "
                 "closing connection");
      return FALSE;
    }

  if (session_rdp->screen_share_mode == GRD_RDP_SCREEN_SHARE_MODE_EXTEND)
    {
      uint32_t max_monitor_count;

      max_monitor_count = get_max_monitor_count (session_rdp);
      monitor_config =
        grd_rdp_monitor_config_new_from_client_data (rdp_settings,
                                                     max_monitor_count, &error);
    }
  else
    {
      g_autoptr (GStrvBuilder) connector_builder = NULL;
      char **connectors;

      monitor_config = g_new0 (GrdRdpMonitorConfig, 1);

      connector_builder = g_strv_builder_new ();
      g_strv_builder_add (connector_builder, "");
      connectors = g_strv_builder_end (connector_builder);

      monitor_config->connectors = connectors;
      monitor_config->monitor_count = 1;
    }
  if (!monitor_config)
    {
      g_warning ("[RDP] Received invalid monitor layout from client: %s, "
                 "closing connection", error->message);
      return FALSE;
    }

  if (session_rdp->screen_share_mode == GRD_RDP_SCREEN_SHARE_MODE_EXTEND)
    {
      freerdp_settings_set_uint32 (rdp_settings, FreeRDP_DesktopWidth,
                                   monitor_config->desktop_width);
      freerdp_settings_set_uint32 (rdp_settings, FreeRDP_DesktopHeight,
                                   monitor_config->desktop_height);
    }

  if (monitor_config->is_virtual)
    g_debug ("[RDP] Remote Desktop session will use virtual monitors");
  else
    g_debug ("[RDP] Remote Desktop session will mirror the primary monitor");

  grd_rdp_layout_manager_submit_new_monitor_config (session_rdp->layout_manager,
                                                    monitor_config);

  return TRUE;
}

static void
notify_post_connected (gpointer user_data)
{
  GrdSessionRdp *session_rdp = user_data;
  GrdContext *grd_context = grd_session_get_context (GRD_SESSION (session_rdp));

  g_mutex_lock (&session_rdp->notify_post_connected_mutex);
  session_rdp->notify_post_connected_source_id = 0;
  g_mutex_unlock (&session_rdp->notify_post_connected_mutex);

  grd_rdp_session_metrics_notify_phase_completion (session_rdp->session_metrics,
                                                   GRD_RDP_PHASE_POST_CONNECT);
  if (grd_context_get_runtime_mode (grd_context) != GRD_RUNTIME_MODE_SYSTEM)
    grd_session_start (GRD_SESSION (session_rdp));

  g_signal_emit (session_rdp, signals[POST_CONNECTED], 0);
}

static BOOL
rdp_peer_post_connect (freerdp_peer *peer)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  rdpSettings *rdp_settings = peer->context->settings;
  uint32_t rdp_version =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_RdpVersion);
  uint32_t keyboard_type =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_KeyboardType);
  uint32_t os_major_type =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_OsMajorType);

  if (freerdp_settings_get_uint32 (rdp_settings, FreeRDP_PointerCacheSize) <= 0)
    {
      g_warning ("Client doesn't have a pointer cache, closing connection");
      return FALSE;
    }
  if (!freerdp_settings_get_bool (rdp_settings, FreeRDP_FastPathOutput))
    {
      g_warning ("Client does not support fastpath output, closing connection");
      return FALSE;
    }

  g_debug ("New RDP client: [OS major type, OS minor type]: [%s, %s]",
           freerdp_peer_os_major_type_string (peer),
           freerdp_peer_os_minor_type_string (peer));
  g_debug ("[RDP] Maximum common RDP version 0x%08X", rdp_version);
  g_debug ("[RDP] Client uses keyboard type %u", keyboard_type);

  g_debug ("[RDP] Virtual Channels: compression flags: %u, "
           "compression level: %u, chunk size: %u",
           freerdp_settings_get_uint32 (rdp_settings, FreeRDP_VCFlags),
           freerdp_settings_get_uint32 (rdp_settings, FreeRDP_CompressionLevel),
           freerdp_settings_get_uint32 (rdp_settings, FreeRDP_VCChunkSize));

  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline) &&
      !freerdp_settings_get_bool (rdp_settings, FreeRDP_NetworkAutoDetect))
    {
      g_warning ("Client does not support autodetecting network characteristics "
                 "(RTT detection, Bandwidth measurement). "
                 "High latency connections will suffer!");
    }
  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_AudioPlayback) &&
      !freerdp_settings_get_bool (rdp_settings, FreeRDP_NetworkAutoDetect))
    {
      g_warning ("[RDP] Client does not support autodetecting network "
                 "characteristics. Disabling audio output redirection");
      freerdp_settings_set_bool (rdp_settings, FreeRDP_AudioPlayback, FALSE);
    }
  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_AudioPlayback) &&
      (os_major_type == OSMAJORTYPE_IOS ||
       os_major_type == OSMAJORTYPE_ANDROID))
    {
      g_warning ("[RDP] Client cannot handle graphics and audio "
                 "simultaneously. Disabling audio output redirection");
      freerdp_settings_set_bool (rdp_settings, FreeRDP_AudioPlayback, FALSE);
    }

  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline))
    grd_rdp_renderer_notify_graphics_pipeline_reset (session_rdp->renderer);

  g_clear_pointer (&session_rdp->sam_file, grd_rdp_sam_free_sam_file);

  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline) &&
      rdp_peer_context->network_autodetection)
    {
      grd_rdp_network_autodetection_ensure_rtt_consumer (
        rdp_peer_context->network_autodetection,
        GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_RDPGFX);
    }

  set_rdp_peer_flag (session_rdp, RDP_PEER_ACTIVATED);

  g_mutex_lock (&session_rdp->notify_post_connected_mutex);
  session_rdp->notify_post_connected_source_id =
    g_idle_add_once (notify_post_connected, session_rdp);
  g_mutex_unlock (&session_rdp->notify_post_connected_mutex);

  return TRUE;
}

static BOOL
rdp_peer_activate (freerdp_peer *peer)
{
  g_debug ("Activating client");

  return TRUE;
}

static void
rdp_peer_context_free (freerdp_peer   *peer,
                       RdpPeerContext *rdp_peer_context)
{
  if (!rdp_peer_context)
    return;

  g_clear_object (&rdp_peer_context->dvc_handler);

  if (rdp_peer_context->vcm != INVALID_HANDLE_VALUE)
    g_clear_pointer (&rdp_peer_context->vcm, WTSCloseServer);

  if (rdp_peer_context->encode_stream)
    {
      Stream_Free (rdp_peer_context->encode_stream, TRUE);
      rdp_peer_context->encode_stream = NULL;
    }

  g_clear_pointer (&rdp_peer_context->rfx_context, rfx_context_free);

  g_mutex_clear (&rdp_peer_context->channel_mutex);
}

static BOOL
rdp_peer_context_new (freerdp_peer   *peer,
                      RdpPeerContext *rdp_peer_context)
{
  g_mutex_init (&rdp_peer_context->channel_mutex);

  rdp_peer_context->rfx_context = rfx_context_new (TRUE);
  if (!rdp_peer_context->rfx_context)
    {
      g_warning ("[RDP] Failed to create RFX context");
      return FALSE;
    }
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  rfx_context_set_pixel_format (rdp_peer_context->rfx_context,
                                PIXEL_FORMAT_BGRX32);
#else
  rfx_context_set_pixel_format (rdp_peer_context->rfx_context,
                                PIXEL_FORMAT_XRGB32);
#endif

  rdp_peer_context->encode_stream = Stream_New (NULL, 64 * 64 * 4);
  if (!rdp_peer_context->encode_stream)
    {
      g_warning ("[RDP] Failed to create encode stream");
      return FALSE;
    }

  rdp_peer_context->vcm = WTSOpenServerA ((LPSTR) peer->context);
  if (!rdp_peer_context->vcm || rdp_peer_context->vcm == INVALID_HANDLE_VALUE)
    {
      g_warning ("[RDP] Failed to create virtual channel manager");
      return FALSE;
    }

  rdp_peer_context->dvc_handler =
    grd_rdp_dvc_handler_new (rdp_peer_context->vcm);

  return TRUE;
}

static gboolean
init_rdp_session (GrdSessionRdp  *session_rdp,
                  const char     *username,
                  const char     *password,
                  GError        **error)
{
  GrdContext *context = grd_session_get_context (GRD_SESSION (session_rdp));
  GrdSettings *settings = grd_context_get_settings (context);
  GSocket *socket = g_socket_connection_get_socket (session_rdp->connection);
  g_autofree char *server_cert = NULL;
  g_autofree char *server_key = NULL;
  gboolean use_client_configs;
  freerdp_peer *peer;
  rdpInput *rdp_input;
  rdpAutoDetect *rdp_autodetect;
  RdpPeerContext *rdp_peer_context;
  rdpSettings *rdp_settings;
  rdpCertificate *rdp_certificate;
  rdpPrivateKey *rdp_private_key;

  use_client_configs = session_rdp->screen_share_mode ==
                       GRD_RDP_SCREEN_SHARE_MODE_EXTEND;

  g_debug ("Initialize RDP session");

  peer = freerdp_peer_new (g_socket_get_fd (socket));
  if (!peer)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create peer");
      return FALSE;
    }

  peer->ContextSize = sizeof (RdpPeerContext);
  peer->ContextFree = (psPeerContextFree) rdp_peer_context_free;
  peer->ContextNew = (psPeerContextNew) rdp_peer_context_new;
  if (!freerdp_peer_context_new (peer))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create peer context");
      freerdp_peer_free (peer);
      return FALSE;
    }
  session_rdp->peer = peer;

  rdp_peer_context = (RdpPeerContext *) peer->context;
  rdp_peer_context->session_rdp = session_rdp;

  session_rdp->sam_file = grd_rdp_sam_create_sam_file (username, password);
  if (!session_rdp->sam_file)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create SAM database");
      return FALSE;
    }

  rdp_settings = peer->context->settings;
  if (!freerdp_settings_set_string (rdp_settings, FreeRDP_NtlmSamFile,
                                    session_rdp->sam_file->filename))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to set path of SAM database");
      return FALSE;
    }

  g_object_get (G_OBJECT (settings),
                "rdp-server-cert", &server_cert,
                "rdp-server-key", &server_key,
                NULL);

  rdp_certificate = freerdp_certificate_new_from_pem (server_cert);
  if (!rdp_certificate)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create certificate from file");
      return FALSE;
    }
  if (!freerdp_settings_set_pointer_len (rdp_settings,
                                         FreeRDP_RdpServerCertificate,
                                         rdp_certificate, 1))
    g_assert_not_reached ();

  rdp_private_key = freerdp_key_new_from_pem (server_key);
  if (!rdp_private_key)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create private key from file");
      return FALSE;
    }
  if (!freerdp_settings_set_pointer_len (rdp_settings,
                                         FreeRDP_RdpServerRsaKey,
                                         rdp_private_key, 1))
    g_assert_not_reached ();

  freerdp_settings_set_bool (rdp_settings, FreeRDP_RdpSecurity, FALSE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_TlsSecurity, FALSE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_NlaSecurity, TRUE);

  if (grd_context_get_runtime_mode (context) == GRD_RUNTIME_MODE_HANDOVER)
    {
      freerdp_settings_set_string (rdp_settings, FreeRDP_Username, username);
      freerdp_settings_set_string (rdp_settings, FreeRDP_Password, password);
      freerdp_settings_set_bool (rdp_settings, FreeRDP_RdstlsSecurity, TRUE);
    }

  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_OsMajorType,
                               OSMAJORTYPE_UNIX);
  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_OsMinorType,
                               OSMINORTYPE_PSEUDO_XSERVER);

  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_ColorDepth, 32);

  freerdp_settings_set_bool (rdp_settings, FreeRDP_GfxAVC444v2, FALSE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_GfxAVC444, FALSE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_GfxH264, FALSE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_GfxSmallCache, FALSE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_GfxThinClient, FALSE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline, TRUE);

  freerdp_settings_set_bool (rdp_settings, FreeRDP_RemoteFxCodec, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_RemoteFxImageCodec, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_NSCodec, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_SurfaceFrameMarkerEnabled, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);

  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_PointerCacheSize, 100);

  freerdp_settings_set_bool (rdp_settings, FreeRDP_FastPathOutput, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_NetworkAutoDetect, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_RefreshRect, FALSE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_SupportMonitorLayoutPdu,
                             use_client_configs);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_SupportMultitransport, FALSE);
  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_VCFlags, VCCAPS_COMPR_SC);
  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_VCChunkSize, 16256);

  freerdp_settings_set_bool (rdp_settings, FreeRDP_HasExtendedMouseEvent, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_HasHorizontalWheel, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_HasRelativeMouseEvent, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_HasQoeEvent, FALSE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_UnicodeInput, TRUE);

  freerdp_settings_set_bool (rdp_settings, FreeRDP_AudioCapture, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_AudioPlayback, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_RemoteConsoleAudio, TRUE);

  peer->Capabilities = rdp_peer_capabilities;
  peer->PostConnect = rdp_peer_post_connect;
  peer->Activate = rdp_peer_activate;

  peer->context->update->SuppressOutput = rdp_suppress_output;

  rdp_input = peer->context->input;
  rdp_input->SynchronizeEvent = rdp_input_synchronize_event;
  rdp_input->MouseEvent = rdp_input_mouse_event;
  rdp_input->ExtendedMouseEvent = rdp_input_extended_mouse_event;
  rdp_input->RelMouseEvent = rdp_input_rel_mouse_event;
  rdp_input->KeyboardEvent = rdp_input_keyboard_event;
  rdp_input->UnicodeKeyboardEvent = rdp_input_unicode_keyboard_event;

  rdp_autodetect = peer->context->autodetect;
  rdp_autodetect->OnConnectTimeAutoDetectBegin =
    rdp_autodetect_on_connect_time_autodetect_begin;

  if (!peer->Initialize (peer))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to initialize peer");
      return FALSE;
    }

  return TRUE;
}

gpointer
socket_thread_func (gpointer data)
{
  GrdSessionRdp *session_rdp = data;
  freerdp_peer *peer;
  RdpPeerContext *rdp_peer_context;
  HANDLE vcm;
  HANDLE channel_event;
  HANDLE events[32] = {};
  uint32_t n_events;
  uint32_t n_freerdp_handles;

  peer = session_rdp->peer;
  rdp_peer_context = (RdpPeerContext *) peer->context;
  vcm = rdp_peer_context->vcm;
  channel_event = WTSVirtualChannelManagerGetEventHandle (vcm);

  while (TRUE)
    {
      GrdRdpNetworkAutodetection *network_autodetection =
        rdp_peer_context->network_autodetection;
      gboolean pending_bw_measure_stop = FALSE;
      HANDLE bw_measure_stop_event = NULL;

      n_events = 0;

      events[n_events++] = session_rdp->stop_event;
      if (network_autodetection)
        {
          bw_measure_stop_event =
            grd_rdp_network_autodetection_get_bw_measure_stop_event_handle (network_autodetection);
          events[n_events++] = bw_measure_stop_event;
        }

      events[n_events++] = channel_event;

      n_freerdp_handles = peer->GetEventHandles (peer, &events[n_events],
                                                 32 - n_events);
      if (!n_freerdp_handles)
        {
          g_warning ("Failed to get FreeRDP transport event handles");
          handle_client_gone (session_rdp);
          break;
        }
      n_events += n_freerdp_handles;

      WaitForMultipleObjects (n_events, events, FALSE, INFINITE);

      if (session_rdp->session_should_stop)
        break;

      if (!peer->CheckFileDescriptor (peer))
        {
          g_message ("[RDP] Network or intentional disconnect, stopping session");
          handle_client_gone (session_rdp);
          break;
        }

      if (peer->connected &&
          WTSVirtualChannelManagerIsChannelJoined (vcm, DRDYNVC_SVC_CHANNEL_NAME))
        {
          GrdRdpDvcTelemetry *telemetry;
          GrdRdpDvcGraphicsPipeline *graphics_pipeline;
          GrdRdpDvcInput *input;
          GrdRdpDvcAudioPlayback *audio_playback;
          GrdRdpDvcDisplayControl *display_control;
          GrdRdpDvcAudioInput *audio_input;

          switch (WTSVirtualChannelManagerGetDrdynvcState (vcm))
            {
            case DRDYNVC_STATE_NONE:
              /*
               * This ensures that WTSVirtualChannelManagerCheckFileDescriptor()
               * will be called, which initializes the drdynvc channel
               */
              SetEvent (channel_event);
              break;
            case DRDYNVC_STATE_READY:
              g_mutex_lock (&rdp_peer_context->channel_mutex);
              telemetry = rdp_peer_context->telemetry;
              graphics_pipeline = rdp_peer_context->graphics_pipeline;
              input = rdp_peer_context->input;
              audio_playback = rdp_peer_context->audio_playback;
              display_control = rdp_peer_context->display_control;
              audio_input = rdp_peer_context->audio_input;

              if (telemetry && !session_rdp->session_should_stop)
                grd_rdp_dvc_maybe_init (GRD_RDP_DVC (telemetry));
              if (graphics_pipeline && !session_rdp->session_should_stop)
                grd_rdp_dvc_maybe_init (GRD_RDP_DVC (graphics_pipeline));
              if (input && !session_rdp->session_should_stop)
                grd_rdp_dvc_maybe_init (GRD_RDP_DVC (input));
              if (audio_playback && !session_rdp->session_should_stop)
                grd_rdp_dvc_maybe_init (GRD_RDP_DVC (audio_playback));
              if (display_control && !session_rdp->session_should_stop)
                grd_rdp_dvc_maybe_init (GRD_RDP_DVC (display_control));
              if (audio_input && !session_rdp->session_should_stop)
                grd_rdp_dvc_maybe_init (GRD_RDP_DVC (audio_input));
              g_mutex_unlock (&rdp_peer_context->channel_mutex);
              break;
            }

          if (session_rdp->session_should_stop)
            break;
        }

      if (bw_measure_stop_event)
        {
          pending_bw_measure_stop = WaitForSingleObject (bw_measure_stop_event,
                                                         0) == WAIT_OBJECT_0;
        }

      if (WaitForSingleObject (channel_event, 0) == WAIT_OBJECT_0 &&
          !WTSVirtualChannelManagerCheckFileDescriptor (vcm))
        {
          g_message ("Unable to check VCM file descriptor, closing connection");
          handle_client_gone (session_rdp);
          break;
        }

      if (pending_bw_measure_stop)
        grd_rdp_network_autodetection_bw_measure_stop (network_autodetection);
    }

  return NULL;
}

static void
on_view_only_changed (GrdSettings   *settings,
                      GParamSpec    *pspec,
                      GrdSessionRdp *session_rdp)
{
  g_object_get (G_OBJECT (settings),
                "rdp-view-only", &session_rdp->is_view_only,
                NULL);
}

GrdSessionRdp *
grd_session_rdp_new (GrdRdpServer      *rdp_server,
                     GSocketConnection *connection,
                     GrdHwAccelVulkan  *hwaccel_vulkan,
                     GrdHwAccelNvidia  *hwaccel_nvidia)
{
  g_autoptr (GrdSessionRdp) session_rdp = NULL;
  GrdContext *context;
  GrdSettings *settings;
  char *username;
  char *password;
  g_autoptr (GError) error = NULL;

  context = grd_rdp_server_get_context (rdp_server);
  settings = grd_context_get_settings (context);
  if (!grd_settings_get_rdp_credentials (settings,
                                         &username, &password,
                                         &error))
    {
      if (error)
        g_warning ("[RDP] Couldn't retrieve RDP credentials: %s", error->message);
      else
        g_message ("[RDP] Credentials are not set, denying client");

      return NULL;
    }

  session_rdp = g_object_new (GRD_TYPE_SESSION_RDP,
                              "context", context,
                              NULL);

  session_rdp->connection = g_object_ref (connection);
  session_rdp->hwaccel_vulkan = hwaccel_vulkan;
  session_rdp->hwaccel_nvidia = hwaccel_nvidia;

  g_object_get (G_OBJECT (settings),
                "rdp-screen-share-mode", &session_rdp->screen_share_mode,
                "rdp-view-only", &session_rdp->is_view_only,
                NULL);

  g_signal_connect (settings, "notify::rdp-view-only",
                    G_CALLBACK (on_view_only_changed),
                    session_rdp);

  session_rdp->renderer = grd_rdp_renderer_new (session_rdp, hwaccel_nvidia);
  session_rdp->layout_manager =
    grd_rdp_layout_manager_new (session_rdp,
                                session_rdp->renderer,
                                hwaccel_vulkan,
                                hwaccel_nvidia);

  if (!init_rdp_session (session_rdp, username, password, &error))
    {
      g_warning ("[RDP] Couldn't initialize session: %s", error->message);
      g_free (password);
      g_free (username);
      return NULL;
    }

  session_rdp->socket_thread = g_thread_new ("RDP socket thread",
                                             socket_thread_func,
                                             session_rdp);

  g_free (password);
  g_free (username);

  return g_steal_pointer (&session_rdp);
}

static gboolean
has_session_close_queued (GrdSessionRdp *session_rdp)
{
  gboolean pending_session_closing;

  g_mutex_lock (&session_rdp->close_session_mutex);
  pending_session_closing = session_rdp->close_session_idle_id != 0;
  g_mutex_unlock (&session_rdp->close_session_mutex);

  return pending_session_closing;
}

static void
clear_rdp_peer (GrdSessionRdp *session_rdp)
{
  g_clear_pointer (&session_rdp->sam_file, grd_rdp_sam_free_sam_file);

  if (session_rdp->peer)
    {
      freerdp_peer_context_free (session_rdp->peer);
      g_clear_pointer (&session_rdp->peer, freerdp_peer_free);
    }
}

static void
grd_session_rdp_stop (GrdSession *session)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;

  g_debug ("Stopping RDP session");

  unset_rdp_peer_flag (session_rdp, RDP_PEER_ACTIVATED);
  session_rdp->session_should_stop = TRUE;
  SetEvent (session_rdp->stop_event);

  if (!has_session_close_queued (session_rdp))
    {
      freerdp_set_error_info (peer->context->rdp,
                              ERRINFO_RPC_INITIATED_DISCONNECT);
    }
  else if (session_rdp->rdp_error_info)
    {
      freerdp_set_error_info (peer->context->rdp, session_rdp->rdp_error_info);
    }

  if (rdp_peer_context->network_autodetection)
    {
      grd_rdp_network_autodetection_invoke_shutdown (
        rdp_peer_context->network_autodetection);
    }

  grd_rdp_renderer_invoke_shutdown (session_rdp->renderer);

  g_mutex_lock (&rdp_peer_context->channel_mutex);
  g_clear_object (&rdp_peer_context->audio_input);
  g_clear_object (&rdp_peer_context->clipboard_rdp);
  g_clear_object (&rdp_peer_context->audio_playback);
  g_clear_object (&rdp_peer_context->display_control);
  g_clear_object (&rdp_peer_context->input);
  g_clear_object (&rdp_peer_context->graphics_pipeline);
  g_clear_object (&rdp_peer_context->telemetry);
  g_mutex_unlock (&rdp_peer_context->channel_mutex);

  g_clear_pointer (&session_rdp->socket_thread, g_thread_join);
  g_clear_handle_id (&session_rdp->notify_post_connected_source_id,
                     g_source_remove);

  g_hash_table_foreach_remove (session_rdp->pressed_keys,
                               notify_keycode_released,
                               session_rdp);
  g_hash_table_foreach_remove (session_rdp->pressed_unicode_keys,
                               notify_keysym_released,
                               session_rdp);
  grd_rdp_event_queue_flush (session_rdp->rdp_event_queue);

  g_clear_object (&session_rdp->layout_manager);

  g_clear_object (&session_rdp->cursor_renderer);
  g_clear_object (&session_rdp->renderer);

  peer->Close (peer);
  g_clear_object (&session_rdp->connection);

  g_clear_object (&rdp_peer_context->network_autodetection);

  peer->Disconnect (peer);
  clear_rdp_peer (session_rdp);

  g_clear_handle_id (&session_rdp->close_session_idle_id, g_source_remove);
}

static gboolean
close_session_idle (gpointer user_data)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (user_data);

  grd_session_stop (GRD_SESSION (session_rdp));

  session_rdp->close_session_idle_id = 0;

  return G_SOURCE_REMOVE;
}

static void
initialize_graphics_pipeline (GrdSessionRdp *session_rdp)
{
  rdpContext *rdp_context = session_rdp->peer->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  GrdRdpDvcGraphicsPipeline *graphics_pipeline;
  GrdRdpDvcTelemetry *telemetry;

  g_assert (!rdp_peer_context->telemetry);
  g_assert (!rdp_peer_context->graphics_pipeline);

  telemetry = grd_rdp_dvc_telemetry_new (session_rdp,
                                         rdp_peer_context->dvc_handler,
                                         rdp_peer_context->vcm,
                                         rdp_context);
  rdp_peer_context->telemetry = telemetry;

  graphics_pipeline =
    grd_rdp_dvc_graphics_pipeline_new (session_rdp,
                                       session_rdp->renderer,
                                       rdp_peer_context->dvc_handler,
                                       rdp_peer_context->vcm,
                                       rdp_context,
                                       rdp_peer_context->network_autodetection,
                                       rdp_peer_context->encode_stream,
                                       rdp_peer_context->rfx_context);
  grd_rdp_dvc_graphics_pipeline_set_hwaccel_nvidia (graphics_pipeline,
                                                    session_rdp->hwaccel_nvidia);
  rdp_peer_context->graphics_pipeline = graphics_pipeline;
}

static void
initialize_remaining_virtual_channels (GrdSessionRdp *session_rdp)
{
  rdpContext *rdp_context = session_rdp->peer->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  rdpSettings *rdp_settings = rdp_context->settings;
  GrdRdpDvcHandler *dvc_handler = rdp_peer_context->dvc_handler;
  HANDLE vcm = rdp_peer_context->vcm;

  rdp_peer_context->input =
    grd_rdp_dvc_input_new (session_rdp->layout_manager,
                           session_rdp, dvc_handler, vcm);

  if (session_rdp->screen_share_mode == GRD_RDP_SCREEN_SHARE_MODE_EXTEND)
    {
      rdp_peer_context->display_control =
        grd_rdp_dvc_display_control_new (session_rdp->layout_manager,
                                         session_rdp,
                                         dvc_handler,
                                         rdp_peer_context->vcm,
                                         get_max_monitor_count (session_rdp));
    }
  if (WTSVirtualChannelManagerIsChannelJoined (vcm, CLIPRDR_SVC_CHANNEL_NAME))
    {
      uint32_t os_major_type =
        freerdp_settings_get_uint32 (rdp_settings, FreeRDP_OsMajorType);
      gboolean peer_is_on_ms_windows;

      peer_is_on_ms_windows = os_major_type == OSMAJORTYPE_WINDOWS;
      rdp_peer_context->clipboard_rdp =
        grd_clipboard_rdp_new (session_rdp, vcm, !peer_is_on_ms_windows);
    }
  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_AudioPlayback) &&
      !freerdp_settings_get_bool (rdp_settings, FreeRDP_RemoteConsoleAudio))
    {
      rdp_peer_context->audio_playback =
        grd_rdp_dvc_audio_playback_new (session_rdp, dvc_handler, vcm,
                                        rdp_context);
    }
  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_AudioCapture))
    {
      rdp_peer_context->audio_input =
        grd_rdp_dvc_audio_input_new (session_rdp, dvc_handler, vcm,
                                     rdp_context);
    }
}

static void
on_remote_desktop_session_ready (GrdSession *session)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);

  grd_rdp_session_metrics_notify_phase_completion (session_rdp->session_metrics,
                                                   GRD_RDP_PHASE_SESSION_READY);

  session_rdp->cursor_renderer =
    grd_rdp_cursor_renderer_new (session_rdp->renderer,
                                 session_rdp->peer->context);
  grd_rdp_cursor_renderer_notify_session_ready (session_rdp->cursor_renderer);

  initialize_graphics_pipeline (session_rdp);
  initialize_remaining_virtual_channels (session_rdp);
}

static void
on_remote_desktop_session_started (GrdSession *session)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);
  rdpContext *rdp_context = session_rdp->peer->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  GrdRdpDvcGraphicsPipeline *graphics_pipeline =
    rdp_peer_context->graphics_pipeline;

  if (!grd_rdp_renderer_start (session_rdp->renderer,
                               session_rdp->hwaccel_vulkan,
                               graphics_pipeline, rdp_context))
    {
      grd_session_rdp_notify_error (session_rdp,
                                    GRD_SESSION_RDP_ERROR_CLOSE_STACK_ON_DRIVER_FAILURE);
      return;
    }

  grd_rdp_session_metrics_notify_phase_completion (session_rdp->session_metrics,
                                                   GRD_RDP_PHASE_SESSION_STARTED);
  grd_rdp_layout_manager_notify_session_started (session_rdp->layout_manager,
                                                 session_rdp->cursor_renderer,
                                                 rdp_context,
                                                 session_rdp->screen_share_mode);
  grd_rdp_event_queue_flush_synchronization (session_rdp->rdp_event_queue);
}

static void
grd_session_rdp_on_stream_created (GrdSession *session,
                                   uint32_t    stream_id,
                                   GrdStream  *stream)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);
  GrdRdpStreamOwner *stream_owner = NULL;

  if (!g_hash_table_lookup_extended (session_rdp->stream_table,
                                     GUINT_TO_POINTER (stream_id),
                                     NULL, (gpointer) &stream_owner))
    g_assert_not_reached ();

  grd_rdp_stream_owner_notify_stream_created (stream_owner, stream_id, stream);
}

static void
grd_session_rdp_on_caps_lock_state_changed (GrdSession *session,
                                            gboolean    state)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;

  grd_rdp_event_queue_update_caps_lock_state (rdp_event_queue, state);
}

static void
grd_session_rdp_on_num_lock_state_changed (GrdSession *session,
                                           gboolean    state)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;

  grd_rdp_event_queue_update_num_lock_state (rdp_event_queue, state);
}

static void
grd_session_rdp_dispose (GObject *object)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (object);

  g_assert (!session_rdp->notify_post_connected_source_id);
  g_assert (!session_rdp->cursor_renderer);

  g_clear_object (&session_rdp->layout_manager);
  clear_rdp_peer (session_rdp);
  g_clear_object (&session_rdp->connection);

  g_clear_object (&session_rdp->renderer);

  g_clear_object (&session_rdp->rdp_event_queue);
  g_clear_object (&session_rdp->session_metrics);

  g_clear_pointer (&session_rdp->stream_table, g_hash_table_unref);
  g_clear_pointer (&session_rdp->pressed_unicode_keys, g_hash_table_unref);
  g_clear_pointer (&session_rdp->pressed_keys, g_hash_table_unref);

  g_clear_pointer (&session_rdp->stop_event, CloseHandle);

  G_OBJECT_CLASS (grd_session_rdp_parent_class)->dispose (object);
}

static void
grd_session_rdp_finalize (GObject *object)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (object);

  g_mutex_clear (&session_rdp->notify_post_connected_mutex);
  g_mutex_clear (&session_rdp->close_session_mutex);
  g_mutex_clear (&session_rdp->rdp_flags_mutex);

  G_OBJECT_CLASS (grd_session_rdp_parent_class)->finalize (object);
}

static void
grd_session_rdp_init (GrdSessionRdp *session_rdp)
{
  session_rdp->stop_event = CreateEvent (NULL, TRUE, FALSE, NULL);

  session_rdp->pressed_keys = g_hash_table_new (NULL, NULL);
  session_rdp->pressed_unicode_keys = g_hash_table_new (NULL, NULL);
  session_rdp->stream_table = g_hash_table_new (NULL, NULL);

  g_mutex_init (&session_rdp->rdp_flags_mutex);
  g_mutex_init (&session_rdp->close_session_mutex);
  g_mutex_init (&session_rdp->notify_post_connected_mutex);

  session_rdp->session_metrics = grd_rdp_session_metrics_new ();
  session_rdp->rdp_event_queue = grd_rdp_event_queue_new (session_rdp);

  g_signal_connect (session_rdp, "ready",
                    G_CALLBACK (on_remote_desktop_session_ready), NULL);
  g_signal_connect (session_rdp, "started",
                    G_CALLBACK (on_remote_desktop_session_started), NULL);
}

static void
grd_session_rdp_class_init (GrdSessionRdpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdSessionClass *session_class = GRD_SESSION_CLASS (klass);

  object_class->dispose = grd_session_rdp_dispose;
  object_class->finalize = grd_session_rdp_finalize;

  session_class->stop = grd_session_rdp_stop;
  session_class->on_stream_created = grd_session_rdp_on_stream_created;
  session_class->on_caps_lock_state_changed =
    grd_session_rdp_on_caps_lock_state_changed;
  session_class->on_num_lock_state_changed =
    grd_session_rdp_on_num_lock_state_changed;

  signals[POST_CONNECTED] = g_signal_new ("post-connected",
                                          G_TYPE_FROM_CLASS (klass),
                                          G_SIGNAL_RUN_LAST,
                                          0,
                                          NULL, NULL, NULL,
                                          G_TYPE_NONE, 0);
}
