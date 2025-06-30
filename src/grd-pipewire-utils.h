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

#pragma once

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <stdint.h>
#include <spa/param/video/format-utils.h>

#include "grd-types.h"

#define CURSOR_META_SIZE(width, height) \
 (sizeof(struct spa_meta_cursor) + \
  sizeof(struct spa_meta_bitmap) + width * height * 4)

typedef struct _GrdPipeWireSource
{
  GSource base;

  char *message_tag;
  struct pw_loop *pipewire_loop;
} GrdPipeWireSource;

GrdPipeWireSource *grd_attached_pipewire_source_new (const char  *message_tag,
                                                     GError     **error);

gboolean grd_pipewire_buffer_has_pointer_bitmap (struct pw_buffer *buffer);

gboolean grd_pipewire_buffer_has_frame_data (struct pw_buffer *buffer);

gboolean grd_spa_pixel_format_to_grd_pixel_format (uint32_t        spa_format,
                                                   GrdPixelFormat *out_format);

void grd_get_spa_format_details (enum spa_video_format  spa_format,
                                 uint32_t              *drm_format,
                                 int                   *bpp);
