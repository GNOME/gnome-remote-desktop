/*
 * Copyright (C) 2020 Pascal Nowack
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

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <gio/gio.h>

#include "grd-context.h"
#include "grd-damage-utils.h"
#include "grd-rdp-sam.h"
#include "grd-rdp-server.h"
#include "grd-rdp-pipewire-stream.h"
#include "grd-settings.h"
#include "grd-stream.h"

typedef enum _RdpPeerFlag
{
  RDP_PEER_ACTIVATED      = 1 << 0,
  RDP_PEER_OUTPUT_ENABLED = 1 << 1,
} RdpPeerFlag;

struct _GrdSessionRdp
{
  GrdSession parent;

  GSocketConnection *connection;
  GSource *source;
  freerdp_peer *peer;

  uint8_t *last_frame;

  unsigned int close_session_idle_id;

  GrdRdpPipeWireStream *pipewire_stream;
};

typedef struct _RdpPeerContext
{
  rdpContext rdp_context;

  GrdSessionRdp *session_rdp;
  GrdRdpSAMFile *sam_file;

  RdpPeerFlag flags;
  uint32_t frame_id;

  RFX_CONTEXT *rfx_context;
  wStream *encode_stream;
} RdpPeerContext;

typedef struct _NSCThreadPoolContext
{
  uint32_t src_stride;
  uint8_t *src_data;
  rdpSettings *rdp_settings;
} NSCThreadPoolContext;

typedef struct _NSCEncodeContext
{
  cairo_rectangle_int_t cairo_rect;
  wStream *stream;
} NSCEncodeContext;

G_DEFINE_TYPE (GrdSessionRdp, grd_session_rdp, GRD_TYPE_SESSION);

static void
grd_session_rdp_detach_source (GrdSessionRdp *session_rdp);

static gboolean
close_session_idle (gpointer user_data);

static void
rdp_peer_refresh_region (freerdp_peer   *peer,
                         cairo_region_t *region,
                         uint8_t        *data);

void
grd_session_rdp_resize_framebuffer (GrdSessionRdp *session_rdp,
                                    uint32_t       width,
                                    uint32_t       height)
{
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  rdpSettings *rdp_settings = peer->settings;

  if (rdp_settings->DesktopWidth == width &&
      rdp_settings->DesktopHeight == height)
    return;

  rdp_settings->DesktopWidth = width;
  rdp_settings->DesktopHeight = height;
  peer->update->DesktopResize (peer->context);

  g_clear_pointer (&session_rdp->last_frame, g_free);

  rfx_context_reset (rdp_peer_context->rfx_context,
                     rdp_settings->DesktopWidth,
                     rdp_settings->DesktopHeight);
}

void
grd_session_rdp_take_buffer (GrdSessionRdp *session_rdp,
                             void          *data)
{
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  rdpSettings *rdp_settings = peer->settings;
  uint32_t stride = grd_session_rdp_get_framebuffer_stride (session_rdp);
  cairo_region_t *region;

  if (rdp_peer_context->flags & RDP_PEER_ACTIVATED &&
      rdp_peer_context->flags & RDP_PEER_OUTPUT_ENABLED)
    {
      region = grd_get_damage_region ((uint8_t *) data,
                                      session_rdp->last_frame,
                                      rdp_settings->DesktopWidth,
                                      rdp_settings->DesktopHeight,
                                      64, 64,
                                      stride, 4);
      if (!cairo_region_is_empty (region))
        rdp_peer_refresh_region (peer, region, (uint8_t *) data);

      g_clear_pointer (&session_rdp->last_frame, g_free);
      session_rdp->last_frame = g_steal_pointer (&data);

      cairo_region_destroy (region);
    }

  g_free (data);
}

static void
maybe_queue_close_session_idle (GrdSessionRdp *session_rdp)
{
  if (session_rdp->close_session_idle_id)
    return;

  session_rdp->close_session_idle_id =
    g_idle_add (close_session_idle, session_rdp);
}

static void
handle_client_gone (GrdSessionRdp *session_rdp)
{
  g_debug ("RDP client gone");

  grd_session_rdp_detach_source (session_rdp);
  maybe_queue_close_session_idle (session_rdp);
}

static gboolean
is_view_only (GrdSessionRdp *session_rdp)
{
  return TRUE;
}

static void
rdp_peer_refresh_rfx (freerdp_peer   *peer,
                      cairo_region_t *region,
                      uint8_t        *data)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  rdpSettings *rdp_settings = peer->settings;
  rdpUpdate *rdp_update = peer->update;
  uint32_t desktop_width = rdp_settings->DesktopWidth;
  uint32_t desktop_height = rdp_settings->DesktopHeight;
  uint32_t src_stride = grd_session_rdp_get_framebuffer_stride (session_rdp);
  SURFACE_BITS_COMMAND cmd = {0};
  cairo_rectangle_int_t cairo_rect;
  RFX_RECT *rfx_rects, *rfx_rect;
  int n_rects;
  RFX_MESSAGE *rfx_messages;
  int n_messages;
  BOOL first, last;
  int i;

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
                                      desktop_width,
                                      desktop_height,
                                      src_stride,
                                      &n_messages,
                                      rdp_settings->MultifragMaxRequestSize);

  cmd.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
  cmd.bmp.codecID = rdp_settings->RemoteFxCodecId;
  cmd.destLeft = 0;
  cmd.destTop = 0;
  cmd.destRight = cmd.destLeft + desktop_width;
  cmd.destBottom = cmd.destTop + desktop_height;
  cmd.bmp.bpp = 32;
  cmd.bmp.flags = 0;
  cmd.bmp.width = desktop_width;
  cmd.bmp.height = desktop_height;

  for (i = 0; i < n_messages; ++i)
    {
      Stream_SetPosition (rdp_peer_context->encode_stream, 0);

      if (!rfx_write_message (rdp_peer_context->rfx_context,
                              rdp_peer_context->encode_stream,
                              &rfx_messages[i]))
        {
          g_warning ("rfx_write_message failed");

          for (; i < n_messages; ++i)
            rfx_message_free (rdp_peer_context->rfx_context, &rfx_messages[i]);

          break;
        }

      rfx_message_free (rdp_peer_context->rfx_context, &rfx_messages[i]);
      cmd.bmp.bitmapDataLength = Stream_GetPosition (rdp_peer_context->encode_stream);
      cmd.bmp.bitmapData = Stream_Buffer (rdp_peer_context->encode_stream);
      first = i == 0 ? TRUE : FALSE;
      last = i + 1 == n_messages ? TRUE : FALSE;

      if (!rdp_settings->SurfaceFrameMarkerEnabled)
        rdp_update->SurfaceBits (rdp_update->context, &cmd);
      else
        rdp_update->SurfaceFrameBits (rdp_update->context, &cmd, first, last,
                                      rdp_peer_context->frame_id);
    }

  g_free (rfx_messages);
  g_free (rfx_rects);
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
  NSC_CONTEXT *nsc_context;
  uint8_t *src_data_at_pos;

  nsc_context = nsc_context_new ();
  nsc_context_set_parameters (nsc_context,
                              NSC_COLOR_LOSS_LEVEL,
                              rdp_settings->NSCodecColorLossLevel);
  nsc_context_set_parameters (nsc_context,
                              NSC_ALLOW_SUBSAMPLING,
                              rdp_settings->NSCodecAllowSubsampling);
  nsc_context_set_parameters (nsc_context,
                              NSC_DYNAMIC_COLOR_FIDELITY,
                              rdp_settings->NSCodecAllowDynamicColorFidelity);
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
}

static void
rdp_peer_refresh_nsc (freerdp_peer   *peer,
                      cairo_region_t *region,
                      uint8_t        *data)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  rdpSettings *rdp_settings = peer->settings;
  rdpUpdate *rdp_update = peer->update;
  uint32_t src_stride = grd_session_rdp_get_framebuffer_stride (session_rdp);
  cairo_rectangle_int_t *cairo_rect;
  int n_rects;
  GThreadPool *thread_pool;
  NSCThreadPoolContext thread_pool_context;
  NSCEncodeContext *encode_contexts;
  NSCEncodeContext *encode_context;
  SURFACE_BITS_COMMAND cmd = {0};
  BOOL first, last;
  int i;

  n_rects = cairo_region_num_rectangles (region);
  if (!(encode_contexts = g_malloc0 (n_rects * sizeof (NSCEncodeContext))))
    return;

  thread_pool_context.src_stride = src_stride;
  thread_pool_context.src_data = data;
  thread_pool_context.rdp_settings = rdp_settings;

  thread_pool = g_thread_pool_new (rdp_peer_encode_nsc_rect,
                                   &thread_pool_context,
                                   g_get_num_processors (),
                                   TRUE,
                                   NULL);

  for (i = 0; i < n_rects; ++i)
    {
      encode_context = &encode_contexts[i];
      cairo_region_get_rectangle (region, i, &encode_context->cairo_rect);

      g_thread_pool_push (thread_pool, encode_context, NULL);
    }

  g_thread_pool_free (thread_pool, FALSE, TRUE);

  cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
  cmd.bmp.bpp = 32;
  cmd.bmp.codecID = rdp_settings->NSCodecId;

  for (i = 0; i < n_rects; ++i)
    {
      encode_context = &encode_contexts[i];
      cairo_rect = &encode_context->cairo_rect;

      cmd.destLeft = cairo_rect->x;
      cmd.destTop = cairo_rect->y;
      cmd.destRight = cmd.destLeft + cairo_rect->width;
      cmd.destBottom = cmd.destTop + cairo_rect->height;
      cmd.bmp.width = cairo_rect->width;
      cmd.bmp.height = cairo_rect->height;
      cmd.bmp.bitmapDataLength = Stream_GetPosition (encode_context->stream);
      cmd.bmp.bitmapData = Stream_Buffer (encode_context->stream);
      first = i == 0 ? TRUE : FALSE;
      last = i + 1 == n_rects ? TRUE : FALSE;

      if (!rdp_settings->SurfaceFrameMarkerEnabled)
        rdp_update->SurfaceBits (rdp_update->context, &cmd);
      else
        rdp_update->SurfaceFrameBits (rdp_update->context, &cmd, first, last,
                                      rdp_peer_context->frame_id);

      Stream_Free (encode_context->stream, TRUE);
    }

  g_free (encode_contexts);
}

static void
rdp_peer_refresh_region (freerdp_peer   *peer,
                         cairo_region_t *region,
                         uint8_t        *data)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  rdpSettings *rdp_settings = peer->settings;

  if (rdp_settings->RemoteFxCodec)
    {
      rdp_peer_refresh_rfx (peer, region, data);
    }
  else if (rdp_settings->NSCodec)
    {
      rdp_peer_refresh_nsc (peer, region, data);
    }
  else
    {
      g_warning ("Neither NSCodec or RemoteFxCodec are available, closing connection");
      rdp_peer_context->flags &= ~RDP_PEER_ACTIVATED;
      handle_client_gone (rdp_peer_context->session_rdp);
    }

  ++rdp_peer_context->frame_id;
}

static BOOL
rdp_input_synchronize_event (rdpInput *rdp_input,
                             uint32_t  flags)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;

  if (!(rdp_peer_context->flags & RDP_PEER_ACTIVATED))
    return TRUE;

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

  if (!(rdp_peer_context->flags & RDP_PEER_ACTIVATED) ||
      is_view_only (session_rdp))
    return TRUE;

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

  if (!(rdp_peer_context->flags & RDP_PEER_ACTIVATED) ||
      is_view_only (session_rdp))
    return TRUE;

  return TRUE;
}

static BOOL
rdp_input_keyboard_event (rdpInput *rdp_input,
                          uint16_t  flags,
                          uint16_t  code)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;

  if (!(rdp_peer_context->flags & RDP_PEER_ACTIVATED) ||
      is_view_only (session_rdp))
    return TRUE;

  return TRUE;
}

static BOOL
rdp_input_unicode_keyboard_event (rdpInput *rdp_input,
                                  uint16_t  flags,
                                  uint16_t  code)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;

  if (!(rdp_peer_context->flags & RDP_PEER_ACTIVATED) ||
      is_view_only (session_rdp))
    return TRUE;

  return TRUE;
}

static BOOL
rdp_suppress_output (rdpContext         *rdp_context,
                     uint8_t             allow,
                     const RECTANGLE_16 *area)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;

  if (allow)
    rdp_peer_context->flags |= RDP_PEER_OUTPUT_ENABLED;
  else
    rdp_peer_context->flags &= ~RDP_PEER_OUTPUT_ENABLED;

  return TRUE;
}

static BOOL
rdp_peer_capabilities (freerdp_peer *peer)
{
  rdpSettings *rdp_settings = peer->settings;

  switch (rdp_settings->ColorDepth)
    {
    case 32:
      break;
    case 24:
    case 16:
    case 15:
      /* We currently don't support sending raw bitmaps */
      g_warning ("Sending raw bitmaps ist not supported, closing connection");
      return FALSE;
    default:
      g_warning ("Invalid colour depth, closing connection");
      return FALSE;
    }

  if (!rdp_settings->DesktopResize)
    {
      g_warning ("Client doesn't support resizing, closing connection");
      return FALSE;
    }

  return TRUE;
}

