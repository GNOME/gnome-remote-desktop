/*
 * Copyright (C) 2024 Pascal Nowack
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

#include "grd-encode-session-ca-sw.h"

#include <gio/gio.h>

#include "grd-bitstream.h"
#include "grd-encode-context.h"
#include "grd-image-view-rgb.h"
#include "grd-local-buffer.h"
#include "grd-rdp-sw-encoder-ca.h"

/*
 * One surface is needed, when encoding a frame,
 * one is needed, when preparing the view,
 * one is needed for the submitted pending frame in the renderer,
 * and one is needed to prepare a new pending frame before it is submitted to
 * the renderer, where it then replaces the old pending frame
 *
 * In total, this makes four needed source surfaces
 */
#define N_SRC_SURFACES 4

/*
 * One encode stream is needed, when submitting a frame,
 * one is needed, when encoding a frame
 *
 * In total, this makes two needed encode streams
 */
#define N_ENCODE_STREAMS 2

#define INITIAL_STREAM_SIZE 16384

typedef struct
{
  GrdImageView *image_view;
} RGBSurface;

struct _GrdEncodeSessionCaSw
{
  GrdEncodeSession parent;

  GrdRdpSwEncoderCa *encoder_ca;

  uint32_t surface_width;
  uint32_t surface_height;

  GHashTable *surfaces;

  GMutex pending_encodes_mutex;
  GHashTable *pending_encodes;

  GMutex acquired_encode_streams_mutex;
  wStream *encode_streams[N_ENCODE_STREAMS];
  GHashTable *acquired_encode_streams;

  GMutex bitstreams_mutex;
  GHashTable *bitstreams;

  gboolean pending_header;
};

G_DEFINE_TYPE (GrdEncodeSessionCaSw, grd_encode_session_ca_sw,
               GRD_TYPE_ENCODE_SESSION)

static RGBSurface *
rgb_surface_new (void)
{
  RGBSurface *rgb_surface = NULL;
  GrdImageViewRGB *image_view_rgb;

  rgb_surface = g_new0 (RGBSurface, 1);

  image_view_rgb = grd_image_view_rgb_new ();
  rgb_surface->image_view = GRD_IMAGE_VIEW (image_view_rgb);

  return rgb_surface;
}

static void
rgb_surface_free (RGBSurface *rgb_surface)
{
  g_clear_object (&rgb_surface->image_view);

  g_free (rgb_surface);
}

static void
grd_encode_session_ca_sw_get_surface_size (GrdEncodeSession *encode_session,
                                           uint32_t         *surface_width,
                                           uint32_t         *surface_height)
{
  g_assert_not_reached ();
}

static GList *
grd_encode_session_ca_sw_get_image_views (GrdEncodeSession *encode_session)
{
  GrdEncodeSessionCaSw *encode_session_ca =
    GRD_ENCODE_SESSION_CA_SW (encode_session);

  return g_hash_table_get_keys (encode_session_ca->surfaces);
}

static gboolean
grd_encode_session_ca_sw_has_pending_frames (GrdEncodeSession *encode_session)
{
  GrdEncodeSessionCaSw *encode_session_ca =
    GRD_ENCODE_SESSION_CA_SW (encode_session);

  g_mutex_lock (&encode_session_ca->pending_encodes_mutex);
  g_assert (g_hash_table_size (encode_session_ca->pending_encodes) == 0);
  g_mutex_unlock (&encode_session_ca->pending_encodes_mutex);

  return FALSE;
}

static gboolean
grd_encode_session_ca_sw_encode_frame (GrdEncodeSession  *encode_session,
                                       GrdEncodeContext  *encode_context,
                                       GrdImageView      *image_view,
                                       GError           **error)
{
  GrdEncodeSessionCaSw *encode_session_ca =
    GRD_ENCODE_SESSION_CA_SW (encode_session);
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&encode_session_ca->pending_encodes_mutex);
  g_assert (!g_hash_table_contains (encode_session_ca->pending_encodes,
                                    image_view));

  g_hash_table_insert (encode_session_ca->pending_encodes,
                       image_view, encode_context);

  return TRUE;
}

static wStream *
acquire_encode_stream (GrdEncodeSessionCaSw *encode_session_ca)
{
  g_autoptr (GMutexLocker) locker = NULL;
  uint32_t i;

  locker = g_mutex_locker_new (&encode_session_ca->acquired_encode_streams_mutex);
  for (i = 0; i < N_ENCODE_STREAMS; ++i)
    {
      wStream *encode_stream = encode_session_ca->encode_streams[i];

      if (g_hash_table_contains (encode_session_ca->acquired_encode_streams,
                                 encode_stream))
        continue;

      g_hash_table_add (encode_session_ca->acquired_encode_streams,
                        encode_stream);
      return encode_stream;
    }

  g_assert_not_reached ();
  return NULL;
}

