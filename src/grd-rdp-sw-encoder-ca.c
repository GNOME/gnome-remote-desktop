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

#include "grd-rdp-sw-encoder-ca.h"

#include <gio/gio.h>

struct _GrdRdpSwEncoderCa
{
  GObject parent;

  GMutex encode_mutex;
  RFX_CONTEXT *rfx_context;
};

G_DEFINE_TYPE (GrdRdpSwEncoderCa, grd_rdp_sw_encoder_ca, G_TYPE_OBJECT)

static void
rfx_progressive_write_message (RFX_MESSAGE *rfx_message,
                               wStream     *s,
                               gboolean     write_header)
{
  const RFX_RECT *rfx_rects;
  uint16_t n_rfx_rects = 0;
  const UINT32 *quant_vals;
  uint16_t n_quant_vals = 0;
  const RFX_TILE **rfx_tiles;
  uint16_t n_rfx_tiles = 0;
  uint32_t block_len;
  const uint32_t *qv;
  const RFX_TILE *rfx_tile;
  uint32_t tiles_data_size;
  uint16_t i;

  rfx_rects = rfx_message_get_rects (rfx_message, &n_rfx_rects);
  quant_vals = rfx_message_get_quants (rfx_message, &n_quant_vals);
  rfx_tiles = rfx_message_get_tiles (rfx_message, &n_rfx_tiles);

  if (write_header)
    {
      /* RFX_PROGRESSIVE_SYNC */
      block_len = 12;
      if (!Stream_EnsureRemainingCapacity (s, block_len))
        g_assert_not_reached ();

      Stream_Write_UINT16 (s, 0xCCC0);     /* blockType */
      Stream_Write_UINT32 (s, block_len);  /* blockLen */
      Stream_Write_UINT32 (s, 0xCACCACCA); /* magic */
      Stream_Write_UINT16 (s, 0x0100);     /* version */

      /* RFX_PROGRESSIVE_CONTEXT */
      block_len = 10;
      if (!Stream_EnsureRemainingCapacity (s, block_len))
        g_assert_not_reached ();

      Stream_Write_UINT16 (s, 0xCCC3);    /* blockType */
      Stream_Write_UINT32 (s, block_len); /* blockLen */
      Stream_Write_UINT8 (s, 0);          /* ctxId */
      Stream_Write_UINT16 (s, 0x0040);    /* tileSize */
      Stream_Write_UINT8 (s, 0);          /* flags */
    }

  /* RFX_PROGRESSIVE_FRAME_BEGIN */
  block_len = 12;
  if (!Stream_EnsureRemainingCapacity (s, block_len))
    g_assert_not_reached ();

  Stream_Write_UINT16 (s, 0xCCC1);                                  /* blockType */
  Stream_Write_UINT32 (s, block_len);                               /* blockLen */
  Stream_Write_UINT32 (s, rfx_message_get_frame_idx (rfx_message)); /* frameIndex */
  Stream_Write_UINT16 (s, 1);                                       /* regionCount */

  /* RFX_PROGRESSIVE_REGION */
  block_len = 18;
  block_len += n_rfx_rects * 8;
  block_len += n_quant_vals * 5;
  tiles_data_size = n_rfx_tiles * 22;

  for (i = 0; i < n_rfx_tiles; i++)
    {
      rfx_tile = rfx_tiles[i];
      tiles_data_size += rfx_tile->YLen + rfx_tile->CbLen + rfx_tile->CrLen;
    }

  block_len += tiles_data_size;
  if (!Stream_EnsureRemainingCapacity (s, block_len))
    g_assert_not_reached ();

  Stream_Write_UINT16 (s, 0xCCC4);          /* blockType */
  Stream_Write_UINT32 (s, block_len);       /* blockLen */
  Stream_Write_UINT8 (s, 0x40);             /* tileSize */
  Stream_Write_UINT16 (s, n_rfx_rects);     /* numRects */
  Stream_Write_UINT8 (s, n_quant_vals);     /* numQuant */
  Stream_Write_UINT8 (s, 0);                /* numProgQuant */
  Stream_Write_UINT8 (s, 0);                /* flags */
  Stream_Write_UINT16 (s, n_rfx_tiles);     /* numTiles */
  Stream_Write_UINT32 (s, tiles_data_size); /* tilesDataSize */

  for (i = 0; i < n_rfx_rects; i++)
    {
      /* TS_RFX_RECT */
      Stream_Write_UINT16 (s, rfx_rects[i].x);      /* x */
      Stream_Write_UINT16 (s, rfx_rects[i].y);      /* y */
      Stream_Write_UINT16 (s, rfx_rects[i].width);  /* width */
      Stream_Write_UINT16 (s, rfx_rects[i].height); /* height */
    }

  /*
   * The RFX_COMPONENT_CODEC_QUANT structure differs from the
   * TS_RFX_CODEC_QUANT ([MS-RDPRFX] section 2.2.2.1.5) structure with respect
   * to the order of the bands.
   *             0    1    2    3    4    5    6    7    8    9
   * RDPRFX:   LL3, LH3, HL3, HH3, LH2, HL2, HH2, LH1, HL1, HH1
   * RDPEGFX:  LL3, HL3, LH3, HH3, HL2, LH2, HH2, HL1, LH1, HH1
   */
  for (i = 0, qv = quant_vals; i < n_quant_vals; ++i, qv += 10)
    {
      /* RFX_COMPONENT_CODEC_QUANT */
      Stream_Write_UINT8 (s, qv[0] + (qv[2] << 4)); /* LL3, HL3 */
      Stream_Write_UINT8 (s, qv[1] + (qv[3] << 4)); /* LH3, HH3 */
      Stream_Write_UINT8 (s, qv[5] + (qv[4] << 4)); /* HL2, LH2 */
      Stream_Write_UINT8 (s, qv[6] + (qv[8] << 4)); /* HH2, HL1 */
      Stream_Write_UINT8 (s, qv[7] + (qv[9] << 4)); /* LH1, HH1 */
    }

  for (i = 0; i < n_rfx_tiles; ++i)
    {
      /* RFX_PROGRESSIVE_TILE_SIMPLE */
      rfx_tile = rfx_tiles[i];
      block_len = 22 + rfx_tile->YLen + rfx_tile->CbLen + rfx_tile->CrLen;
      Stream_Write_UINT16 (s, 0xCCC5);                     /* blockType */
      Stream_Write_UINT32 (s, block_len);                  /* blockLen */
      Stream_Write_UINT8 (s, rfx_tile->quantIdxY);         /* quantIdxY */
      Stream_Write_UINT8 (s, rfx_tile->quantIdxCb);        /* quantIdxCb */
      Stream_Write_UINT8 (s, rfx_tile->quantIdxCr);        /* quantIdxCr */
      Stream_Write_UINT16 (s, rfx_tile->xIdx);             /* xIdx */
      Stream_Write_UINT16 (s, rfx_tile->yIdx);             /* yIdx */
      Stream_Write_UINT8 (s, 0);                           /* flags */
      Stream_Write_UINT16 (s, rfx_tile->YLen);             /* YLen */
      Stream_Write_UINT16 (s, rfx_tile->CbLen);            /* CbLen */
      Stream_Write_UINT16 (s, rfx_tile->CrLen);            /* CrLen */
      Stream_Write_UINT16 (s, 0);                          /* tailLen */
      Stream_Write (s, rfx_tile->YData, rfx_tile->YLen);   /* YData */
      Stream_Write (s, rfx_tile->CbData, rfx_tile->CbLen); /* CbData */
      Stream_Write (s, rfx_tile->CrData, rfx_tile->CrLen); /* CrData */
    }

  /* RFX_PROGRESSIVE_FRAME_END */
  block_len = 6;
  if (!Stream_EnsureRemainingCapacity (s, block_len))
    g_assert_not_reached ();

  Stream_Write_UINT16 (s, 0xCCC2);    /* blockType */
  Stream_Write_UINT32 (s, block_len); /* blockLen */

  Stream_SealLength (s);
}

