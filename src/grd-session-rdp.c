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

#include <freerdp/channels/wtsvc.h>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#ifdef HAS_RDP_UNICODE_INPUT
#include <xkbcommon/xkbcommon.h>
#endif

#include "grd-clipboard-rdp.h"
#include "grd-context.h"
#include "grd-damage-utils.h"
#include "grd-rdp-event-queue.h"
#include "grd-rdp-pipewire-stream.h"
#include "grd-rdp-sam.h"
#include "grd-rdp-server.h"
#include "grd-settings.h"
#include "grd-stream.h"

#define DISCRETE_SCROLL_STEP 10.0

typedef enum _RdpPeerFlag
{
  RDP_PEER_ACTIVATED      = 1 << 0,
  RDP_PEER_OUTPUT_ENABLED = 1 << 1,
} RdpPeerFlag;

typedef enum _PointerType
{
  POINTER_TYPE_DEFAULT = 0,
  POINTER_TYPE_HIDDEN  = 1 << 0,
  POINTER_TYPE_NORMAL  = 1 << 1,
} PointerType;

typedef struct _Pointer
{
  uint8_t *bitmap;
  uint16_t hotspot_x;
  uint16_t hotspot_y;
  uint16_t width;
  uint16_t height;

  uint16_t cache_index;
  int64_t last_used;
} Pointer;

struct _GrdSessionRdp
{
  GrdSession parent;

  GSocketConnection *connection;
  freerdp_peer *peer;

  GThread *socket_thread;
  HANDLE start_event;
  HANDLE stop_event;

  uint8_t *last_frame;
  Pointer *last_pointer;
  GHashTable *pointer_cache;
  PointerType pointer_type;

  GMutex pointer_mutex;
  uint16_t pointer_x;
  uint16_t pointer_y;

#ifdef HAS_RDP_UNICODE_INPUT
  GHashTable *pressed_unicode_keys;
#endif

  GrdRdpEventQueue *rdp_event_queue;

  GThreadPool *thread_pool;
  GCond pending_jobs_cond;
  GMutex pending_jobs_mutex;

  unsigned int close_session_idle_id;

  GrdRdpPipeWireStream *pipewire_stream;
};

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

typedef struct _RdpPeerContext
{
  rdpContext rdp_context;

  GrdSessionRdp *session_rdp;
  GrdRdpSAMFile *sam_file;

  GMutex flags_mutex;
  RdpPeerFlag flags;
  uint32_t frame_id;

  RFX_CONTEXT *rfx_context;
  wStream *encode_stream;

  NSCThreadPoolContext nsc_thread_pool_context;
  RawThreadPoolContext raw_thread_pool_context;

  uint16_t planar_flags;

  /* Virtual Channel Manager */
  HANDLE vcm;

  GrdClipboardRdp *clipboard_rdp;
} RdpPeerContext;

G_DEFINE_TYPE (GrdSessionRdp, grd_session_rdp, GRD_TYPE_SESSION);

static gboolean
close_session_idle (gpointer user_data);

static void
rdp_peer_refresh_region (freerdp_peer   *peer,
                         cairo_region_t *region,
                         uint8_t        *data);

static gboolean
are_pointer_bitmaps_equal (gconstpointer a,
                           gconstpointer b);

static gboolean
is_rdp_peer_flag_set (RdpPeerContext *rdp_peer_context,
                      RdpPeerFlag     flag)
{
  gboolean state;

  g_mutex_lock (&rdp_peer_context->flags_mutex);
  state = rdp_peer_context->flags & flag;
  g_mutex_unlock (&rdp_peer_context->flags_mutex);

  return state;
}

static void
set_rdp_peer_flag (RdpPeerContext *rdp_peer_context,
                   RdpPeerFlag     flag)
{
  g_mutex_lock (&rdp_peer_context->flags_mutex);
  rdp_peer_context->flags |= flag;
  g_mutex_unlock (&rdp_peer_context->flags_mutex);
}

static void
unset_rdp_peer_flag (RdpPeerContext *rdp_peer_context,
                     RdpPeerFlag     flag)
{
  g_mutex_lock (&rdp_peer_context->flags_mutex);
  rdp_peer_context->flags &= ~flag;
  g_mutex_unlock (&rdp_peer_context->flags_mutex);
}

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