static void
release_encode_stream (GrdEncodeSessionCaSw *encode_session_ca,
                       wStream              *encode_stream)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&encode_session_ca->acquired_encode_streams_mutex);
  if (!g_hash_table_remove (encode_session_ca->acquired_encode_streams,
                            encode_stream))
    g_assert_not_reached ();
}

static GrdBitstream *
grd_encode_session_ca_sw_lock_bitstream (GrdEncodeSession  *encode_session,
                                         GrdImageView      *image_view,
                                         GError           **error)
{
  GrdEncodeSessionCaSw *encode_session_ca =
    GRD_ENCODE_SESSION_CA_SW (encode_session);
  GrdImageViewRGB *image_view_rgb = GRD_IMAGE_VIEW_RGB (image_view);
  GrdEncodeContext *encode_context = NULL;
  g_autoptr (GMutexLocker) locker = NULL;
  GrdLocalBuffer *local_buffer;
  uint8_t *buffer;
  uint32_t buffer_stride;
  cairo_region_t *damage_region;
  wStream *encode_stream;
  GrdBitstream *bitstream;

  locker = g_mutex_locker_new (&encode_session_ca->pending_encodes_mutex);
  if (!g_hash_table_lookup_extended (encode_session_ca->pending_encodes,
                                     image_view,
                                     NULL, (gpointer *) &encode_context))
    g_assert_not_reached ();

  g_clear_pointer (&locker, g_mutex_locker_free);
  g_assert (encode_context);

  local_buffer = grd_image_view_rgb_get_local_buffer (image_view_rgb);
  buffer = grd_local_buffer_get_buffer (local_buffer);
  buffer_stride = grd_local_buffer_get_buffer_stride (local_buffer);
  damage_region = grd_encode_context_get_damage_region (encode_context);
  encode_stream = acquire_encode_stream (encode_session_ca);

  grd_rdp_sw_encoder_ca_encode_progressive_frame (encode_session_ca->encoder_ca,
                                                  encode_session_ca->surface_width,
                                                  encode_session_ca->surface_height,
                                                  buffer,
                                                  buffer_stride,
                                                  damage_region,
                                                  encode_stream,
                                                  encode_session_ca->pending_header);

  encode_session_ca->pending_header = FALSE;

  bitstream = grd_bitstream_new (Stream_Buffer (encode_stream),
                                 Stream_Length (encode_stream));

  g_mutex_lock (&encode_session_ca->bitstreams_mutex);
  g_hash_table_insert (encode_session_ca->bitstreams, bitstream, encode_stream);
  g_mutex_unlock (&encode_session_ca->bitstreams_mutex);

  locker = g_mutex_locker_new (&encode_session_ca->pending_encodes_mutex);
  if (!g_hash_table_remove (encode_session_ca->pending_encodes, image_view))
    g_assert_not_reached ();

  return bitstream;
}

static gboolean
grd_encode_session_ca_sw_unlock_bitstream (GrdEncodeSession  *encode_session,
                                           GrdBitstream      *bitstream,
                                           GError           **error)
{
  GrdEncodeSessionCaSw *encode_session_ca =
    GRD_ENCODE_SESSION_CA_SW (encode_session);
  wStream *encode_stream = NULL;
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&encode_session_ca->bitstreams_mutex);
  if (!g_hash_table_steal_extended (encode_session_ca->bitstreams,
                                    bitstream,
                                    NULL, (gpointer *) &encode_stream))
    g_assert_not_reached ();

  g_clear_pointer (&locker, g_mutex_locker_free);
  g_assert (encode_stream);

  grd_bitstream_free (bitstream);
  release_encode_stream (encode_session_ca, encode_stream);

  return TRUE;
}

static gboolean
init_surfaces_and_streams (GrdEncodeSessionCaSw  *encode_session_ca,
                           GError               **error)
{
  uint32_t i;

  for (i = 0; i < N_SRC_SURFACES; ++i)
    {
      RGBSurface *surface;

      surface = rgb_surface_new ();

      g_hash_table_insert (encode_session_ca->surfaces,
                           surface->image_view, surface);
    }

  for (i = 0; i < N_ENCODE_STREAMS; ++i)
    {
      wStream *encode_stream;

      encode_stream = Stream_New (NULL, INITIAL_STREAM_SIZE);
      if (!encode_stream)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Failed to create encode stream");
          return FALSE;
        }
      encode_session_ca->encode_streams[i] = encode_stream;
    }

  return TRUE;
}