void
grd_rdp_sw_encoder_ca_encode_progressive_frame (GrdRdpSwEncoderCa *encoder_ca,
                                                uint32_t           surface_width,
                                                uint32_t           surface_height,
                                                uint8_t           *src_buffer,
                                                uint32_t           src_stride,
                                                cairo_region_t    *damage_region,
                                                wStream           *encode_stream,
                                                gboolean           write_header)
{
  g_autoptr (GMutexLocker) locker = NULL;
  g_autofree RFX_RECT *rfx_rects = NULL;
  int n_rects;
  RFX_MESSAGE *rfx_message;
  int i;

  locker = g_mutex_locker_new (&encoder_ca->encode_mutex);
  rfx_context_set_mode (encoder_ca->rfx_context, RLGR1);
  rfx_context_reset (encoder_ca->rfx_context, surface_width, surface_height);

  n_rects = cairo_region_num_rectangles (damage_region);
  rfx_rects = g_new0 (RFX_RECT, n_rects);

  for (i = 0; i < n_rects; ++i)
    {
      RFX_RECT *rfx_rect = &rfx_rects[i];
      cairo_rectangle_int_t cairo_rect;

      cairo_region_get_rectangle (damage_region, i, &cairo_rect);

      rfx_rect->x = cairo_rect.x;
      rfx_rect->y = cairo_rect.y;
      rfx_rect->width = cairo_rect.width;
      rfx_rect->height = cairo_rect.height;
    }

  rfx_message = rfx_encode_message (encoder_ca->rfx_context,
                                    rfx_rects, n_rects,
                                    src_buffer,
                                    surface_width, surface_height,
                                    src_stride);

  Stream_SetPosition (encode_stream, 0);
  rfx_progressive_write_message (rfx_message, encode_stream, write_header);
  rfx_message_free (encoder_ca->rfx_context, rfx_message);
}