static int
grd_session_rdp_get_framebuffer_stride (GrdSessionRdp *session_rdp)
{
  rdpSettings *rdp_settings = session_rdp->peer->settings;

  return grd_session_rdp_get_stride_for_width (session_rdp,
                                               rdp_settings->DesktopWidth);
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

  if (is_rdp_peer_flag_set (rdp_peer_context, RDP_PEER_ACTIVATED) &&
      is_rdp_peer_flag_set (rdp_peer_context, RDP_PEER_OUTPUT_ENABLED))
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

static gboolean
is_mouse_pointer_hidden (uint32_t  width,
                         uint32_t  height,
                         uint8_t  *data)
{
  uint8_t *src_data;
  uint32_t i;

  for (i = 0, src_data = data; i < height * width; ++i, ++src_data)
    {
      src_data += 3;
      if (*src_data)
        return FALSE;
    }

  return TRUE;
}

static gboolean
find_equal_pointer_bitmap (gpointer key,
                           gpointer value,
                           gpointer user_data)
{
  return are_pointer_bitmaps_equal (value, user_data);
}

void
grd_session_rdp_update_pointer (GrdSessionRdp *session_rdp,
                                uint16_t       hotspot_x,
                                uint16_t       hotspot_y,
                                uint16_t       width,
                                uint16_t       height,
                                uint8_t       *data)
{
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  rdpSettings *rdp_settings = peer->settings;
  rdpUpdate *rdp_update = peer->update;
  POINTER_SYSTEM_UPDATE pointer_system = {0};
  POINTER_NEW_UPDATE pointer_new = {0};
  POINTER_LARGE_UPDATE pointer_large = {0};
  POINTER_CACHED_UPDATE pointer_cached = {0};
  POINTER_COLOR_UPDATE *pointer_color;
  cairo_rectangle_int_t cairo_rect;
  Pointer *new_pointer;
  void *key, *value;
  uint32_t stride;
  uint32_t xor_mask_length;
  uint8_t *xor_mask;
  uint8_t *src_data, *dst_data;
  uint8_t r, g, b, a;
  uint32_t x, y;

  if (!is_rdp_peer_flag_set (rdp_peer_context, RDP_PEER_ACTIVATED))
    {
      g_free (data);
      return;
    }

  if (is_mouse_pointer_hidden (width, height, data))
    {
      if (session_rdp->pointer_type != POINTER_TYPE_HIDDEN)
        {
          session_rdp->last_pointer = NULL;
          session_rdp->pointer_type = POINTER_TYPE_HIDDEN;
          pointer_system.type = SYSPTR_NULL;

          rdp_update->pointer->PointerSystem (peer->context, &pointer_system);
        }

      g_free (data);
      return;
    }

  /* RDP only handles pointer bitmaps up to 384x384 pixels */
  if (width > 384 || height > 384)
    {
      if (session_rdp->pointer_type != POINTER_TYPE_DEFAULT)
        {
          session_rdp->last_pointer = NULL;
          session_rdp->pointer_type = POINTER_TYPE_DEFAULT;
          pointer_system.type = SYSPTR_DEFAULT;

          rdp_update->pointer->PointerSystem (peer->context, &pointer_system);
        }

      g_free (data);
      return;
    }

  session_rdp->pointer_type = POINTER_TYPE_NORMAL;

  stride = width * 4;
  if (session_rdp->last_pointer &&
      session_rdp->last_pointer->hotspot_x == hotspot_x &&
      session_rdp->last_pointer->hotspot_y == hotspot_y &&
      session_rdp->last_pointer->width == width &&
      session_rdp->last_pointer->height == height)
    {
      cairo_rect.x = cairo_rect.y = 0;
      cairo_rect.width = width;
      cairo_rect.height = height;

      if (!grd_is_tile_dirty (&cairo_rect,
                              data,
                              session_rdp->last_pointer->bitmap,
                              stride, 4))
        {
          session_rdp->last_pointer->last_used = g_get_monotonic_time ();
          g_free (data);
          return;
        }
    }

  new_pointer = g_malloc0 (sizeof (Pointer));
  new_pointer->bitmap = data;
  new_pointer->hotspot_x = hotspot_x;
  new_pointer->hotspot_y = hotspot_y;
  new_pointer->width = width;
  new_pointer->height = height;
  if ((value = g_hash_table_find (session_rdp->pointer_cache,
                                  find_equal_pointer_bitmap,
                                  new_pointer)))
    {
      session_rdp->last_pointer = (Pointer *) value;
      session_rdp->last_pointer->last_used = g_get_monotonic_time ();
      pointer_cached.cacheIndex = session_rdp->last_pointer->cache_index;

      rdp_update->pointer->PointerCached (peer->context, &pointer_cached);

      g_free (new_pointer);
      g_free (data);

      return;
    }

  xor_mask_length = height * stride * sizeof (uint8_t);
  xor_mask = g_malloc0 (xor_mask_length);

  for (y = 0; y < height; ++y)
    {
      src_data = &data[stride * (height - 1 - y)];
      dst_data = &xor_mask[stride * y];

      for (x = 0; x < width; ++x)
        {
          r = *src_data++;
          g = *src_data++;
          b = *src_data++;
          a = *src_data++;

          *dst_data++ = b;
          *dst_data++ = g;
          *dst_data++ = r;
          *dst_data++ = a;
        }
    }

  new_pointer->cache_index = g_hash_table_size (session_rdp->pointer_cache);
  if (g_hash_table_size (session_rdp->pointer_cache) >= rdp_settings->PointerCacheSize)
    {
      /* Least recently used pointer */
      Pointer *lru_pointer = NULL;
      GHashTableIter iter;

      g_hash_table_iter_init (&iter, session_rdp->pointer_cache);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          if (!lru_pointer || lru_pointer->last_used > ((Pointer *) key)->last_used)
            lru_pointer = (Pointer *) key;
        }

      g_hash_table_steal (session_rdp->pointer_cache, lru_pointer);
      new_pointer->cache_index = lru_pointer->cache_index;

      g_free (lru_pointer->bitmap);
      g_free (lru_pointer);
    }

  new_pointer->last_used = g_get_monotonic_time ();

  if (width <= 96 && height <= 96)
    {
      pointer_new.xorBpp = 32;
      pointer_color = &pointer_new.colorPtrAttr;
      pointer_color->cacheIndex = new_pointer->cache_index;
      /* xPos and yPos actually represent the hotspot coordinates of the pointer
       * instead of the actual pointer position.
       * FreeRDP just uses a confusing naming convention here.
       * See also 2.2.9.1.1.4.4 Color Pointer Update (TS_COLORPOINTERATTRIBUTE)
       * for reference
       */
      pointer_color->xPos = hotspot_x;
      pointer_color->yPos = hotspot_y;
      pointer_color->width = width;
      pointer_color->height = height;
      pointer_color->lengthAndMask = 0;
      pointer_color->lengthXorMask = xor_mask_length;
      pointer_color->andMaskData = NULL;
      pointer_color->xorMaskData = xor_mask;
      pointer_cached.cacheIndex = pointer_color->cacheIndex;

      rdp_update->pointer->PointerNew (peer->context, &pointer_new);
    }
  else
    {
      pointer_large.xorBpp = 32;
      pointer_large.cacheIndex = new_pointer->cache_index;
      pointer_large.hotSpotX = hotspot_x;
      pointer_large.hotSpotY = hotspot_y;
      pointer_large.width = width;
      pointer_large.height = height;
      pointer_large.lengthAndMask = 0;
      pointer_large.lengthXorMask = xor_mask_length;
      pointer_large.andMaskData = NULL;
      pointer_large.xorMaskData = xor_mask;
      pointer_cached.cacheIndex = pointer_large.cacheIndex;

      rdp_update->pointer->PointerLarge (peer->context, &pointer_large);
    }

  rdp_update->pointer->PointerCached (peer->context, &pointer_cached);

  g_hash_table_add (session_rdp->pointer_cache, new_pointer);
  session_rdp->last_pointer = new_pointer;

  g_free (xor_mask);
}

