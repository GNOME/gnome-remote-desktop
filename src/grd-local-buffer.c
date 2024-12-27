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

#include "grd-local-buffer.h"

G_DEFINE_ABSTRACT_TYPE (GrdLocalBuffer, grd_local_buffer,
                        G_TYPE_OBJECT)

uint8_t *
grd_local_buffer_get_buffer (GrdLocalBuffer *local_buffer)
{
  GrdLocalBufferClass *klass = GRD_LOCAL_BUFFER_GET_CLASS (local_buffer);

  return klass->get_buffer (local_buffer);
}

uint32_t
grd_local_buffer_get_buffer_stride (GrdLocalBuffer *local_buffer)
{
  GrdLocalBufferClass *klass = GRD_LOCAL_BUFFER_GET_CLASS (local_buffer);

  return klass->get_buffer_stride (local_buffer);
}

static void
grd_local_buffer_init (GrdLocalBuffer *local_buffer)
{
}

static void
grd_local_buffer_class_init (GrdLocalBufferClass *klass)
{
}
