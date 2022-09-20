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

#ifndef GRD_NAL_WRITER_H
#define GRD_NAL_WRITER_H

#include <glib-object.h>
#include <va/va.h>

#define GRD_TYPE_NAL_WRITER (grd_nal_writer_get_type ())
G_DECLARE_FINAL_TYPE (GrdNalWriter, grd_nal_writer,
                      GRD, NAL_WRITER, GObject)

GrdNalWriter *grd_nal_writer_new (void);

uint8_t *grd_nal_writer_get_aud_bitstream (GrdNalWriter *nal_writer,
                                           uint32_t     *bitstream_length);

uint8_t *grd_nal_writer_get_sps_bitstream (GrdNalWriter                           *nal_writer,
                                           const VAEncSequenceParameterBufferH264 *sequence_param,
                                           uint32_t                               *bitstream_length);

uint8_t *grd_nal_writer_get_pps_bitstream (GrdNalWriter                          *nal_writer,
                                           const VAEncPictureParameterBufferH264 *picture_param,
                                           uint32_t                              *bitstream_length);

uint8_t *grd_nal_writer_get_slice_header_bitstream (GrdNalWriter                           *nal_writer,
                                                    const VAEncSliceParameterBufferH264    *slice_param,
                                                    const VAEncSequenceParameterBufferH264 *sequence_param,
                                                    const VAEncPictureParameterBufferH264  *picture_param,
                                                    uint32_t                               *bitstream_length);

#endif /* GRD_NAL_WRITER_H */