void
grd_session_rdp_hide_pointer (GrdSessionRdp *session_rdp)
{
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  rdpUpdate *rdp_update = peer->update;
  POINTER_SYSTEM_UPDATE pointer_system = {0};

  if (!is_rdp_peer_flag_set (rdp_peer_context, RDP_PEER_ACTIVATED))
    return;

  if (session_rdp->pointer_type == POINTER_TYPE_HIDDEN)
    return;

  session_rdp->last_pointer = NULL;
  session_rdp->pointer_type = POINTER_TYPE_HIDDEN;
  pointer_system.type = SYSPTR_NULL;

  rdp_update->pointer->PointerSystem (peer->context, &pointer_system);
}

void
grd_session_rdp_move_pointer (GrdSessionRdp *session_rdp,
                              uint16_t       x,
                              uint16_t       y)
{
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  rdpUpdate *rdp_update = peer->update;
  POINTER_POSITION_UPDATE pointer_position = {0};

  if (!is_rdp_peer_flag_set (rdp_peer_context, RDP_PEER_ACTIVATED))
    return;

  g_mutex_lock (&session_rdp->pointer_mutex);
  if (session_rdp->pointer_x == x && session_rdp->pointer_y == y)
    {
      g_mutex_unlock (&session_rdp->pointer_mutex);
      return;
    }

  pointer_position.xPos = session_rdp->pointer_x = x;
  pointer_position.yPos = session_rdp->pointer_y = y;
  g_mutex_unlock (&session_rdp->pointer_mutex);

  rdp_update->pointer->PointerPosition (peer->context, &pointer_position);
}