static BOOL
rdp_peer_post_connect (freerdp_peer *peer)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  rdpSettings *rdp_settings = peer->settings;
  GrdRdpSAMFile *sam_file;

  g_debug ("New RDP client");

  if (rdp_settings->MultifragMaxRequestSize < 0x3F0000)
    {
      g_message ("Disabling NSCodec since it does not support fragmentation");
      rdp_settings->NSCodec = FALSE;
    }

  grd_session_start (GRD_SESSION (session_rdp));
  grd_session_rdp_detach_source (session_rdp);

  sam_file = g_steal_pointer (&rdp_peer_context->sam_file);
  grd_rdp_sam_maybe_close_and_free_sam_file (sam_file);

  rdp_peer_context->flags |= RDP_PEER_ACTIVATED;

  return TRUE;
}

static BOOL
rdp_peer_activate (freerdp_peer *peer)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  rdpSettings *rdp_settings = peer->settings;

  rfx_context_reset (rdp_peer_context->rfx_context,
                     rdp_settings->DesktopWidth,
                     rdp_settings->DesktopHeight);

  return TRUE;
}

static BOOL
rdp_peer_context_new (freerdp_peer   *peer,
                      RdpPeerContext *rdp_peer_context)
{
  rdp_peer_context->frame_id = 0;

  rdp_peer_context->rfx_context = rfx_context_new (TRUE);
  rdp_peer_context->rfx_context->mode = RLGR3;
  rfx_context_set_pixel_format (rdp_peer_context->rfx_context,
                                PIXEL_FORMAT_BGRX32);

  rdp_peer_context->encode_stream = Stream_New (NULL, 64 * 64 * 4);

  rdp_peer_context->flags = RDP_PEER_OUTPUT_ENABLED;

  return TRUE;
}

