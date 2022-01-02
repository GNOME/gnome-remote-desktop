/*
 * Copyright (C) 2022 Pascal Nowack
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

#include "grd-rdp-buffer.h"

#include <gio/gio.h>

GrdRdpBuffer *
grd_rdp_buffer_new (void)
{
  GrdRdpBuffer *buffer;

  buffer = g_new0 (GrdRdpBuffer, 1);

  return buffer;
}

static void
clear_buffers (GrdRdpBuffer *buffer)
{
  g_clear_pointer (&buffer->local_data, g_free);
}

void
grd_rdp_buffer_free (GrdRdpBuffer *buffer)
{
  clear_buffers (buffer);
  g_free (buffer);
}

void
grd_rdp_buffer_resize (GrdRdpBuffer *buffer,
                       uint32_t      width,
                       uint32_t      height,
                       uint32_t      stride)
{
  clear_buffers (buffer);

  buffer->width = width;
  buffer->height = height;
  buffer->local_data = g_malloc0 (stride * height * sizeof (uint8_t));
}
