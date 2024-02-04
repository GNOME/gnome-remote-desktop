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
#include "grd-debug.h"
#include "grd-hwaccel-nvidia.h"
#include "grd-rdp-audio-input.h"
#include "grd-rdp-audio-playback.h"
#include "grd-rdp-buffer.h"
#include "grd-rdp-cursor-renderer.h"
#include "grd-rdp-damage-detector.h"
#include "grd-rdp-display-control.h"
#include "grd-rdp-dvc.h"
#include "grd-rdp-event-queue.h"
#include "grd-rdp-graphics-pipeline.h"
#include "grd-rdp-layout-manager.h"
#include "grd-rdp-network-autodetection.h"
#include "grd-rdp-private.h"
#include "grd-rdp-renderer.h"
#include "grd-rdp-sam.h"
#include "grd-rdp-server.h"
#include "grd-rdp-session-metrics.h"
#include "grd-rdp-surface.h"
#include "grd-rdp-telemetry.h"
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

typedef struct _NSCThreadPoolContext
{
  uint32_t pending_job_count;
  GCond *pending_jobs_cond;
  GMutex *pending_jobs_mutex;

  uint32_t src_stride;
  uint8_t *src_data;
  rdpSettings *rdp_settings;
} NSCThreadPoolContext;

typedef struct _NSCEncodeContext
{
  cairo_rectangle_int_t cairo_rect;
  wStream *stream;
} NSCEncodeContext;

typedef struct _RawThreadPoolContext
{
  uint32_t pending_job_count;
  GCond *pending_jobs_cond;
  GMutex *pending_jobs_mutex;

  uint16_t planar_flags;
  uint32_t src_stride;
  uint8_t *src_data;
} RawThreadPoolContext;

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

  GThreadPool *thread_pool;
  GCond pending_jobs_cond;
  GMutex pending_jobs_mutex;
  NSCThreadPoolContext nsc_thread_pool_context;
  RawThreadPoolContext raw_thread_pool_context;

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

static void
rdp_peer_refresh_region (GrdSessionRdp       *session_rdp,
                         GrdRdpSurface       *rdp_surface,
                         GrdRdpRenderContext *render_context,
                         GrdRdpBuffer        *buffer);

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

void
grd_session_rdp_maybe_encode_pending_frame (GrdSessionRdp       *session_rdp,
                                            GrdRdpSurface       *rdp_surface,
                                            GrdRdpRenderContext *render_context)
{
  GrdRdpBuffer *buffer;

  g_assert (session_rdp->peer);

  if (!is_rdp_peer_flag_set (session_rdp, RDP_PEER_ACTIVATED))
    return;

  buffer = g_steal_pointer (&rdp_surface->pending_framebuffer);
  if (!grd_rdp_damage_detector_submit_new_framebuffer (rdp_surface->detector,
                                                       buffer))
    {
      grd_rdp_buffer_release (buffer);
      grd_session_rdp_notify_error (session_rdp,
                                    GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED);
      return;
    }

  if (!grd_rdp_damage_detector_is_region_damaged (rdp_surface->detector))
    return;

  rdp_peer_refresh_region (session_rdp, rdp_surface, render_context, buffer);
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
rdp_peer_refresh_gfx (GrdSessionRdp       *session_rdp,
                      GrdRdpSurface       *rdp_surface,
                      GrdRdpRenderContext *render_context,
                      GrdRdpBuffer        *buffer)
{
  rdpContext *rdp_context = session_rdp->peer->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  GrdRdpGraphicsPipeline *graphics_pipeline = rdp_peer_context->graphics_pipeline;

  return grd_rdp_graphics_pipeline_refresh_gfx (graphics_pipeline, rdp_surface,
                                                render_context, buffer);
}

static gboolean
rdp_peer_refresh_rfx (GrdSessionRdp  *session_rdp,
                      GrdRdpSurface  *rdp_surface,
                      cairo_region_t *region,
                      GrdRdpBuffer   *buffer)
{
  rdpContext *rdp_context = session_rdp->peer->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  rdpSettings *rdp_settings = rdp_context->settings;
  rdpUpdate *rdp_update = rdp_context->update;
  uint32_t multifrag_max_request_size =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_MultifragMaxRequestSize);
  uint32_t remote_fx_codec_id =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_RemoteFxCodecId);
  gboolean has_surface_frame_marker =
    freerdp_settings_get_bool (rdp_settings, FreeRDP_SurfaceFrameMarkerEnabled);
  GrdRdpSurfaceMapping *surface_mapping;
  uint8_t *data = grd_rdp_buffer_get_local_data (buffer);
  uint32_t surface_width = grd_rdp_surface_get_width (rdp_surface);
  uint32_t surface_height = grd_rdp_surface_get_height (rdp_surface);
  uint32_t src_stride = grd_rdp_buffer_get_stride (buffer);
  SURFACE_BITS_COMMAND cmd = {0};
  cairo_rectangle_int_t cairo_rect;
  RFX_RECT *rfx_rects, *rfx_rect;
  int n_rects;
  RFX_MESSAGE_LIST *rfx_messages;
  size_t n_messages;
  BOOL first, last;
  size_t i;

  surface_mapping = grd_rdp_surface_get_mapping (rdp_surface);

  rfx_context_set_mode (rdp_peer_context->rfx_context, RLGR3);

  n_rects = cairo_region_num_rectangles (region);
  rfx_rects = g_malloc0 (n_rects * sizeof (RFX_RECT));
  for (i = 0; i < n_rects; ++i)
    {
      cairo_region_get_rectangle (region, i, &cairo_rect);

      rfx_rect = &rfx_rects[i];
      rfx_rect->x = cairo_rect.x;
      rfx_rect->y = cairo_rect.y;
      rfx_rect->width = cairo_rect.width;
      rfx_rect->height = cairo_rect.height;
    }

  rfx_messages = rfx_encode_messages (rdp_peer_context->rfx_context,
                                      rfx_rects,
                                      n_rects,
                                      data,
                                      surface_width,
                                      surface_height,
                                      src_stride,
                                      &n_messages,
                                      multifrag_max_request_size);

  cmd.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
  cmd.bmp.codecID = remote_fx_codec_id;
  cmd.destLeft = surface_mapping->output_origin_x;
  cmd.destTop = surface_mapping->output_origin_y;
  cmd.destRight = cmd.destLeft + surface_width;
  cmd.destBottom = cmd.destTop + surface_height;
  cmd.bmp.bpp = 32;
  cmd.bmp.flags = 0;
  cmd.bmp.width = surface_width;
  cmd.bmp.height = surface_height;

  for (i = 0; i < n_messages; ++i)
    {
      const RFX_MESSAGE *rfx_message = rfx_message_list_get (rfx_messages, i);

      Stream_SetPosition (rdp_peer_context->encode_stream, 0);

      if (!rfx_write_message (rdp_peer_context->rfx_context,
                              rdp_peer_context->encode_stream,
                              rfx_message))
        {
          g_warning ("[RDP] Failed to write RFX message");
          break;
        }

      cmd.bmp.bitmapDataLength = Stream_GetPosition (rdp_peer_context->encode_stream);
      cmd.bmp.bitmapData = Stream_Buffer (rdp_peer_context->encode_stream);
      first = i == 0 ? TRUE : FALSE;
      last = i + 1 == n_messages ? TRUE : FALSE;

      if (!has_surface_frame_marker)
        rdp_update->SurfaceBits (rdp_update->context, &cmd);
      else
        rdp_update->SurfaceFrameBits (rdp_update->context, &cmd, first, last,
                                      rdp_peer_context->frame_id);
    }

  g_clear_pointer (&rfx_messages, rfx_message_list_free);
  g_free (rfx_rects);

  return TRUE;
}