static void
rdp_peer_context_free (freerdp_peer   *peer,
                       RdpPeerContext *rdp_peer_context)
{
  if (!rdp_peer_context)
    return;

  if (rdp_peer_context->encode_stream)
    Stream_Free (rdp_peer_context->encode_stream, TRUE);

  if (rdp_peer_context->rfx_context)
    rfx_context_free (rdp_peer_context->rfx_context);
}

int
grd_session_rdp_get_framebuffer_stride (GrdSessionRdp *session_rdp)
{
  rdpSettings *rdp_settings = session_rdp->peer->settings;

  return rdp_settings->DesktopWidth * 4;
}

static void
init_rdp_session (GrdSessionRdp *session_rdp,
                  const char    *username,
                  const char    *password)
{
  GrdContext *context = grd_session_get_context (GRD_SESSION (session_rdp));
  GrdSettings *settings = grd_context_get_settings (context);
  GSocket *socket = g_socket_connection_get_socket (session_rdp->connection);
  freerdp_peer *peer;
  rdpInput *rdp_input;
  RdpPeerContext *rdp_peer_context;
  rdpSettings *rdp_settings;

  g_debug ("Initialize RDP session");

  peer = freerdp_peer_new (g_socket_get_fd (socket));

  peer->ContextSize = sizeof (RdpPeerContext);
  peer->ContextNew = (psPeerContextNew) rdp_peer_context_new;
  peer->ContextFree = (psPeerContextFree) rdp_peer_context_free;
  freerdp_peer_context_new (peer);

  rdp_peer_context = (RdpPeerContext *) peer->context;
  rdp_peer_context->session_rdp = session_rdp;

  rdp_peer_context->sam_file = grd_rdp_sam_create_sam_file (username, password);

  rdp_settings = peer->settings;
  freerdp_settings_set_string (rdp_settings,
                               FreeRDP_NtlmSamFile,
                               rdp_peer_context->sam_file->filename);
  rdp_settings->CertificateFile = strdup (grd_settings_get_rdp_server_cert (settings));
  rdp_settings->PrivateKeyFile = strdup (grd_settings_get_rdp_server_key (settings));
  rdp_settings->RdpSecurity = FALSE;
  rdp_settings->TlsSecurity = FALSE;
  rdp_settings->NlaSecurity = TRUE;

  rdp_settings->OsMajorType = OSMAJORTYPE_UNIX;
  rdp_settings->OsMajorType = OSMINORTYPE_PSEUDO_XSERVER;
  rdp_settings->ColorDepth = 32;
  rdp_settings->RefreshRect = TRUE;
  rdp_settings->RemoteFxCodec = TRUE;
  rdp_settings->NSCodec = TRUE;
  rdp_settings->FrameMarkerCommandEnabled = TRUE;
  rdp_settings->SurfaceFrameMarkerEnabled = TRUE;

  peer->Capabilities = rdp_peer_capabilities;
  peer->PostConnect = rdp_peer_post_connect;
  peer->Activate = rdp_peer_activate;

  peer->update->SuppressOutput = rdp_suppress_output;

  rdp_input = peer->input;
  rdp_input->SynchronizeEvent = rdp_input_synchronize_event;
  rdp_input->MouseEvent = rdp_input_mouse_event;
  rdp_input->ExtendedMouseEvent = rdp_input_extended_mouse_event;
  rdp_input->KeyboardEvent = rdp_input_keyboard_event;
  rdp_input->UnicodeKeyboardEvent = rdp_input_unicode_keyboard_event;

  peer->Initialize (peer);

  session_rdp->peer = peer;
  session_rdp->last_frame = NULL;
}

