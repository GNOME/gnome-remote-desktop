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

#ifndef GRD_RDP_DSP_H
#define GRD_RDP_DSP_H

#include <glib-object.h>
#include <stdint.h>

#define GRD_TYPE_RDP_DSP (grd_rdp_dsp_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpDsp, grd_rdp_dsp,
                      GRD, RDP_DSP, GObject)

typedef enum _GrdRdpDspCodec
{
  GRD_RDP_DSP_CODEC_NONE,
  GRD_RDP_DSP_CODEC_AAC,
} GrdRdpDspCodec;

typedef enum
{
  GRD_RDP_DSP_CREATE_FLAG_NONE = 0,
  GRD_RDP_DSP_CREATE_FLAG_ENCODER = 1 << 0,
} GrdRdpDspCreateFlag;

typedef struct
{
  GrdRdpDspCreateFlag create_flags;

  /* Encoder */
  uint32_t n_samples_per_sec;
  uint32_t n_channels;
  uint32_t bitrate_aac;
} GrdRdpDspDescriptor;

GrdRdpDsp *grd_rdp_dsp_new (const GrdRdpDspDescriptor  *dsp_descriptor,
                            GError                    **error);

gboolean grd_rdp_dsp_encode (GrdRdpDsp       *rdp_dsp,
                             GrdRdpDspCodec   codec,
                             int16_t         *input_data,
                             uint32_t         input_size,
                             uint32_t         input_elem_size,
                             uint8_t        **output_data,
                             uint32_t        *output_size);

#endif /* GRD_RDP_DSP_H */