static void
maybe_queue_close_session_idle (GrdSessionRdp *session_rdp)
{
  if (session_rdp->close_session_idle_id)
    return;

  session_rdp->close_session_idle_id =
    g_idle_add (close_session_idle, session_rdp);

  SetEvent (session_rdp->stop_event);
}

static void
handle_client_gone (GrdSessionRdp *session_rdp)
{
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;

  g_debug ("RDP client gone");

  unset_rdp_peer_flag (rdp_peer_context, RDP_PEER_ACTIVATED);
  maybe_queue_close_session_idle (session_rdp);
}

static gboolean
is_view_only (GrdSessionRdp *session_rdp)
{
  GrdContext *context = grd_session_get_context (GRD_SESSION (session_rdp));
  GrdSettings *settings = grd_context_get_settings (context);

  return grd_settings_get_rdp_view_only (settings);
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

  g_mutex_lock (thread_pool_context->pending_jobs_mutex);
  --thread_pool_context->pending_job_count;
  g_cond_signal (thread_pool_context->pending_jobs_cond);
  g_mutex_unlock (thread_pool_context->pending_jobs_mutex);
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
  NSCThreadPoolContext *thread_pool_context =
    &rdp_peer_context->nsc_thread_pool_context;
  g_autoptr (GError) error = NULL;
  cairo_rectangle_int_t *cairo_rect;
  int n_rects;
  NSCEncodeContext *encode_contexts;
  NSCEncodeContext *encode_context;
  SURFACE_BITS_COMMAND cmd = {0};
  BOOL first, last;
  int i;

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
  g_cond_signal (thread_pool_context->pending_jobs_cond);
  g_mutex_unlock (thread_pool_context->pending_jobs_mutex);
}