static void
rdp_peer_encode_nsc_rect (gpointer data,
                          gpointer user_data)
{
  NSCThreadPoolContext *thread_pool_context = (NSCThreadPoolContext *) user_data;
  NSCEncodeContext *encode_context = (NSCEncodeContext *) data;
  cairo_rectangle_int_t *cairo_rect = &encode_context->cairo_rect;
  uint32_t src_stride = thread_pool_context->src_stride;
  uint8_t *src_data = thread_pool_context->src_data;
  rdpSettings *rdp_settings = thread_pool_context->rdp_settings;
  uint32_t color_loss_level =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_NSCodecColorLossLevel);
  gboolean allow_subsampling =
    freerdp_settings_get_bool (rdp_settings, FreeRDP_NSCodecAllowSubsampling);
  gboolean allow_dynamic_color_fidelity =
    freerdp_settings_get_bool (rdp_settings, FreeRDP_NSCodecAllowDynamicColorFidelity);
  NSC_CONTEXT *nsc_context;
  uint8_t *src_data_at_pos;

  nsc_context = nsc_context_new ();
  nsc_context_set_parameters (nsc_context, NSC_COLOR_LOSS_LEVEL,
                              color_loss_level);
  nsc_context_set_parameters (nsc_context, NSC_ALLOW_SUBSAMPLING,
                              allow_subsampling);
  nsc_context_set_parameters (nsc_context, NSC_DYNAMIC_COLOR_FIDELITY,
                              allow_dynamic_color_fidelity);
  nsc_context_set_parameters (nsc_context,
                              NSC_COLOR_FORMAT,
                              PIXEL_FORMAT_BGRX32);
  nsc_context_reset (nsc_context,
                     cairo_rect->width,
                     cairo_rect->height);

  encode_context->stream = Stream_New (NULL, 64 * 64 * 4);

  src_data_at_pos = &src_data[cairo_rect->y * src_stride + cairo_rect->x * 4];
  Stream_SetPosition (encode_context->stream, 0);
  nsc_compose_message (nsc_context,
                       encode_context->stream,
                       src_data_at_pos,
                       cairo_rect->width,
                       cairo_rect->height,
                       src_stride);

  nsc_context_free (nsc_context);

  g_mutex_lock (thread_pool_context->pending_jobs_mutex);
  --thread_pool_context->pending_job_count;

  if (!thread_pool_context->pending_job_count)
    g_cond_signal (thread_pool_context->pending_jobs_cond);
  g_mutex_unlock (thread_pool_context->pending_jobs_mutex);
}