GrdRdpSwEncoderCa *
grd_rdp_sw_encoder_ca_new (GError **error)
{
  g_autoptr (GrdRdpSwEncoderCa) encoder_ca = NULL;

  encoder_ca = g_object_new (GRD_TYPE_RDP_SW_ENCODER_CA, NULL);

  encoder_ca->rfx_context = rfx_context_new (TRUE);
  if (!encoder_ca->rfx_context)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create RFX context");
      return NULL;
    }
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  rfx_context_set_pixel_format (encoder_ca->rfx_context,
                                PIXEL_FORMAT_BGRX32);
#else
  rfx_context_set_pixel_format (encoder_ca->rfx_context,
                                PIXEL_FORMAT_XRGB32);
#endif

  return g_steal_pointer (&encoder_ca);
}

static void
grd_rdp_sw_encoder_ca_dispose (GObject *object)
{
  GrdRdpSwEncoderCa *encoder_ca = GRD_RDP_SW_ENCODER_CA (object);

  g_clear_pointer (&encoder_ca->rfx_context, rfx_context_free);

  G_OBJECT_CLASS (grd_rdp_sw_encoder_ca_parent_class)->dispose (object);
}

static void
grd_rdp_sw_encoder_ca_finalize (GObject *object)
{
  GrdRdpSwEncoderCa *encoder_ca = GRD_RDP_SW_ENCODER_CA (object);

  g_mutex_clear (&encoder_ca->encode_mutex);

  G_OBJECT_CLASS (grd_rdp_sw_encoder_ca_parent_class)->finalize (object);
}

static void
grd_rdp_sw_encoder_ca_init (GrdRdpSwEncoderCa *encoder_ca)
{
  g_mutex_init (&encoder_ca->encode_mutex);
}

static void
grd_rdp_sw_encoder_ca_class_init (GrdRdpSwEncoderCaClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_sw_encoder_ca_dispose;
  object_class->finalize = grd_rdp_sw_encoder_ca_finalize;
}
