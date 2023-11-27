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

#include "config.h"

#include "grd-rdp-dsp.h"

#include <fdk-aac/aacenc_lib.h>
#include <gio/gio.h>

#define G711_QUANT_MASK 0xF
#define G711_SEG_MASK 0x70
#define G711_SEG_SHIFT 4

struct _GrdRdpDsp
{
  GObject parent;

  GrdRdpDspCreateFlag create_flags;

  HANDLE_AACENCODER aac_encoder;
};

G_DEFINE_TYPE (GrdRdpDsp, grd_rdp_dsp, G_TYPE_OBJECT)

const char *
grd_rdp_dsp_codec_to_string (GrdRdpDspCodec dsp_codec)
{
  switch (dsp_codec)
    {
    case GRD_RDP_DSP_CODEC_NONE:
      return "PCM";
    case GRD_RDP_DSP_CODEC_AAC:
      return "AAC";
    case GRD_RDP_DSP_CODEC_ALAW:
      return "A-law";
    }

  g_assert_not_reached ();
}

static gboolean
encode_aac (GrdRdpDsp  *rdp_dsp,
            int16_t    *input_data,
            int         input_size,
            int         input_elem_size,
            uint8_t   **output_data,
            uint32_t   *output_size)
{
  AACENC_BufferIdentifier ident_in;
  AACENC_BufferIdentifier ident_out;
  AACENC_BufDesc buffer_descriptor_in = {};
  AACENC_BufDesc buffer_descriptor_out = {};
  AACENC_InArgs args_in = {};
  AACENC_OutArgs args_out = {};
  int output_allocation_size;
  int output_elem_size;
  AACENC_ERROR aac_error;

  *output_size = 0;

  ident_in = IN_AUDIO_DATA;

  buffer_descriptor_in.numBufs = 1;
  buffer_descriptor_in.bufs = (void **) &input_data;
  buffer_descriptor_in.bufferIdentifiers = (int *) &ident_in;
  buffer_descriptor_in.bufSizes = &input_size;
  buffer_descriptor_in.bufElSizes = &input_elem_size;

  ident_out = OUT_BITSTREAM_DATA;
  output_elem_size = sizeof (uint8_t);

  output_allocation_size = input_size;
  *output_data = g_malloc0 (output_allocation_size);

  buffer_descriptor_out.numBufs = 1;
  buffer_descriptor_out.bufs = (void **) output_data;
  buffer_descriptor_out.bufferIdentifiers = (int *) &ident_out;
  buffer_descriptor_out.bufSizes = &output_allocation_size;
  buffer_descriptor_out.bufElSizes = &output_elem_size;

  args_in.numInSamples = input_size / input_elem_size;

  aac_error = aacEncEncode (rdp_dsp->aac_encoder, &buffer_descriptor_in,
                            &buffer_descriptor_out, &args_in, &args_out);
  if (aac_error != AACENC_OK)
    {
      g_warning ("[RDP.DSP] Failed to AAC encode samples");
      g_clear_pointer (output_data, g_free);
      return FALSE;
    }

  *output_size = args_out.numOutBytes;

  return TRUE;
}

gboolean
grd_rdp_dsp_encode (GrdRdpDsp       *rdp_dsp,
                    GrdRdpDspCodec   codec,
                    int16_t         *input_data,
                    uint32_t         input_size,
                    uint32_t         input_elem_size,
                    uint8_t        **output_data,
                    uint32_t        *output_size)
{
  g_assert (rdp_dsp->create_flags & GRD_RDP_DSP_CREATE_FLAG_ENCODER);

  switch (codec)
    {
    case GRD_RDP_DSP_CODEC_NONE:
      g_assert_not_reached ();
      return FALSE;
    case GRD_RDP_DSP_CODEC_AAC:
      return encode_aac (rdp_dsp, input_data, input_size, input_elem_size,
                         output_data, output_size);
    case GRD_RDP_DSP_CODEC_ALAW:
      g_assert_not_reached ();
      return FALSE;
    }

  g_assert_not_reached ();

  return FALSE;
}

static int16_t
alaw_to_s16 (uint8_t alaw_value)
{
  int16_t segment;
  int16_t temp;

  alaw_value ^= 0x55;

  temp = (alaw_value & G711_QUANT_MASK) << 4;
  temp += 8;

  segment = (alaw_value & G711_SEG_MASK) >> G711_SEG_SHIFT;

  if (segment > 0)
    temp += 0x100;
  if (segment > 1)
    temp <<= segment - 1;

  return alaw_value > 127 ? temp : -temp;
}

static gboolean
decode_alaw (GrdRdpDsp  *rdp_dsp,
             uint8_t    *input_data,
             uint32_t    input_size,
             int16_t   **output_data,
             uint32_t   *output_size)
{
  uint32_t i;

  g_assert (output_data);
  g_assert (output_size);

  g_assert (input_data);
  g_assert (input_size > 0);

  *output_size = input_size / sizeof (uint8_t) * sizeof (int16_t);
  *output_data = g_malloc0 (*output_size);

  for (i = 0; i < input_size; ++i)
    (*output_data)[i] = alaw_to_s16 (input_data[i]);

  return TRUE;
}

gboolean
grd_rdp_dsp_decode (GrdRdpDsp       *rdp_dsp,
                    GrdRdpDspCodec   codec,
                    uint8_t         *input_data,
                    uint32_t         input_size,
                    int16_t        **output_data,
                    uint32_t        *output_size)
{
  g_assert (rdp_dsp->create_flags & GRD_RDP_DSP_CREATE_FLAG_DECODER);

  switch (codec)
    {
    case GRD_RDP_DSP_CODEC_NONE:
    case GRD_RDP_DSP_CODEC_AAC:
      g_assert_not_reached ();
      return FALSE;
    case GRD_RDP_DSP_CODEC_ALAW:
      return decode_alaw (rdp_dsp, input_data, input_size,
                          output_data, output_size);
    }

  g_assert_not_reached ();

  return FALSE;
}

