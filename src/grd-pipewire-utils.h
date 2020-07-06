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

#ifndef GRD_PIPEWIRE_UTILS_H
#define GRD_PIPEWIRE_UTILS_H

#include <gio/gio.h>
#include <stdint.h>

#include "grd-types.h"

#define CURSOR_META_SIZE(width, height) \
 (sizeof(struct spa_meta_cursor) + \
  sizeof(struct spa_meta_bitmap) + width * height * 4)

typedef struct _GrdPipeWireSource
{
  GSource base;

  struct pw_loop *pipewire_loop;
} GrdPipeWireSource;

void grd_maybe_initialize_pipewire (void);

gboolean grd_spa_pixel_format_to_grd_pixel_format (uint32_t        spa_format,
                                                   GrdPixelFormat *out_format);

void grd_sync_dma_buf (int      fd,
                       uint64_t start_or_end);

#endif /* GRD_PIPEWIRE_UTILS_H */