static gboolean
rdp_peer_refresh_nsc (GrdSessionRdp  *session_rdp,
                      GrdRdpSurface  *rdp_surface,
                      cairo_region_t *region,
                      GrdRdpBuffer   *buffer)
{
  rdpContext *rdp_context = session_rdp->peer->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  rdpSettings *rdp_settings = rdp_context->settings;
  rdpUpdate *rdp_update = rdp_context->update;
  uint32_t ns_codec_id =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_NSCodecId);
  gboolean has_surface_frame_marker =
    freerdp_settings_get_bool (rdp_settings, FreeRDP_SurfaceFrameMarkerEnabled);
  GrdRdpSurfaceMapping *surface_mapping;
  uint8_t *data = grd_rdp_buffer_get_local_data (buffer);
  uint32_t src_stride = grd_rdp_buffer_get_stride (buffer);
  NSCThreadPoolContext *thread_pool_context =
    &session_rdp->nsc_thread_pool_context;
  g_autoptr (GError) error = NULL;
  cairo_rectangle_int_t *cairo_rect;
  int n_rects;
  NSCEncodeContext *encode_contexts;
  NSCEncodeContext *encode_context;
  SURFACE_BITS_COMMAND cmd = {0};
  BOOL first, last;
  int i;

  surface_mapping = grd_rdp_surface_get_mapping (rdp_surface);

  n_rects = cairo_region_num_rectangles (region);
  encode_contexts = g_malloc0 (n_rects * sizeof (NSCEncodeContext));

  thread_pool_context->pending_job_count = 0;
  thread_pool_context->pending_jobs_cond = &session_rdp->pending_jobs_cond;
  thread_pool_context->pending_jobs_mutex = &session_rdp->pending_jobs_mutex;
  thread_pool_context->src_stride = src_stride;
  thread_pool_context->src_data = data;
  thread_pool_context->rdp_settings = rdp_settings;

  if (!session_rdp->thread_pool)
    session_rdp->thread_pool = g_thread_pool_new (rdp_peer_encode_nsc_rect,
                                                  thread_pool_context,
                                                  g_get_num_processors (),
                                                  TRUE,
                                                  &error);
  if (!session_rdp->thread_pool)
    {
      g_free (encode_contexts);
      g_error ("Couldn't create thread pool: %s", error->message);
    }

  g_mutex_lock (&session_rdp->pending_jobs_mutex);
  for (i = 0; i < n_rects; ++i)
    {
      encode_context = &encode_contexts[i];
      cairo_region_get_rectangle (region, i, &encode_context->cairo_rect);

      ++thread_pool_context->pending_job_count;
      g_thread_pool_push (session_rdp->thread_pool, encode_context, NULL);
    }

  while (thread_pool_context->pending_job_count)
    {
      g_cond_wait (&session_rdp->pending_jobs_cond,
                   &session_rdp->pending_jobs_mutex);
    }
  g_mutex_unlock (&session_rdp->pending_jobs_mutex);

  cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
  cmd.bmp.bpp = 32;
  cmd.bmp.codecID = ns_codec_id;

  for (i = 0; i < n_rects; ++i)
    {
      encode_context = &encode_contexts[i];
      cairo_rect = &encode_context->cairo_rect;

      cmd.destLeft = surface_mapping->output_origin_x + cairo_rect->x;
      cmd.destTop = surface_mapping->output_origin_y + cairo_rect->y;
      cmd.destRight = cmd.destLeft + cairo_rect->width;
      cmd.destBottom = cmd.destTop + cairo_rect->height;
      cmd.bmp.width = cairo_rect->width;
      cmd.bmp.height = cairo_rect->height;
      cmd.bmp.bitmapDataLength = Stream_GetPosition (encode_context->stream);
      cmd.bmp.bitmapData = Stream_Buffer (encode_context->stream);
      first = i == 0 ? TRUE : FALSE;
      last = i + 1 == n_rects ? TRUE : FALSE;

      if (!has_surface_frame_marker)
        rdp_update->SurfaceBits (rdp_update->context, &cmd);
      else
        rdp_update->SurfaceFrameBits (rdp_update->context, &cmd, first, last,
                                      rdp_peer_context->frame_id);

      Stream_Free (encode_context->stream, TRUE);
    }

  g_free (encode_contexts);

  return TRUE;
}

