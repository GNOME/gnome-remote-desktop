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

#pragma once

#include <ffnvcodec/dynlink_cuda.h>
#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_BUFFER_POOL (grd_rdp_buffer_pool_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpBufferPool, grd_rdp_buffer_pool,
                      GRD, RDP_BUFFER_POOL, GObject)

GrdRdpBufferPool *grd_rdp_buffer_pool_new (GrdRdpSurface *rdp_surface,
                                           uint32_t       minimum_size);

GrdRdpSurface *grd_rdp_buffer_pool_get_surface (GrdRdpBufferPool *buffer_pool);

gboolean grd_rdp_buffer_pool_resize_buffers (GrdRdpBufferPool *buffer_pool,
                                             uint32_t          buffer_height,
                                             uint32_t          buffer_stride);

GrdRdpLegacyBuffer *grd_rdp_buffer_pool_acquire (GrdRdpBufferPool *buffer_pool);

void grd_rdp_buffer_pool_release_buffer (GrdRdpBufferPool   *buffer_pool,
                                         GrdRdpLegacyBuffer *buffer);