static gboolean
handle_socket_data (GSocket      *socket,
                    GIOCondition  condition,
                    gpointer      user_data)
{
  GrdSessionRdp *session_rdp = user_data;
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;

  if (condition & G_IO_IN)
    {
      if (!peer->CheckFileDescriptor (peer))
        {
          g_message ("Unable to check file descriptor, closing connection");
          rdp_peer_context->flags &= ~RDP_PEER_ACTIVATED;
          handle_client_gone (session_rdp);

          return G_SOURCE_REMOVE;
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
grd_session_rdp_attach_source (GrdSessionRdp *session_rdp)
{
  GSocket *socket;

  socket = g_socket_connection_get_socket (session_rdp->connection);
  session_rdp->source = g_socket_create_source (socket,
                                                G_IO_IN | G_IO_PRI,
                                                NULL);
  g_source_set_callback (session_rdp->source,
                         (GSourceFunc) handle_socket_data,
                         session_rdp, NULL);
  g_source_attach (session_rdp->source, NULL);
}

static void
grd_session_rdp_detach_source (GrdSessionRdp *session_rdp)
{
  g_clear_pointer (&session_rdp->source, g_source_destroy);
}

GrdSessionRdp *
grd_session_rdp_new (GrdRdpServer      *rdp_server,
                     GSocketConnection *connection)
{
  GrdSessionRdp *session_rdp;
  GrdContext *context;
  GrdSettings *settings;
  char *username;
  char *password;
  g_autoptr (GError) error = NULL;

  context = grd_rdp_server_get_context (rdp_server);
  settings = grd_context_get_settings (context);
  username = grd_settings_get_rdp_username (settings, &error);
  if (!username)
    {
      g_warning ("Couldn't retrieve RDP username: %s", error->message);
      return NULL;
    }
  password = grd_settings_get_rdp_password (settings, &error);
  if (!password)
    {
      g_warning ("Couldn't retrieve RDP password: %s", error->message);
      g_free (username);
      return NULL;
    }

  session_rdp = g_object_new (GRD_TYPE_SESSION_RDP,
                              "context", context,
                              NULL);

  session_rdp->connection = g_object_ref (connection);

  grd_session_rdp_attach_source (session_rdp);

  init_rdp_session (session_rdp, username, password);

  g_free (password);
  g_free (username);

  return session_rdp;
}

static void
grd_session_rdp_stop (GrdSession *session)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;

  g_debug ("Stopping RDP session");

  g_clear_object (&session_rdp->pipewire_stream);

  grd_session_rdp_detach_source (session_rdp);

  g_clear_object (&session_rdp->connection);

  if (rdp_peer_context->sam_file)
    grd_rdp_sam_maybe_close_and_free_sam_file (rdp_peer_context->sam_file);

  peer->Disconnect (peer);
  freerdp_peer_context_free (peer);
  freerdp_peer_free (peer);

  g_clear_pointer (&session_rdp->last_frame, g_free);

  if (session_rdp->close_session_idle_id)
    {
      g_source_remove (session_rdp->close_session_idle_id);
      session_rdp->close_session_idle_id = 0;
    }
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
on_pipewire_stream_closed (GrdRdpPipeWireStream *stream,
                           GrdSessionRdp        *session_rdp)
{
  g_warning ("PipeWire stream closed, closing client");

  maybe_queue_close_session_idle (session_rdp);
}

static void
grd_session_rdp_stream_ready (GrdSession *session,
                              GrdStream  *stream)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);
  uint32_t pipewire_node_id;
  g_autoptr (GError) error = NULL;

  pipewire_node_id = grd_stream_get_pipewire_node_id (stream);
  session_rdp->pipewire_stream = grd_rdp_pipewire_stream_new (session_rdp,
                                                              pipewire_node_id,
                                                              &error);
  if (!session_rdp->pipewire_stream)
    {
      g_warning ("Failed to establish PipeWire stream: %s", error->message);
      return;
    }

  g_signal_connect (session_rdp->pipewire_stream, "closed",
                    G_CALLBACK (on_pipewire_stream_closed),
                    session_rdp);

  if (!session_rdp->source)
    grd_session_rdp_attach_source (session_rdp);
}

static void
grd_session_rdp_init (GrdSessionRdp *session_rdp)
{
}

static void
grd_session_rdp_class_init (GrdSessionRdpClass *klass)
{
  GrdSessionClass *session_class = GRD_SESSION_CLASS (klass);

  session_class->stop = grd_session_rdp_stop;
  session_class->stream_ready = grd_session_rdp_stream_ready;
}