static void
rdp_peer_compress_raw_tile (gpointer data,
                            gpointer user_data)
{
  RawThreadPoolContext *thread_pool_context = (RawThreadPoolContext *) user_data;
  BITMAP_DATA *dst_bitmap = (BITMAP_DATA *) data;
  uint16_t planar_flags = thread_pool_context->planar_flags;
  uint32_t dst_bpp = dst_bitmap->bitsPerPixel;
  uint32_t dst_Bpp = (dst_bpp + 7) / 8;
  BITMAP_PLANAR_CONTEXT *planar_context;
  BITMAP_INTERLEAVED_CONTEXT *interleaved_context;
  uint32_t src_stride;
  uint32_t src_data_pos;
  uint8_t *src_data;
  uint8_t *src_data_at_pos;

  dst_bitmap->bitmapLength = 64 * 64 * 4 * sizeof (uint8_t);
  dst_bitmap->bitmapDataStream = g_malloc0 (dst_bitmap->bitmapLength);

  src_stride = thread_pool_context->src_stride;
  src_data_pos = dst_bitmap->destTop * src_stride + dst_bitmap->destLeft * dst_Bpp;
  src_data = thread_pool_context->src_data;
  src_data_at_pos = &src_data[src_data_pos];

  switch (dst_bpp)
    {
    case 32:
      planar_context = freerdp_bitmap_planar_context_new (planar_flags, 64, 64);
      freerdp_bitmap_planar_context_reset (planar_context, 64, 64);

      dst_bitmap->bitmapDataStream =
        freerdp_bitmap_compress_planar (planar_context,
                                        src_data_at_pos,
                                        PIXEL_FORMAT_BGRX32,
                                        dst_bitmap->width,
                                        dst_bitmap->height,
                                        src_stride,
                                        dst_bitmap->bitmapDataStream,
                                        &dst_bitmap->bitmapLength);

      freerdp_bitmap_planar_context_free (planar_context);
      break;
    case 24:
    case 16:
    case 15:
      interleaved_context = bitmap_interleaved_context_new (TRUE);
      bitmap_interleaved_context_reset (interleaved_context);

      interleaved_compress (interleaved_context,
                            dst_bitmap->bitmapDataStream,
                            &dst_bitmap->bitmapLength,
                            dst_bitmap->width,
                            dst_bitmap->height,
                            src_data,
                            PIXEL_FORMAT_BGRX32,
                            src_stride,
                            dst_bitmap->destLeft,
                            dst_bitmap->destTop,
                            NULL,
                            dst_bpp);

      bitmap_interleaved_context_free (interleaved_context);
      break;
    }

  dst_bitmap->cbScanWidth = dst_bitmap->width * dst_Bpp;
  dst_bitmap->cbUncompressedSize = dst_bitmap->width * dst_bitmap->height * dst_Bpp;
  dst_bitmap->cbCompFirstRowSize = 0;
  dst_bitmap->cbCompMainBodySize = dst_bitmap->bitmapLength;
  dst_bitmap->compressed = TRUE;
  dst_bitmap->flags = 0;

  g_mutex_lock (thread_pool_context->pending_jobs_mutex);
  --thread_pool_context->pending_job_count;

  if (!thread_pool_context->pending_job_count)
    g_cond_signal (thread_pool_context->pending_jobs_cond);
  g_mutex_unlock (thread_pool_context->pending_jobs_mutex);
}

static void
rdp_peer_refresh_raw_rect (rdpContext            *rdp_context,
                           GrdRdpSurface         *rdp_surface,
                           GThreadPool           *thread_pool,
                           uint32_t              *pending_job_count,
                           BITMAP_DATA           *bitmap_data,
                           uint32_t              *n_bitmaps,
                           cairo_rectangle_int_t *cairo_rect,
                           uint8_t               *data)
{
  rdpSettings *rdp_settings = rdp_context->settings;
  uint32_t dst_bits_per_pixel =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_ColorDepth);
  GrdRdpSurfaceMapping *surface_mapping;
  uint32_t cols, rows;
  uint32_t x, y;
  BITMAP_DATA *bitmap;

  surface_mapping = grd_rdp_surface_get_mapping (rdp_surface);

  cols = cairo_rect->width / 64 + (cairo_rect->width % 64 ? 1 : 0);
  rows = cairo_rect->height / 64 + (cairo_rect->height % 64 ? 1 : 0);

  /* Both x- and y-position of each bitmap need to be a multiple of 4 */
  if (cairo_rect->x % 4)
    {
      cairo_rect->width += cairo_rect->x % 4;
      cairo_rect->x -= cairo_rect->x % 4;
    }
  if (cairo_rect->y % 4)
    {
      cairo_rect->height += cairo_rect->y % 4;
      cairo_rect->y -= cairo_rect->y % 4;
    }

  /* Both width and height of each bitmap need to be a multiple of 4 */
  if (cairo_rect->width % 4)
    cairo_rect->width += 4 - cairo_rect->width % 4;
  if (cairo_rect->height % 4)
    cairo_rect->height += 4 - cairo_rect->height % 4;

  for (y = 0; y < rows; ++y)
    {
      for (x = 0; x < cols; ++x)
        {
          bitmap = &bitmap_data[(*n_bitmaps)++];
          bitmap->width = 64;
          bitmap->height = 64;
          bitmap->destLeft = surface_mapping->output_origin_x + cairo_rect->x + x * 64;
          bitmap->destTop = surface_mapping->output_origin_y + cairo_rect->y + y * 64;

          if (bitmap->destLeft + bitmap->width > cairo_rect->x + cairo_rect->width)
            bitmap->width = cairo_rect->x + cairo_rect->width - bitmap->destLeft;
          if (bitmap->destTop + bitmap->height > cairo_rect->y + cairo_rect->height)
            bitmap->height = cairo_rect->y + cairo_rect->height - bitmap->destTop;

          bitmap->destRight = bitmap->destLeft + bitmap->width - 1;
          bitmap->destBottom = bitmap->destTop + bitmap->height - 1;
          bitmap->bitsPerPixel = dst_bits_per_pixel;

          /* Both width and height of each bitmap need to be a multiple of 4 */
          if (bitmap->width < 4 || bitmap->height < 4)
            continue;

          ++*pending_job_count;
          g_thread_pool_push (thread_pool, bitmap, NULL);
        }
    }
}

