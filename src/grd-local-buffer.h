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

#ifndef GRD_LOCAL_BUFFER_H
#define GRD_LOCAL_BUFFER_H

#include <glib-object.h>
#include <stdint.h>

#define GRD_TYPE_LOCAL_BUFFER (grd_local_buffer_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdLocalBuffer, grd_local_buffer,
                          GRD, LOCAL_BUFFER, GObject)

struct _GrdLocalBufferClass
{
  GObjectClass parent_class;

  uint8_t *(* get_buffer) (GrdLocalBuffer *local_buffer);
  uint32_t (* get_buffer_stride) (GrdLocalBuffer *local_buffer);
};

uint8_t *grd_local_buffer_get_buffer (GrdLocalBuffer *local_buffer);

uint32_t grd_local_buffer_get_buffer_stride (GrdLocalBuffer *local_buffer);

#endif /* GRD_LOCAL_BUFFER_H */