static void
rdp_peer_refresh_raw_rect (freerdp_peer          *peer,
                           GThreadPool           *thread_pool,
                           uint32_t              *pending_job_count,
                           BITMAP_DATA           *bitmap_data,
                           uint32_t              *n_bitmaps,
                           cairo_rectangle_int_t *cairo_rect,
                           uint8_t               *data)
{
  rdpSettings *rdp_settings = peer->settings;
  uint32_t dst_bits_per_pixel = rdp_settings->ColorDepth;
  uint32_t cols, rows;
  uint32_t x, y;
  BITMAP_DATA *bitmap;

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
          bitmap->destLeft = cairo_rect->x + x * 64;
          bitmap->destTop = cairo_rect->y + y * 64;

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

static void
rdp_peer_refresh_raw (freerdp_peer   *peer,
                      cairo_region_t *region,
                      uint8_t        *data)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  rdpSettings *rdp_settings = peer->settings;
  rdpUpdate *rdp_update = peer->update;
  uint32_t src_stride = grd_session_rdp_get_framebuffer_stride (session_rdp);
  RawThreadPoolContext *thread_pool_context =
    &rdp_peer_context->raw_thread_pool_context;
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
      rdp_peer_refresh_raw_rect (peer,
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
  max_update_size = rdp_settings->MultifragMaxRequestSize - 2;

  bitmap_update.count = bitmap_update.number = 0;
  bitmap_update.rectangles = bitmap_data;
  bitmap_update.skipCompression = FALSE;

  marker.frameId = rdp_peer_context->frame_id;
  marker.frameAction = SURFACECMD_FRAMEACTION_BEGIN;
  if (rdp_settings->SurfaceFrameMarkerEnabled)
    rdp_update->SurfaceFrameMarker (peer->context, &marker);

  for (i = 0; i < n_bitmaps; ++i)
    {
      /* We send 26 additional Bytes for each bitmap
       * (See also update_write_bitmap_data () in FreeRDP)
       */
      update_size += bitmap_data[i].bitmapLength + 26;

      ++bitmap_update.count;
      ++bitmap_update.number;

      next_size = i + 1 < n_bitmaps ? bitmap_data[i + 1].bitmapLength + 26
                                    : 0;
      if (!next_size || update_size + next_size > max_update_size)
        {
          rdp_update->BitmapUpdate (peer->context, &bitmap_update);

          bitmap_update.rectangles += bitmap_update.count;
          bitmap_update.count = bitmap_update.number = 0;
          update_size = 0;
        }
    }

  marker.frameAction = SURFACECMD_FRAMEACTION_END;
  if (rdp_settings->SurfaceFrameMarkerEnabled)
    rdp_update->SurfaceFrameMarker (peer->context, &marker);

  for (i = 0; i < n_bitmaps; ++i)
    g_free (bitmap_data[i].bitmapDataStream);

  g_free (bitmap_data);
}

static void
rdp_peer_refresh_region (freerdp_peer   *peer,
                         cairo_region_t *region,
                         uint8_t        *data)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;
  rdpSettings *rdp_settings = peer->settings;

  if (rdp_settings->RemoteFxCodec)
    rdp_peer_refresh_rfx (peer, region, data);
  else if (rdp_settings->NSCodec)
    rdp_peer_refresh_nsc (peer, region, data);
  else
    rdp_peer_refresh_raw (peer, region, data);

  ++rdp_peer_context->frame_id;
}

#ifdef HAS_RDP_UNICODE_INPUT
static gboolean
notify_keysym_released (gpointer key,
                        gpointer value,
                        gpointer user_data)
{
  GrdSession *session = (GrdSession *) user_data;
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;
  xkb_keysym_t keysym = GPOINTER_TO_UINT (key);

  grd_rdp_event_queue_add_input_event_keyboard_keysym (rdp_event_queue,
                                                       keysym,
                                                       GRD_KEY_STATE_RELEASED);

  return TRUE;
}
#endif

static BOOL
rdp_input_synchronize_event (rdpInput *rdp_input,
                             uint32_t  flags)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;
#ifdef HAS_RDP_UNICODE_INPUT
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
  GrdSession *session = GRD_SESSION (session_rdp);
#endif

  if (!is_rdp_peer_flag_set (rdp_peer_context, RDP_PEER_ACTIVATED))
    return TRUE;

#ifdef HAS_RDP_UNICODE_INPUT
  g_hash_table_foreach_remove (session_rdp->pressed_unicode_keys,
                               notify_keysym_released,
                               session);
#endif

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
  GrdButtonState button_state;
  int32_t button = 0;
  uint16_t axis_value;
  double axis_step;

  if (!is_rdp_peer_flag_set (rdp_peer_context, RDP_PEER_ACTIVATED) ||
      is_view_only (session_rdp))
    return TRUE;

  if (flags & PTR_FLAGS_MOVE)
    {
      g_mutex_lock (&session_rdp->pointer_mutex);
      session_rdp->pointer_x = x;
      session_rdp->pointer_y = y;
      g_mutex_unlock (&session_rdp->pointer_mutex);

      grd_rdp_event_queue_add_input_event_pointer_motion_abs (rdp_event_queue,
                                                              x, y);
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

  if (!is_rdp_peer_flag_set (rdp_peer_context, RDP_PEER_ACTIVATED) ||
      is_view_only (session_rdp))
    return TRUE;

  if (flags & PTR_FLAGS_MOVE)
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
rdp_input_keyboard_event (rdpInput *rdp_input,
                          uint16_t  flags,
                          uint16_t  code)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;

  if (!is_rdp_peer_flag_set (rdp_peer_context, RDP_PEER_ACTIVATED) ||
      is_view_only (session_rdp))
    return TRUE;

  return TRUE;
}

