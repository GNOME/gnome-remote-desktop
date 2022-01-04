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
#include <stdint.h>

#include "grd-types.h"

struct _GrdRdpBuffer
{
  GrdRdpBufferPool *buffer_pool;

  GrdEglThread *egl_thread;
  GrdHwAccelNvidia *hwaccel_nvidia;

  uint32_t width;
  uint32_t height;

  uint8_t *local_data;

  uint32_t pbo;

  CUgraphicsResource cuda_resource;
  CUstream cuda_stream;
  CUdeviceptr mapped_cuda_pointer;
};

GrdRdpBuffer *grd_rdp_buffer_new (GrdRdpBufferPool *buffer_pool,
                                  GrdEglThread     *egl_thread,
                                  GrdHwAccelNvidia *hwaccel_nvidia,
                                  CUstream          cuda_stream);

void grd_rdp_buffer_free (GrdRdpBuffer *buffer);

void grd_rdp_buffer_release (GrdRdpBuffer *buffer);

gboolean grd_rdp_buffer_resize (GrdRdpBuffer *buffer,
                                uint32_t      width,
                                uint32_t      height,
                                uint32_t      stride,
                                gboolean      preallocate_on_gpu);

#endif /* GRD_RDP_BUFFER_H */