static gboolean
create_aac_encoder (GrdRdpDsp  *rdp_dsp,
                    uint32_t    n_samples_per_sec,
                    uint32_t    n_channels,
                    uint32_t    bitrate,
                    GError    **error)
{
  AACENC_InfoStruct info = {};
  AACENC_ERROR aac_error;

  /*
   * Assert n_channels == 2 due to (currently) hardcoded
   * AACENC_CHANNELMODE setting
   */
  g_assert (n_channels == 2);

  aac_error = aacEncOpen (&rdp_dsp->aac_encoder, 0, n_channels);
  if (aac_error != AACENC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create AAC encoder. AAC error %i", aac_error);
      return FALSE;
    }

  aac_error = aacEncoder_SetParam (rdp_dsp->aac_encoder, AACENC_AOT,
                                   AOT_AAC_LC);
  if (aac_error != AACENC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to set AAC encoder param AACENC_AOT. "
                   "AAC error %i", aac_error);
      return FALSE;
    }

  aac_error = aacEncoder_SetParam (rdp_dsp->aac_encoder, AACENC_BITRATE,
                                   bitrate);
  if (aac_error != AACENC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to set AAC encoder param AACENC_BITRATE. "
                   "AAC error %i", aac_error);
      return FALSE;
    }

  aac_error = aacEncoder_SetParam (rdp_dsp->aac_encoder, AACENC_SAMPLERATE,
                                   n_samples_per_sec);
  if (aac_error != AACENC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to set AAC encoder param AACENC_SAMPLERATE. "
                   "AAC error %i", aac_error);
      return FALSE;
    }

  aac_error = aacEncoder_SetParam (rdp_dsp->aac_encoder, AACENC_CHANNELMODE,
                                   MODE_2);
  if (aac_error != AACENC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to set AAC encoder param AACENC_CHANNELMODE. "
                   "AAC error %i", aac_error);
      return FALSE;
    }

  aac_error = aacEncoder_SetParam (rdp_dsp->aac_encoder, AACENC_CHANNELORDER,
                                   1);
  if (aac_error != AACENC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to set AAC encoder param AACENC_CHANNELORDER. "
                   "AAC error %i", aac_error);
      return FALSE;
    }

  aac_error = aacEncoder_SetParam (rdp_dsp->aac_encoder, AACENC_TRANSMUX,
                                   TT_MP4_RAW);
  if (aac_error != AACENC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to set AAC encoder param AACENC_TRANSMUX. "
                   "AAC error %i", aac_error);
      return FALSE;
    }

  aac_error = aacEncoder_SetParam (rdp_dsp->aac_encoder, AACENC_AFTERBURNER, 1);
  if (aac_error != AACENC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to set AAC encoder param AACENC_AFTERBURNER. "
                   "AAC error %i", aac_error);
      return FALSE;
    }

  aac_error = aacEncEncode (rdp_dsp->aac_encoder, NULL, NULL, NULL, NULL);
  if (aac_error != AACENC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to initialize AAC encoder. AAC error %i", aac_error);
      return FALSE;
    }

  aac_error = aacEncInfo (rdp_dsp->aac_encoder, &info);
  if (aac_error != AACENC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to acquire AAC encoder info. AAC error %i",
                   aac_error);
      return FALSE;
    }

  g_debug ("[RDP.DSP] AAC encoder: maxOutBufBytes: %u, maxAncBytes: %u, "
           "inBufFillLevel: %u, inputChannels: %u, frameLength: %u, "
           "nDelay: %u, nDelayCore: %u",
           info.maxOutBufBytes, info.maxAncBytes, info.inBufFillLevel,
           info.inputChannels, info.frameLength, info.nDelay, info.nDelayCore);

  return TRUE;
}

static gboolean
create_encoders (GrdRdpDsp                  *rdp_dsp,
                 const GrdRdpDspDescriptor  *dsp_descriptor,
                 GError                    **error)
{
  if (!create_aac_encoder (rdp_dsp,
                           dsp_descriptor->n_samples_per_sec,
                           dsp_descriptor->n_channels,
                           dsp_descriptor->bitrate_aac,
                           error))
    return FALSE;

  return TRUE;
}

GrdRdpDsp *
grd_rdp_dsp_new (const GrdRdpDspDescriptor  *dsp_descriptor,
                 GError                    **error)
{
  g_autoptr (GrdRdpDsp) rdp_dsp = NULL;

  rdp_dsp = g_object_new (GRD_TYPE_RDP_DSP, NULL);
  rdp_dsp->create_flags = dsp_descriptor->create_flags;

  if (dsp_descriptor->create_flags & GRD_RDP_DSP_CREATE_FLAG_ENCODER &&
      !create_encoders (rdp_dsp, dsp_descriptor, error))
    return NULL;

  return g_steal_pointer (&rdp_dsp);
}

static void
grd_rdp_dsp_dispose (GObject *object)
{
  GrdRdpDsp *rdp_dsp = GRD_RDP_DSP (object);

  aacEncClose (&rdp_dsp->aac_encoder);

  G_OBJECT_CLASS (grd_rdp_dsp_parent_class)->dispose (object);
}

static void
grd_rdp_dsp_init (GrdRdpDsp *rdp_dsp)
{
}

static void
grd_rdp_dsp_class_init (GrdRdpDspClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_dsp_dispose;
}