static BOOL
rdp_input_unicode_keyboard_event (rdpInput *rdp_input,
                                  uint16_t  flags,
                                  uint16_t  code_utf16)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_input->context;
  GrdSessionRdp *session_rdp = rdp_peer_context->session_rdp;
#ifdef HAS_RDP_UNICODE_INPUT
  GrdRdpEventQueue *rdp_event_queue = session_rdp->rdp_event_queue;
  uint32_t *code_utf32;
  xkb_keysym_t keysym;
  GrdKeyState key_state;
#endif

  if (!is_rdp_peer_flag_set (rdp_peer_context, RDP_PEER_ACTIVATED) ||
      is_view_only (session_rdp))
    return TRUE;

#ifdef HAS_RDP_UNICODE_INPUT
  code_utf32 = g_utf16_to_ucs4 (&code_utf16, 1, NULL, NULL, NULL);
  if (!code_utf32)
    return TRUE;

  keysym = xkb_utf32_to_keysym (*code_utf32);
  g_free (code_utf32);

  key_state = flags & KBD_FLAGS_DOWN ? GRD_KEY_STATE_PRESSED
                                     : GRD_KEY_STATE_RELEASED;

  if (flags & KBD_FLAGS_DOWN)
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
#endif

  return TRUE;
}

static BOOL
rdp_suppress_output (rdpContext         *rdp_context,
                     uint8_t             allow,
                     const RECTANGLE_16 *area)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;

  if (allow)
    set_rdp_peer_flag (rdp_peer_context, RDP_PEER_OUTPUT_ENABLED);
  else
    unset_rdp_peer_flag (rdp_peer_context, RDP_PEER_OUTPUT_ENABLED);

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
      /* Using the interleaved codec with a colour depth of 24
       * leads to bitmaps with artifacts.
       */
      g_message ("Downgrading colour depth from 24 to 16 due to issues with "
                 "the interleaved codec");
      rdp_settings->ColorDepth = 16;
    case 16:
    case 15:
      break;
    default:
      g_warning ("Invalid colour depth, closing connection");
      return FALSE;
    }

  if (!rdp_settings->DesktopResize)
    {
      g_warning ("Client doesn't support resizing, closing connection");
      return FALSE;
    }

  if (rdp_settings->PointerCacheSize <= 0)
    {
      g_warning ("Client doesn't have a pointer cache, closing connection");
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

  rdp_settings->PointerCacheSize = MIN (rdp_settings->PointerCacheSize, 100);

  grd_session_start (GRD_SESSION (session_rdp));

  sam_file = g_steal_pointer (&rdp_peer_context->sam_file);
  grd_rdp_sam_maybe_close_and_free_sam_file (sam_file);

  set_rdp_peer_flag (rdp_peer_context, RDP_PEER_ACTIVATED);

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
  rdpSettings *rdp_settings = peer->settings;

  rdp_peer_context->frame_id = 0;

  rdp_peer_context->rfx_context = rfx_context_new (TRUE);
  rdp_peer_context->rfx_context->mode = RLGR3;
  rfx_context_set_pixel_format (rdp_peer_context->rfx_context,
                                PIXEL_FORMAT_BGRX32);

  rdp_peer_context->encode_stream = Stream_New (NULL, 64 * 64 * 4);

  rdp_peer_context->planar_flags = 0;
  if (rdp_settings->DrawAllowSkipAlpha)
    rdp_peer_context->planar_flags |= PLANAR_FORMAT_HEADER_NA;
  rdp_peer_context->planar_flags |= PLANAR_FORMAT_HEADER_RLE;

  rdp_peer_context->vcm = WTSOpenServerA ((LPSTR) peer->context);

  g_mutex_init (&rdp_peer_context->flags_mutex);
  rdp_peer_context->flags = RDP_PEER_OUTPUT_ENABLED;

  return TRUE;
}

static void
rdp_peer_context_free (freerdp_peer   *peer,
                       RdpPeerContext *rdp_peer_context)
{
  if (!rdp_peer_context)
    return;

  g_clear_pointer (&rdp_peer_context->vcm, WTSCloseServer);

  if (rdp_peer_context->encode_stream)
    Stream_Free (rdp_peer_context->encode_stream, TRUE);

  if (rdp_peer_context->rfx_context)
    rfx_context_free (rdp_peer_context->rfx_context);
}