static gboolean
rdp_peer_refresh_raw (GrdSessionRdp  *session_rdp,
                      GrdRdpSurface  *rdp_surface,
                      cairo_region_t *region,
                      GrdRdpBuffer   *buffer)
{
  rdpContext *rdp_context = session_rdp->peer->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  rdpSettings *rdp_settings = rdp_context->settings;
  rdpUpdate *rdp_update = rdp_context->update;
  uint32_t multifrag_max_request_size =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_MultifragMaxRequestSize);
  gboolean has_surface_frame_marker =
    freerdp_settings_get_bool (rdp_settings, FreeRDP_SurfaceFrameMarkerEnabled);
  uint8_t *data = grd_rdp_buffer_get_local_data (buffer);
  uint32_t src_stride = grd_rdp_buffer_get_stride (buffer);
  RawThreadPoolContext *thread_pool_context =
    &session_rdp->raw_thread_pool_context;
  g_autoptr (GError) error = NULL;
  uint32_t bitmap_data_count = 0;
  uint32_t n_bitmaps = 0;
  uint32_t update_size = 0;
  cairo_rectangle_int_t cairo_rect;
  int n_rects;
  BITMAP_DATA *bitmap_data;
  uint32_t cols, rows;
  SURFACE_FRAME_MARKER marker;
  BITMAP_UPDATE bitmap_update = {0};
  uint32_t max_update_size;
  uint32_t next_size;
  int i;

  n_rects = cairo_region_num_rectangles (region);
  for (i = 0; i < n_rects; ++i)
    {
      cairo_region_get_rectangle (region, i, &cairo_rect);
      cols = cairo_rect.width / 64 + (cairo_rect.width % 64 ? 1 : 0);
      rows = cairo_rect.height / 64 + (cairo_rect.height % 64 ? 1 : 0);
      bitmap_data_count += cols * rows;
    }

  bitmap_data = g_malloc0 (bitmap_data_count * sizeof (BITMAP_DATA));

  thread_pool_context->pending_job_count = 0;
  thread_pool_context->pending_jobs_cond = &session_rdp->pending_jobs_cond;
  thread_pool_context->pending_jobs_mutex = &session_rdp->pending_jobs_mutex;
  thread_pool_context->planar_flags = rdp_peer_context->planar_flags;
  thread_pool_context->src_stride = src_stride;
  thread_pool_context->src_data = data;

  if (!session_rdp->thread_pool)
    session_rdp->thread_pool = g_thread_pool_new (rdp_peer_compress_raw_tile,
                                                  thread_pool_context,
                                                  g_get_num_processors (),
                                                  TRUE,
                                                  &error);
  if (!session_rdp->thread_pool)
    {
      g_free (bitmap_data);
      g_error ("Couldn't create thread pool: %s", error->message);
    }

  g_mutex_lock (&session_rdp->pending_jobs_mutex);
  for (i = 0; i < n_rects; ++i)
    {
      cairo_region_get_rectangle (region, i, &cairo_rect);
      rdp_peer_refresh_raw_rect (rdp_context,
                                 rdp_surface,
                                 session_rdp->thread_pool,
                                 &thread_pool_context->pending_job_count,
                                 bitmap_data,
                                 &n_bitmaps,
                                 &cairo_rect,
                                 data);
    }

  while (thread_pool_context->pending_job_count)
    {
      g_cond_wait (&session_rdp->pending_jobs_cond,
                   &session_rdp->pending_jobs_mutex);
    }
  g_mutex_unlock (&session_rdp->pending_jobs_mutex);

  /* We send 2 additional Bytes for the update header
   * (See also update_write_bitmap_update () in FreeRDP)
   */
  max_update_size = multifrag_max_request_size - 2;

  bitmap_update.number = 0;
  bitmap_update.rectangles = bitmap_data;
  bitmap_update.skipCompression = FALSE;

  marker.frameId = rdp_peer_context->frame_id;
  marker.frameAction = SURFACECMD_FRAMEACTION_BEGIN;
  if (has_surface_frame_marker)
    rdp_update->SurfaceFrameMarker (rdp_context, &marker);

  for (i = 0; i < n_bitmaps; ++i)
    {
      /* We send 26 additional Bytes for each bitmap
       * (See also update_write_bitmap_data () in FreeRDP)
       */
      update_size += bitmap_data[i].bitmapLength + 26;

      ++bitmap_update.number;

      next_size = i + 1 < n_bitmaps ? bitmap_data[i + 1].bitmapLength + 26
                                    : 0;
      if (!next_size || update_size + next_size > max_update_size)
        {
          rdp_update->BitmapUpdate (rdp_context, &bitmap_update);

          bitmap_update.rectangles += bitmap_update.number;
          bitmap_update.number = 0;
          update_size = 0;
        }
    }

  marker.frameAction = SURFACECMD_FRAMEACTION_END;
  if (has_surface_frame_marker)
    rdp_update->SurfaceFrameMarker (rdp_context, &marker);

  for (i = 0; i < n_bitmaps; ++i)
    g_free (bitmap_data[i].bitmapDataStream);

  g_free (bitmap_data);

  return TRUE;
}

