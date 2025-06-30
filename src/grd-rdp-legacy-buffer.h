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
#include <gio/gio.h>

#include "grd-types.h"

GrdRdpLegacyBuffer *grd_rdp_legacy_buffer_new (GrdRdpBufferPool *buffer_pool,
                                               GrdEglThread     *egl_thread,
                                               GrdHwAccelNvidia *hwaccel_nvidia,
                                               CUstream          cuda_stream,
                                               uint32_t          height,
                                               uint32_t          stride,
                                               gboolean          preallocate_on_gpu);

void grd_rdp_legacy_buffer_free (GrdRdpLegacyBuffer *buffer);

uint32_t grd_rdp_legacy_buffer_get_stride (GrdRdpLegacyBuffer *buffer);

uint8_t *grd_rdp_legacy_buffer_get_local_data (GrdRdpLegacyBuffer *buffer);

uint32_t grd_rdp_legacy_buffer_get_pbo (GrdRdpLegacyBuffer *buffer);

CUdeviceptr grd_rdp_legacy_buffer_get_mapped_cuda_pointer (GrdRdpLegacyBuffer *buffer);

void grd_rdp_legacy_buffer_release (GrdRdpLegacyBuffer *buffer);

gboolean grd_rdp_legacy_buffer_register_read_only_gl_buffer (GrdRdpLegacyBuffer *buffer,
                                                             uint32_t            pbo);

gboolean grd_rdp_legacy_buffer_map_cuda_resource (GrdRdpLegacyBuffer *buffer);

void grd_rdp_legacy_buffer_unmap_cuda_resource (GrdRdpLegacyBuffer *buffer);

void grd_rdp_legacy_buffer_queue_resource_unmap (GrdRdpLegacyBuffer *buffer);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdRdpLegacyBuffer, grd_rdp_legacy_buffer_free)
