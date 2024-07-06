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

#include "grd-bitstream.h"

#include <glib.h>

struct _GrdBitstream
{
  uint8_t *data;
  uint32_t data_size;
};

uint8_t *
grd_bitstream_get_data (GrdBitstream *bitstream)
{
  return bitstream->data;
}

uint32_t
grd_bitstream_get_data_size (GrdBitstream *bitstream)
{
  return bitstream->data_size;
}

GrdBitstream *
grd_bitstream_new (uint8_t  *data,
                   uint32_t  data_size)
{
  GrdBitstream *bitstream;

  bitstream = g_new0 (GrdBitstream, 1);
  bitstream->data = data;
  bitstream->data_size = data_size;

  return bitstream;
}

void
grd_bitstream_free (GrdBitstream *bitstream)
{
  g_free (bitstream);
}