static void
rdp_peer_refresh_region (GrdSessionRdp       *session_rdp,
                         GrdRdpSurface       *rdp_surface,
                         GrdRdpRenderContext *render_context,
                         GrdRdpBuffer        *buffer)
{
  rdpContext *rdp_context = session_rdp->peer->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  rdpSettings *rdp_settings = rdp_context->settings;
  cairo_region_t *region = NULL;
  gboolean success;

  if (!freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline))
    {
      region = grd_rdp_damage_detector_get_damage_region (rdp_surface->detector);
      if (!region)
        {
          grd_session_rdp_notify_error (
            session_rdp, GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED);
          return;
        }
    }

  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline))
    success = rdp_peer_refresh_gfx (session_rdp, rdp_surface, render_context, buffer);
  else if (freerdp_settings_get_bool (rdp_settings, FreeRDP_RemoteFxCodec))
    success = rdp_peer_refresh_rfx (session_rdp, rdp_surface, region, buffer);
  else if (freerdp_settings_get_bool (rdp_settings, FreeRDP_NSCodec))
    success = rdp_peer_refresh_nsc (session_rdp, rdp_surface, region, buffer);
  else
    success = rdp_peer_refresh_raw (session_rdp, rdp_surface, region, buffer);

  g_clear_pointer (&region, cairo_region_destroy);

  ++rdp_peer_context->frame_id;

  if (success)
    {
      GrdRdpSessionMetrics *session_metrics = session_rdp->session_metrics;

      grd_rdp_session_metrics_notify_frame_transmission (session_metrics,
                                                         rdp_surface);
    }
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
  GrdEventPointerMotionAbs motion_abs = {};
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
  vkcode = GetVirtualKeyCodeFromVirtualScanCode (fullcode, 4);
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
  GrdRdpMonitorConfig *monitor_config;
  g_autoptr (GError) error = NULL;

  if (!freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline) &&
      !(grd_get_debug_flags () & GRD_DEBUG_RDP_LEGACY_GRAPHICS))
    {
      g_warning ("[RDP] Client did not advertise support for the Graphics "
                 "Pipeline, closing connection");
      return FALSE;
    }

  if (session_rdp->screen_share_mode == GRD_RDP_SCREEN_SHARE_MODE_EXTEND &&
      !freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline))
    {
      g_warning ("[RDP] Sessions with client monitor configurations require "
                 "the Graphics Pipeline, closing connection");
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

  switch (freerdp_settings_get_uint32 (rdp_settings, FreeRDP_ColorDepth))
    {
    case 32:
      break;
    case 24:
      /* Using the interleaved codec with a colour depth of 24
       * leads to bitmaps with artifacts.
       */
      g_message ("Downgrading colour depth from 24 to 16 due to issues with "
                 "the interleaved codec");
      freerdp_settings_set_uint32 (rdp_settings, FreeRDP_ColorDepth, 16);
    case 16:
    case 15:
      break;
    default:
      g_warning ("Invalid colour depth, closing connection");
      return FALSE;
    }

  if (!freerdp_settings_get_bool (rdp_settings, FreeRDP_DesktopResize))
    {
      g_warning ("Client doesn't support desktop resizing, closing connection");
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
  uint32_t multifrag_max_request_size =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_MultifragMaxRequestSize);
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

  g_debug ("[RDP] Virtual Channels: compression flags: %u, "
           "compression level: %u, chunk size: %u",
           freerdp_settings_get_uint32 (rdp_settings, FreeRDP_VCFlags),
           freerdp_settings_get_uint32 (rdp_settings, FreeRDP_CompressionLevel),
           freerdp_settings_get_uint32 (rdp_settings, FreeRDP_VCChunkSize));

  if (!freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline) &&
      !freerdp_settings_get_bool (rdp_settings, FreeRDP_RemoteFxCodec) &&
      freerdp_settings_get_bool (rdp_settings, FreeRDP_NSCodec) &&
      multifrag_max_request_size < 0x3F0000)
    {
      g_message ("Disabling NSCodec since it does not support fragmentation");
      freerdp_settings_set_bool (rdp_settings, FreeRDP_NSCodec, FALSE);
    }

  if (!freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline) &&
      !freerdp_settings_get_bool (rdp_settings, FreeRDP_RemoteFxCodec))
    {
      g_warning ("[RDP] Client does neither support RFX nor GFX. This will "
                 "result in heavy performance and heavy bandwidth usage "
                 "regressions. The legacy path is deprecated!");
    }

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
      !freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline))
    {
      g_warning ("[RDP] Client does not support graphics pipeline. Disabling "
                 "audio output redirection");
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

  g_clear_object (&rdp_peer_context->rdp_dvc);

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
  rdpSettings *rdp_settings = peer->context->settings;

  rdp_peer_context->frame_id = 0;

  g_mutex_init (&rdp_peer_context->channel_mutex);

  rdp_peer_context->rfx_context = rfx_context_new (TRUE);
  if (!rdp_peer_context->rfx_context)
    {
      g_warning ("[RDP] Failed to create RFX context");
      return FALSE;
    }
  rfx_context_set_pixel_format (rdp_peer_context->rfx_context,
                                PIXEL_FORMAT_BGRX32);

  rdp_peer_context->encode_stream = Stream_New (NULL, 64 * 64 * 4);
  if (!rdp_peer_context->encode_stream)
    {
      g_warning ("[RDP] Failed to create encode stream");
      return FALSE;
    }

  rdp_peer_context->planar_flags = 0;
  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_DrawAllowSkipAlpha))
    rdp_peer_context->planar_flags |= PLANAR_FORMAT_HEADER_NA;
  rdp_peer_context->planar_flags |= PLANAR_FORMAT_HEADER_RLE;

  rdp_peer_context->vcm = WTSOpenServerA ((LPSTR) peer->context);
  if (!rdp_peer_context->vcm || rdp_peer_context->vcm == INVALID_HANDLE_VALUE)
    {
      g_warning ("[RDP] Failed to create virtual channel manager");
      return FALSE;
    }

  rdp_peer_context->rdp_dvc = grd_rdp_dvc_new (rdp_peer_context->vcm,
                                               &rdp_peer_context->rdp_context);

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
  freerdp_settings_set_bool (rdp_settings, FreeRDP_HasRelativeMouseEvent, FALSE);
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

  if (session_rdp->hwaccel_nvidia)
    grd_hwaccel_nvidia_push_cuda_context (session_rdp->hwaccel_nvidia);

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
          g_message ("Unable to check file descriptor, closing connection");
          handle_client_gone (session_rdp);
          break;
        }

      if (peer->connected &&
          WTSVirtualChannelManagerIsChannelJoined (vcm, DRDYNVC_SVC_CHANNEL_NAME))
        {
          GrdRdpTelemetry *telemetry;
          GrdRdpGraphicsPipeline *graphics_pipeline;
          GrdRdpAudioPlayback *audio_playback;
          GrdRdpDisplayControl *display_control;
          GrdRdpAudioInput *audio_input;

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
              audio_playback = rdp_peer_context->audio_playback;
              display_control = rdp_peer_context->display_control;
              audio_input = rdp_peer_context->audio_input;

              if (telemetry && !session_rdp->session_should_stop)
                grd_rdp_telemetry_maybe_init (telemetry);
              if (graphics_pipeline && !session_rdp->session_should_stop)
                grd_rdp_graphics_pipeline_maybe_init (graphics_pipeline);
              if (audio_playback && !session_rdp->session_should_stop)
                grd_rdp_audio_playback_maybe_init (audio_playback);
              if (display_control && !session_rdp->session_should_stop)
                grd_rdp_display_control_maybe_init (display_control);
              if (audio_input && !session_rdp->session_should_stop)
                grd_rdp_audio_input_maybe_init (audio_input);
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

  if (session_rdp->hwaccel_nvidia)
    grd_hwaccel_nvidia_pop_cuda_context (session_rdp->hwaccel_nvidia);

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
                                hwaccel_nvidia);

  if (!init_rdp_session (session_rdp, username, password, &error))
    {
      g_warning ("[RDP] Couldn't initialize session: %s", error->message);
      g_clear_object (&session_rdp->connection);
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
  g_clear_object (&rdp_peer_context->graphics_pipeline);
  g_clear_object (&rdp_peer_context->telemetry);
  g_mutex_unlock (&rdp_peer_context->channel_mutex);

  g_clear_pointer (&session_rdp->socket_thread, g_thread_join);
  g_clear_handle_id (&session_rdp->notify_post_connected_source_id,
                     g_source_remove);

  g_clear_object (&session_rdp->layout_manager);

  g_clear_object (&session_rdp->cursor_renderer);
  g_clear_object (&session_rdp->renderer);

  peer->Close (peer);
  g_clear_object (&session_rdp->connection);

  g_clear_object (&rdp_peer_context->network_autodetection);

  if (session_rdp->thread_pool)
    g_thread_pool_free (session_rdp->thread_pool, FALSE, TRUE);

  peer->Disconnect (peer);
  clear_rdp_peer (session_rdp);

  g_hash_table_foreach_remove (session_rdp->pressed_keys,
                               notify_keycode_released,
                               session_rdp);
  g_hash_table_foreach_remove (session_rdp->pressed_unicode_keys,
                               notify_keysym_released,
                               session_rdp);
  grd_rdp_event_queue_flush (session_rdp->rdp_event_queue);

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
maybe_initialize_graphics_pipeline (GrdSessionRdp *session_rdp)
{
  rdpContext *rdp_context = session_rdp->peer->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  rdpSettings *rdp_settings = rdp_context->settings;
  GrdRdpGraphicsPipeline *graphics_pipeline;
  GrdRdpTelemetry *telemetry;

  g_assert (!rdp_peer_context->telemetry);
  g_assert (!rdp_peer_context->graphics_pipeline);

  if (!freerdp_settings_get_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline))
    return;

  telemetry = grd_rdp_telemetry_new (session_rdp,
                                     rdp_peer_context->rdp_dvc,
                                     rdp_peer_context->vcm,
                                     rdp_context);
  rdp_peer_context->telemetry = telemetry;

  graphics_pipeline =
    grd_rdp_graphics_pipeline_new (session_rdp,
                                   session_rdp->renderer,
                                   rdp_peer_context->rdp_dvc,
                                   rdp_peer_context->vcm,
                                   rdp_context,
                                   rdp_peer_context->network_autodetection,
                                   rdp_peer_context->encode_stream,
                                   rdp_peer_context->rfx_context);
  grd_rdp_graphics_pipeline_set_hwaccel_nvidia (graphics_pipeline,
                                                session_rdp->hwaccel_nvidia);
  rdp_peer_context->graphics_pipeline = graphics_pipeline;
}