GrdEncodeSessionCaSw *
grd_encode_session_ca_sw_new (GrdRdpSwEncoderCa  *encoder_ca,
                              uint32_t            source_width,
                              uint32_t            source_height,
                              GError            **error)
{
  g_autoptr (GrdEncodeSessionCaSw) encode_session_ca = NULL;

  encode_session_ca = g_object_new (GRD_TYPE_ENCODE_SESSION_CA_SW, NULL);
  encode_session_ca->encoder_ca = encoder_ca;
  encode_session_ca->surface_width = source_width;
  encode_session_ca->surface_height = source_height;

  if (!init_surfaces_and_streams (encode_session_ca, error))
    return NULL;

  return g_steal_pointer (&encode_session_ca);
}

static void
encode_stream_free (wStream *encode_stream)
{
  Stream_Free (encode_stream, TRUE);
}

static void
grd_encode_session_ca_sw_dispose (GObject *object)
{
  GrdEncodeSessionCaSw *encode_session_ca = GRD_ENCODE_SESSION_CA_SW (object);
  uint32_t i;

  g_assert (g_hash_table_size (encode_session_ca->pending_encodes) == 0);
  g_assert (g_hash_table_size (encode_session_ca->acquired_encode_streams) == 0);
  g_assert (g_hash_table_size (encode_session_ca->bitstreams) == 0);

  for (i = 0; i < N_ENCODE_STREAMS; ++i)
    g_clear_pointer (&encode_session_ca->encode_streams[i], encode_stream_free);

  g_clear_pointer (&encode_session_ca->surfaces, g_hash_table_unref);

  G_OBJECT_CLASS (grd_encode_session_ca_sw_parent_class)->dispose (object);
}

static void
grd_encode_session_ca_sw_finalize (GObject *object)
{
  GrdEncodeSessionCaSw *encode_session_ca = GRD_ENCODE_SESSION_CA_SW (object);

  g_mutex_clear (&encode_session_ca->bitstreams_mutex);
  g_mutex_clear (&encode_session_ca->acquired_encode_streams_mutex);
  g_mutex_clear (&encode_session_ca->pending_encodes_mutex);

  g_clear_pointer (&encode_session_ca->bitstreams, g_hash_table_unref);
  g_clear_pointer (&encode_session_ca->acquired_encode_streams, g_hash_table_unref);
  g_clear_pointer (&encode_session_ca->pending_encodes, g_hash_table_unref);

  G_OBJECT_CLASS (grd_encode_session_ca_sw_parent_class)->finalize (object);
}

static void
grd_encode_session_ca_sw_init (GrdEncodeSessionCaSw *encode_session_ca)
{
  encode_session_ca->pending_header = TRUE;

  encode_session_ca->surfaces =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) rgb_surface_free);
  encode_session_ca->pending_encodes = g_hash_table_new (NULL, NULL);
  encode_session_ca->acquired_encode_streams = g_hash_table_new (NULL, NULL);
  encode_session_ca->bitstreams = g_hash_table_new (NULL, NULL);

  g_mutex_init (&encode_session_ca->pending_encodes_mutex);
  g_mutex_init (&encode_session_ca->acquired_encode_streams_mutex);
  g_mutex_init (&encode_session_ca->bitstreams_mutex);
}

static void
grd_encode_session_ca_sw_class_init (GrdEncodeSessionCaSwClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdEncodeSessionClass *encode_session_class =
    GRD_ENCODE_SESSION_CLASS (klass);

  object_class->dispose = grd_encode_session_ca_sw_dispose;
  object_class->finalize = grd_encode_session_ca_sw_finalize;

  encode_session_class->get_surface_size =
    grd_encode_session_ca_sw_get_surface_size;
  encode_session_class->get_image_views =
    grd_encode_session_ca_sw_get_image_views;
  encode_session_class->has_pending_frames =
    grd_encode_session_ca_sw_has_pending_frames;
  encode_session_class->encode_frame =
    grd_encode_session_ca_sw_encode_frame;
  encode_session_class->lock_bitstream =
    grd_encode_session_ca_sw_lock_bitstream;
  encode_session_class->unlock_bitstream =
    grd_encode_session_ca_sw_unlock_bitstream;
}