int
grd_session_rdp_get_stride_for_width (GrdSessionRdp *session_rdp,
                                      int            width)
{
  return width * 4;
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
  rdp_settings->HasExtendedMouseEvent = TRUE;
  rdp_settings->HasHorizontalWheel = TRUE;
  rdp_settings->RefreshRect = TRUE;
  rdp_settings->RemoteFxCodec = TRUE;
  rdp_settings->NSCodec = TRUE;
  rdp_settings->FrameMarkerCommandEnabled = TRUE;
  rdp_settings->SurfaceFrameMarkerEnabled = TRUE;
#ifdef HAS_RDP_UNICODE_INPUT
  rdp_settings->UnicodeInput = TRUE;
#endif

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
  session_rdp->last_pointer = NULL;
  session_rdp->thread_pool = NULL;

  SetEvent (session_rdp->start_event);
}

gpointer
socket_thread_func (gpointer data)
{
  GrdSessionRdp *session_rdp = data;
  freerdp_peer *peer;
  RdpPeerContext *rdp_peer_context;
  HANDLE channel_event;
  HANDLE events[32];
  uint32_t n_events;
  uint32_t n_freerdp_handles;

  WaitForSingleObject (session_rdp->start_event, INFINITE);

  peer = session_rdp->peer;
  rdp_peer_context = (RdpPeerContext *) peer->context;
  channel_event = WTSVirtualChannelManagerGetEventHandle (rdp_peer_context->vcm);

  while (TRUE)
    {
      n_events = 0;

      events[n_events++] = session_rdp->stop_event;

      n_freerdp_handles = peer->GetEventHandles (peer, &events[n_events],
                                                 32 - n_events);
      if (!n_freerdp_handles)
        {
          g_warning ("Failed to get FreeRDP transport event handles");
          handle_client_gone (session_rdp);
          break;
        }
      n_events += n_freerdp_handles;

      events[n_events++] = channel_event;

      WaitForMultipleObjects (n_events, events, FALSE, INFINITE);

      if (WaitForSingleObject (session_rdp->stop_event, 0) == WAIT_OBJECT_0)
        break;

      if (!peer->CheckFileDescriptor (peer))
        {
          g_message ("Unable to check file descriptor, closing connection");
          handle_client_gone (session_rdp);
          break;
        }

      if (WaitForSingleObject (channel_event, 0) == WAIT_OBJECT_0 &&
          !WTSVirtualChannelManagerCheckFileDescriptor (rdp_peer_context->vcm))
        {
          g_message ("Unable to check VCM file descriptor, closing connection");
          handle_client_gone (session_rdp);
          break;
        }
    }

  return NULL;
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
  session_rdp->start_event = CreateEvent (NULL, TRUE, FALSE, NULL);
  session_rdp->stop_event = CreateEvent (NULL, TRUE, FALSE, NULL);

  session_rdp->socket_thread = g_thread_new ("RDP socket thread",
                                             socket_thread_func,
                                             session_rdp);
  if (!session_rdp->socket_thread)
    {
      g_warning ("Failed to create socket thread");

      g_clear_pointer (&session_rdp->stop_event, CloseHandle);
      g_clear_pointer (&session_rdp->start_event, CloseHandle);
      g_object_unref (connection);
      g_free (password);
      g_free (username);

      return NULL;
    }

  init_rdp_session (session_rdp, username, password);

  g_free (password);
  g_free (username);

  return session_rdp;
}

static gboolean
clear_pointer_bitmap (gpointer key,
                      gpointer value,
                      gpointer user_data)
{
  Pointer *pointer = (Pointer *) key;

  g_free (pointer->bitmap);
  g_free (pointer);

  return TRUE;
}