static void
initialize_remaining_virtual_channels (GrdSessionRdp *session_rdp)
{
  rdpContext *rdp_context = session_rdp->peer->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  rdpSettings *rdp_settings = rdp_context->settings;
  GrdRdpDvc *rdp_dvc = rdp_peer_context->rdp_dvc;
  HANDLE vcm = rdp_peer_context->vcm;

  if (session_rdp->screen_share_mode == GRD_RDP_SCREEN_SHARE_MODE_EXTEND)
    {
      rdp_peer_context->display_control =
        grd_rdp_display_control_new (session_rdp->layout_manager,
                                     session_rdp,
                                     rdp_peer_context->rdp_dvc,
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
        grd_rdp_audio_playback_new (session_rdp, rdp_dvc, vcm, rdp_context);
    }
  if (freerdp_settings_get_bool (rdp_settings, FreeRDP_AudioCapture))
    {
      rdp_peer_context->audio_input =
        grd_rdp_audio_input_new (session_rdp, rdp_dvc, vcm, rdp_context);
    }
}

static void
grd_session_rdp_remote_desktop_session_ready (GrdSession *session)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);

  grd_rdp_session_metrics_notify_phase_completion (session_rdp->session_metrics,
                                                   GRD_RDP_PHASE_SESSION_READY);

  session_rdp->cursor_renderer =
    grd_rdp_cursor_renderer_new (session_rdp->renderer,
                                 session_rdp->peer->context);
  grd_rdp_cursor_renderer_notify_session_ready (session_rdp->cursor_renderer);

  maybe_initialize_graphics_pipeline (session_rdp);
  initialize_remaining_virtual_channels (session_rdp);
}

