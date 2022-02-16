/*
 * Copyright (C) 2021 Pascal Nowack
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

#ifndef GRD_HWACCEL_NVIDIA_H
#define GRD_HWACCEL_NVIDIA_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_HWACCEL_NVIDIA (grd_hwaccel_nvidia_get_type ())
G_DECLARE_FINAL_TYPE (GrdHwAccelNvidia, grd_hwaccel_nvidia,
                      GRD, HWACCEL_NVIDIA, GObject)

GrdHwAccelNvidia *grd_hwaccel_nvidia_new (GrdEglThread *egl_thread);

void grd_hwaccel_nvidia_push_cuda_context (GrdHwAccelNvidia *hwaccel_nvidia);

void grd_hwaccel_nvidia_pop_cuda_context (GrdHwAccelNvidia *hwaccel_nvidia);

gboolean grd_hwaccel_nvidia_create_nvenc_session (GrdHwAccelNvidia *hwaccel_nvidia,
                                                  uint32_t         *encode_session_id,
                                                  uint16_t          surface_width,
                                                  uint16_t          surface_height,
                                                  uint16_t          refresh_rate);

void grd_hwaccel_nvidia_free_nvenc_session (GrdHwAccelNvidia *hwaccel_nvidia,
                                            uint32_t          encode_session_id);

gboolean grd_hwaccel_nvidia_avc420_encode_bgrx_frame (GrdHwAccelNvidia  *hwaccel_nvidia,
                                                      uint32_t           encode_session_id,
                                                      uint8_t           *src_data,
                                                      uint16_t           src_width,
                                                      uint16_t           src_height,
                                                      uint16_t           aligned_width,
                                                      uint16_t           aligned_height,
                                                      uint8_t          **bitstream,
                                                      uint32_t          *bitstream_size);

#endif /* GRD_HWACCEL_NVIDIA_H */
