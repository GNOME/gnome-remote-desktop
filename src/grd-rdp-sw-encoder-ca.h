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

#ifndef GRD_RDP_SW_ENCODER_CA_H
#define GRD_RDP_SW_ENCODER_CA_H

#include <cairo/cairo.h>
#include <freerdp/freerdp.h>
#include <glib-object.h>

#define GRD_TYPE_RDP_SW_ENCODER_CA (grd_rdp_sw_encoder_ca_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpSwEncoderCa, grd_rdp_sw_encoder_ca,
                      GRD, RDP_SW_ENCODER_CA, GObject)

GrdRdpSwEncoderCa *grd_rdp_sw_encoder_ca_new (GError **error);

void grd_rdp_sw_encoder_ca_encode_progressive_frame (GrdRdpSwEncoderCa *encoder_ca,
                                                     uint32_t           surface_width,
                                                     uint32_t           surface_height,
                                                     uint8_t           *src_buffer,
                                                     uint32_t           src_stride,
                                                     cairo_region_t    *damage_region,
                                                     wStream           *encode_stream,
                                                     gboolean           write_header);

#endif /* GRD_RDP_SW_ENCODER_CA_H */
