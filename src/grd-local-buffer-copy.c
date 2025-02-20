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

#include "grd-local-buffer-copy.h"

struct _GrdLocalBufferCopy
{
  GrdLocalBuffer parent;

  uint8_t *buffer;
  uint32_t buffer_stride;
};

G_DEFINE_TYPE (GrdLocalBufferCopy, grd_local_buffer_copy,
               GRD_TYPE_LOCAL_BUFFER)

static uint8_t *
grd_local_buffer_copy_get_buffer (GrdLocalBuffer *local_buffer)
{
  GrdLocalBufferCopy *local_buffer_copy = GRD_LOCAL_BUFFER_COPY (local_buffer);

  return local_buffer_copy->buffer;
}

static uint32_t
grd_local_buffer_copy_get_buffer_stride (GrdLocalBuffer *local_buffer)
{
  GrdLocalBufferCopy *local_buffer_copy = GRD_LOCAL_BUFFER_COPY (local_buffer);

  return local_buffer_copy->buffer_stride;
}

GrdLocalBufferCopy *
grd_local_buffer_copy_new (uint32_t buffer_width,
                           uint32_t buffer_height)
{
  GrdLocalBufferCopy *local_buffer_copy;
  uint32_t buffer_stride;

  g_assert (buffer_width > 0);
  g_assert (buffer_height > 0);

  local_buffer_copy = g_object_new (GRD_TYPE_LOCAL_BUFFER_COPY, NULL);

  buffer_stride = buffer_width * 4;

  local_buffer_copy->buffer = g_new0 (uint8_t, buffer_stride * buffer_height);
  local_buffer_copy->buffer_stride = buffer_stride;

  return local_buffer_copy;
}

static void
grd_local_buffer_copy_dispose (GObject *object)
{
  GrdLocalBufferCopy *local_buffer_copy = GRD_LOCAL_BUFFER_COPY (object);

  g_clear_pointer (&local_buffer_copy->buffer, g_free);

  G_OBJECT_CLASS (grd_local_buffer_copy_parent_class)->dispose (object);
}

static void
grd_local_buffer_copy_init (GrdLocalBufferCopy *local_buffer)
{
}

static void
grd_local_buffer_copy_class_init (GrdLocalBufferCopyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdLocalBufferClass *local_buffer_class = GRD_LOCAL_BUFFER_CLASS (klass);

  object_class->dispose = grd_local_buffer_copy_dispose;

  local_buffer_class->get_buffer = grd_local_buffer_copy_get_buffer;
  local_buffer_class->get_buffer_stride =
    grd_local_buffer_copy_get_buffer_stride;
}
