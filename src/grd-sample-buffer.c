/*
 * Copyright (C) 2025 Pascal Nowack
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

#include "grd-sample-buffer.h"

#include <gio/gio.h>

struct _GrdSampleBuffer
{
  uint8_t *data;
  size_t max_size;

  size_t used_size;
};

uint8_t *
grd_sample_buffer_get_data_pointer (GrdSampleBuffer *sample_buffer)
{
  return sample_buffer->data;
}

gboolean
grd_sample_buffer_load_sample (GrdSampleBuffer  *sample_buffer,
                               uint8_t          *sample_data,
                               size_t            sample_size,
                               GError          **error)
{
  if (sample_size > sample_buffer->max_size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "The sample to load "
                   "is too large (have: %zu Bytes, need: %zu Bytes)",
                   sample_buffer->max_size, sample_size);
      return FALSE;
    }

  memcpy (sample_buffer->data, sample_data, sample_size);
  sample_buffer->used_size = sample_size;

  return TRUE;
}

size_t
grd_sample_buffer_get_sample_size (GrdSampleBuffer *sample_buffer)
{
  return sample_buffer->used_size;
}

GrdSampleBuffer *
grd_sample_buffer_new (uint8_t *data,
                       size_t   data_size)
{
  GrdSampleBuffer *sample_buffer;

  sample_buffer = g_new0 (GrdSampleBuffer, 1);
  sample_buffer->data = data;
  sample_buffer->max_size = data_size;

  return sample_buffer;
}

void
grd_sample_buffer_free (GrdSampleBuffer *sample_buffer)
{
  g_free (sample_buffer);
}