static void
grd_session_rdp_stop (GrdSession *session)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;

  g_debug ("Stopping RDP session");

  if (WaitForSingleObject (session_rdp->stop_event, 0) == WAIT_TIMEOUT)
    {
      freerdp_set_error_info (peer->context->rdp,
                              ERRINFO_RPC_INITIATED_DISCONNECT);

      SetEvent (session_rdp->stop_event);
    }

  g_clear_object (&session_rdp->pipewire_stream);

  g_clear_object (&rdp_peer_context->clipboard_rdp);

  peer->Close (peer);
  g_clear_pointer (&session_rdp->socket_thread, g_thread_join);
  g_clear_object (&session_rdp->connection);

  if (rdp_peer_context->sam_file)
    grd_rdp_sam_maybe_close_and_free_sam_file (rdp_peer_context->sam_file);

  if (session_rdp->thread_pool)
    g_thread_pool_free (session_rdp->thread_pool, FALSE, TRUE);

  peer->Disconnect (peer);
  freerdp_peer_context_free (peer);
  freerdp_peer_free (peer);

#ifdef HAS_RDP_UNICODE_INPUT
  g_hash_table_foreach_remove (session_rdp->pressed_unicode_keys,
                               notify_keysym_released,
                               session);
#endif
  g_clear_object (&session_rdp->rdp_event_queue);

  g_clear_pointer (&session_rdp->last_frame, g_free);
  g_hash_table_foreach_remove (session_rdp->pointer_cache,
                               clear_pointer_bitmap,
                               NULL);

  g_clear_pointer (&session_rdp->stop_event, CloseHandle);
  g_clear_pointer (&session_rdp->start_event, CloseHandle);

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
on_pipewire_stream_closed (GrdRdpPipeWireStream *stream,
                           GrdSessionRdp        *session_rdp)
{
  g_warning ("PipeWire stream closed, closing client");

  maybe_queue_close_session_idle (session_rdp);
}

static void
grd_session_rdp_remote_desktop_session_ready (GrdSession *session)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (session);
  freerdp_peer *peer = session_rdp->peer;
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) peer->context;

  rdp_peer_context->clipboard_rdp = grd_clipboard_rdp_new (session_rdp,
                                                           rdp_peer_context->vcm,
                                                           session_rdp->stop_event);
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
}

static void
grd_session_rdp_dispose (GObject *object)
{
  GrdSessionRdp *session_rdp = GRD_SESSION_RDP (object);

#ifdef HAS_RDP_UNICODE_INPUT
  g_clear_pointer (&session_rdp->pressed_unicode_keys, g_hash_table_unref);
#endif
  g_clear_pointer (&session_rdp->pointer_cache, g_hash_table_unref);

  G_OBJECT_CLASS (grd_session_rdp_parent_class)->dispose (object);
}

static gboolean
are_pointer_bitmaps_equal (gconstpointer a,
                           gconstpointer b)
{
  Pointer *first = (Pointer *) a;
  Pointer *second = (Pointer *) b;
  cairo_rectangle_int_t cairo_rect;

  if (first->hotspot_x != second->hotspot_x ||
      first->hotspot_y != second->hotspot_y ||
      first->width != second->width ||
      first->height != second->height)
    return FALSE;

  cairo_rect.x = cairo_rect.y = 0;
  cairo_rect.width = first->width;
  cairo_rect.height = first->height;
  if (grd_is_tile_dirty (&cairo_rect,
                         first->bitmap,
                         second->bitmap,
                         first->width * 4, 4))
    return FALSE;

  return TRUE;
}

static void
grd_session_rdp_init (GrdSessionRdp *session_rdp)
{
  session_rdp->pointer_cache = g_hash_table_new (NULL, are_pointer_bitmaps_equal);
#ifdef HAS_RDP_UNICODE_INPUT
  session_rdp->pressed_unicode_keys = g_hash_table_new (NULL, NULL);
#endif

  g_cond_init (&session_rdp->pending_jobs_cond);
  g_mutex_init (&session_rdp->pending_jobs_mutex);
  g_mutex_init (&session_rdp->pointer_mutex);

  session_rdp->rdp_event_queue = grd_rdp_event_queue_new (session_rdp);
}

static void
grd_session_rdp_class_init (GrdSessionRdpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdSessionClass *session_class = GRD_SESSION_CLASS (klass);

  object_class->dispose = grd_session_rdp_dispose;

  session_class->remote_desktop_session_ready =
    grd_session_rdp_remote_desktop_session_ready;
  session_class->stop = grd_session_rdp_stop;
  session_class->stream_ready = grd_session_rdp_stream_ready;
}
