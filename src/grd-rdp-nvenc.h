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

#ifndef GRD_RDP_NVENC_H
#define GRD_RDP_NVENC_H

#include <glib-object.h>
#include <stdint.h>

#define GRD_TYPE_RDP_NVENC (grd_rdp_nvenc_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpNvenc, grd_rdp_nvenc,
                      GRD, RDP_NVENC, GObject);

GrdRdpNvenc *grd_rdp_nvenc_new (void);

gboolean grd_rdp_nvenc_create_encode_session (GrdRdpNvenc *rdp_nvenc,
                                              uint32_t    *encode_session_id,
                                              uint16_t     surface_width,
                                              uint16_t     surface_height,
                                              uint16_t     refresh_rate);

void grd_rdp_nvenc_free_encode_session (GrdRdpNvenc *rdp_nvenc,
                                        uint32_t     encode_session_id);

gboolean grd_rdp_nvenc_avc420_encode_bgrx_frame (GrdRdpNvenc  *rdp_nvenc,
                                                 uint32_t      encode_session_id,
                                                 uint8_t      *src_data,
                                                 uint16_t      src_width,
                                                 uint16_t      src_height,
                                                 uint16_t      aligned_width,
                                                 uint16_t      aligned_height,
                                                 uint8_t     **bitstream,
                                                 uint32_t     *bitstream_size);

#endif /* GRD_RDP_NVENC_H */
