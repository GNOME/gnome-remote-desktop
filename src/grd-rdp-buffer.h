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

#ifndef GRD_RDP_BUFFER_H
#define GRD_RDP_BUFFER_H

#include <ffnvcodec/dynlink_cuda.h>
#include <gio/gio.h>

#include "grd-types.h"

GrdRdpBuffer *grd_rdp_buffer_new (GrdRdpBufferPool *buffer_pool,
                                  GrdEglThread     *egl_thread,
                                  GrdHwAccelNvidia *hwaccel_nvidia,
                                  CUstream          cuda_stream,
                                  uint32_t          width,
                                  uint32_t          height,
                                  uint32_t          stride,
                                  gboolean          preallocate_on_gpu);

void grd_rdp_buffer_free (GrdRdpBuffer *rdp_buffer);

uint32_t grd_rdp_buffer_get_width (GrdRdpBuffer *rdp_buffer);

uint32_t grd_rdp_buffer_get_height (GrdRdpBuffer *rdp_buffer);

uint8_t *grd_rdp_buffer_get_local_data (GrdRdpBuffer *rdp_buffer);

uint32_t grd_rdp_buffer_get_pbo (GrdRdpBuffer *rdp_buffer);

CUdeviceptr grd_rdp_buffer_get_mapped_cuda_pointer (GrdRdpBuffer *rdp_buffer);

void grd_rdp_buffer_release (GrdRdpBuffer *rdp_buffer);

gboolean grd_rdp_buffer_register_read_only_gl_buffer (GrdRdpBuffer *rdp_buffer,
                                                      uint32_t      pbo);

gboolean grd_rdp_buffer_map_cuda_resource (GrdRdpBuffer *rdp_buffer);

void grd_rdp_buffer_unmap_cuda_resource (GrdRdpBuffer *rdp_buffer);

void grd_rdp_buffer_queue_resource_unmap (GrdRdpBuffer *rdp_buffer);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdRdpBuffer, grd_rdp_buffer_free)

#endif /* GRD_RDP_BUFFER_H */