static void
grd_session_rdp_remote_desktop_session_started (GrdSession *session)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);
  rdpContext *rdp_context = session_rdp->peer->context;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  GrdRdpGraphicsPipeline *graphics_pipeline = rdp_peer_context->graphics_pipeline;

  grd_rdp_session_metrics_notify_phase_completion (session_rdp->session_metrics,
                                                   GRD_RDP_PHASE_SESSION_STARTED);

  grd_rdp_renderer_notify_session_started (session_rdp->renderer,
                                           graphics_pipeline, rdp_context);
  grd_rdp_layout_manager_notify_session_started (session_rdp->layout_manager,
                                                 session_rdp->cursor_renderer,
                                                 rdp_context,
                                                 !!graphics_pipeline);
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
  g_mutex_clear (&session_rdp->pending_jobs_mutex);
  g_cond_clear (&session_rdp->pending_jobs_cond);

  G_OBJECT_CLASS (grd_session_rdp_parent_class)->finalize (object);
}

static void
grd_session_rdp_init (GrdSessionRdp *session_rdp)
{
  session_rdp->stop_event = CreateEvent (NULL, TRUE, FALSE, NULL);

  session_rdp->pressed_keys = g_hash_table_new (NULL, NULL);
  session_rdp->pressed_unicode_keys = g_hash_table_new (NULL, NULL);
  session_rdp->stream_table = g_hash_table_new (NULL, NULL);

  g_cond_init (&session_rdp->pending_jobs_cond);
  g_mutex_init (&session_rdp->pending_jobs_mutex);
  g_mutex_init (&session_rdp->rdp_flags_mutex);
  g_mutex_init (&session_rdp->close_session_mutex);
  g_mutex_init (&session_rdp->notify_post_connected_mutex);

  session_rdp->session_metrics = grd_rdp_session_metrics_new ();
  session_rdp->rdp_event_queue = grd_rdp_event_queue_new (session_rdp);
}

static void
grd_session_rdp_class_init (GrdSessionRdpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdSessionClass *session_class = GRD_SESSION_CLASS (klass);

  object_class->dispose = grd_session_rdp_dispose;
  object_class->finalize = grd_session_rdp_finalize;

  session_class->remote_desktop_session_ready =
    grd_session_rdp_remote_desktop_session_ready;
  session_class->remote_desktop_session_started =
    grd_session_rdp_remote_desktop_session_started;
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
