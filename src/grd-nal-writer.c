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

#include "grd-nal-writer.h"

#define H264_PROFILE_HIGH 100

/* See also E.2.1 VUI parameters semantics (Rec. ITU-T H.264 (08/2021)) */
#define H264_Extended_SAR 255

#define H264_NAL_REF_IDC_ZERO 0
#define H264_NAL_REF_IDC_MEDIUM 2
#define H264_NAL_REF_IDC_HIGH 3

/*
 * See also Table 7-1 – NAL unit type codes, syntax element categories,
 * and NAL unit type classes (Rec. ITU-T H.264 (08/2021))
 */
#define H264_NAL_UNIT_TYPE_SLICE_NON_IDR 1
#define H264_NAL_UNIT_TYPE_SLICE_IDR 5
#define H264_NAL_UNIT_TYPE_SPS 7
#define H264_NAL_UNIT_TYPE_PPS 8
#define H264_NAL_UNIT_TYPE_AUD 9

/*
 * See also Table 7-6 – Name association to slice_type
 * (Rec. ITU-T H.264 (08/2021))
 */
#define H264_SLICE_TYPE_P 0
#define H264_SLICE_TYPE_B 1
#define H264_SLICE_TYPE_I 2

#define BITSTREAM_ALLOCATION_STEP 4096

typedef struct
{
  uint32_t *buffer;
  uint32_t bit_offset;
  uint32_t capacity;
} NalBitstream;

struct _GrdNalWriter
{
  GObject parent;

  NalBitstream *nal_bitstream;
};

G_DEFINE_TYPE (GrdNalWriter, grd_nal_writer, G_TYPE_OBJECT)

static void
start_bitstream (GrdNalWriter *nal_writer)
{
  g_assert (!nal_writer->nal_bitstream);

  nal_writer->nal_bitstream = g_new0 (NalBitstream, 1);
}

static uint32_t
swap_byte_order (uint32_t value)
{
  uint8_t *ptr = (uint8_t *) &value;

  return ptr[0] << 24 | ptr[1] << 16 | ptr[2] << 8 | ptr[3];
}

static uint8_t *
end_bitstream (GrdNalWriter *nal_writer,
               uint32_t     *bitstream_length)
{
  NalBitstream *nal_bitstream = nal_writer->nal_bitstream;
  uint32_t *buffer = nal_bitstream->buffer;
  uint32_t offset_in_dword;
  uint32_t byte_pos;
  uint32_t bits_left;

  byte_pos = nal_bitstream->bit_offset >> 5;
  offset_in_dword = nal_bitstream->bit_offset & 0x1F;
  bits_left = 32 - offset_in_dword;

  if (bits_left)
    buffer[byte_pos] = swap_byte_order (buffer[byte_pos] << bits_left);

  *bitstream_length = nal_bitstream->bit_offset;
  g_clear_pointer (&nal_writer->nal_bitstream, g_free);

  return (uint8_t *) buffer;
}

static void
ensure_remaining_capacity (NalBitstream *nal_bitstream,
                           uint32_t      required_capacity_in_bits)
{
  uint32_t new_capacity;

  if (required_capacity_in_bits <= nal_bitstream->capacity * 32)
    return;

  new_capacity = nal_bitstream->capacity + BITSTREAM_ALLOCATION_STEP;
  g_assert (required_capacity_in_bits <= new_capacity * 32);

  nal_bitstream->buffer = g_realloc (nal_bitstream->buffer,
                                     new_capacity * sizeof (uint32_t));
  nal_bitstream->capacity = new_capacity;
}

static void
write_u (GrdNalWriter *nal_writer,
         uint32_t      value,
         uint32_t      n_bits)
{
  NalBitstream *nal_bitstream = nal_writer->nal_bitstream;
  uint32_t offset_in_dword;
  uint32_t byte_pos;
  uint32_t bits_left;

  if (n_bits == 0)
    return;

  ensure_remaining_capacity (nal_bitstream, nal_bitstream->bit_offset + n_bits);

  byte_pos = nal_bitstream->bit_offset >> 5;
  offset_in_dword = nal_bitstream->bit_offset & 0x1F;
  bits_left = 32 - offset_in_dword;

  if (!offset_in_dword)
    nal_bitstream->buffer[byte_pos] = 0;

  nal_bitstream->bit_offset += n_bits;
  if (bits_left > n_bits)
    {
      nal_bitstream->buffer[byte_pos] <<= n_bits;
      nal_bitstream->buffer[byte_pos] |= value;
      return;
    }

  n_bits -= bits_left;
  nal_bitstream->buffer[byte_pos] <<= bits_left;
  nal_bitstream->buffer[byte_pos] |= value >> n_bits;
  nal_bitstream->buffer[byte_pos] =
    swap_byte_order (nal_bitstream->buffer[byte_pos]);

  nal_bitstream->buffer[++byte_pos] = value;
}

/* Exponential Golomb coding (unsigned) */
static void
write_ue (GrdNalWriter *nal_writer,
          uint32_t      value)
{
  uint32_t value_to_write;
  uint32_t n_bits = 0;
  uint32_t tmp;

  /*
   * Write down value + 1, but before that write down n - 1 zeros,
   * where n represents the bits to be written for value + 1
   */
  value_to_write = value + 1;
  tmp = value_to_write;
  while (tmp)
    {
      ++n_bits;
      tmp >>= 1;
    }

  if (n_bits > 1)
    write_u (nal_writer, 0, n_bits - 1);
  write_u (nal_writer, value_to_write, n_bits);
}

/* Exponential Golomb coding (signed) */
static void
write_se (GrdNalWriter *nal_writer,
          int32_t       value)
{
  /*
   * If the value is <= 0, map the value to -2 * value (even integer value),
   * otherwise map it to 2 * value - 1 (odd integer value)
   */
  if (value <= 0)
    write_ue (nal_writer, -value << 1);
  else
    write_ue (nal_writer, (value << 1) - 1);
}

static void
write_nal_start_code_prefix (GrdNalWriter *nal_writer)
{
  write_u (nal_writer, 0x00000001, 32);
}

static void
write_nal_header (GrdNalWriter *nal_writer,
                  uint8_t       nal_ref_idc,
                  uint8_t       nal_unit_type)
{
  /* See also 7.3.1 NAL unit syntax (Rec. ITU-T H.264 (08/2021)) */
  /* forbidden_zero_bit */
  write_u (nal_writer, 0, 1);
  /* nal_ref_idc */
  write_u (nal_writer, nal_ref_idc, 2);
  /* nal_unit_type */
  write_u (nal_writer, nal_unit_type, 5);
}

static void
byte_align_bitstream (GrdNalWriter *nal_writer)
{
  NalBitstream *nal_bitstream = nal_writer->nal_bitstream;
  uint32_t offset_in_byte;
  uint32_t bits_left;

  offset_in_byte = nal_bitstream->bit_offset & 0x7;
  bits_left = 8 - offset_in_byte;

  if (!bits_left)
    return;

  /* rbsp_alignment_zero_bit */
  write_u (nal_writer, 0, bits_left);
}

static void
write_trailing_bits (GrdNalWriter *nal_writer)
{
  /* rbsp_stop_one_bit */
  write_u (nal_writer, 1, 1);
  byte_align_bitstream (nal_writer);
}

static void
write_access_unit_delimiter (GrdNalWriter *nal_writer)
{
  uint32_t primary_pic_type;

  primary_pic_type = 1;

  write_u (nal_writer, primary_pic_type, 3);
}

uint8_t *
grd_nal_writer_get_aud_bitstream (GrdNalWriter *nal_writer,
                                  uint32_t     *bitstream_length)
{
  uint8_t *bitstream;

  start_bitstream (nal_writer);
  write_nal_start_code_prefix (nal_writer);
  write_nal_header (nal_writer, H264_NAL_REF_IDC_ZERO, H264_NAL_UNIT_TYPE_AUD);
  write_access_unit_delimiter (nal_writer);
  write_trailing_bits (nal_writer);

  bitstream = end_bitstream (nal_writer, bitstream_length);
  g_assert (*bitstream_length % 8 == 0);

  return bitstream;
}

static void
write_vui_parameters (GrdNalWriter                           *nal_writer,
                      const VAEncSequenceParameterBufferH264 *sequence_param)
{
  uint32_t aspect_ratio_info_present_flag;
  uint32_t overscan_info_present_flag;
  uint32_t video_signal_type_present_flag;
  uint32_t chroma_loc_info_present_flag;
  uint32_t timing_info_present_flag;
  uint32_t nal_hrd_parameters_present_flag;
  uint32_t vcl_hrd_parameters_present_flag;
  uint32_t pic_struct_present_flag;
  uint32_t bitstream_restriction_flag;

  g_assert (sequence_param->vui_parameters_present_flag);

  aspect_ratio_info_present_flag =
    sequence_param->vui_fields.bits.aspect_ratio_info_present_flag;
  timing_info_present_flag =
    sequence_param->vui_fields.bits.timing_info_present_flag;
  bitstream_restriction_flag =
    sequence_param->vui_fields.bits.bitstream_restriction_flag;

  overscan_info_present_flag = 0;
  video_signal_type_present_flag = 0;
  chroma_loc_info_present_flag = 0;
  nal_hrd_parameters_present_flag = 0;
  vcl_hrd_parameters_present_flag = 0;
  pic_struct_present_flag = 0;

  /*
   * See also E.1.1 VUI parameters syntax (Rec. ITU-T H.264 (08/2021))
   *
   * Not all paths are covered, only the ones relevant for GNOME Remote Desktop.
   * Unhandled branches are preceded with an assertion.
   */

  /* aspect_ratio_info_present_flag */
  write_u (nal_writer, aspect_ratio_info_present_flag, 1);
  if (aspect_ratio_info_present_flag)
    {
      /* aspect_ratio_idc */
      write_u (nal_writer, sequence_param->aspect_ratio_idc, 8);
      if (sequence_param->aspect_ratio_idc == H264_Extended_SAR)
        {
          /* sar_width */
          write_u (nal_writer, sequence_param->sar_width, 16);
          /* sar_height */
          write_u (nal_writer, sequence_param->sar_height, 16);
        }
    }
  /* overscan_info_present_flag */
  write_u (nal_writer, overscan_info_present_flag, 1);
  g_assert (!overscan_info_present_flag);

  /* video_signal_type_present_flag */
  write_u (nal_writer, video_signal_type_present_flag, 1);
  g_assert (!video_signal_type_present_flag);

  /* chroma_loc_info_present_flag */
  write_u (nal_writer, chroma_loc_info_present_flag, 1);
  g_assert (!chroma_loc_info_present_flag);

  /* timing_info_present_flag */
  write_u (nal_writer, timing_info_present_flag, 1);
  if (timing_info_present_flag)
    {
      uint32_t fixed_frame_rate_flag =
        sequence_param->vui_fields.bits.fixed_frame_rate_flag;

      /* num_units_in_tick */
      write_u (nal_writer, sequence_param->num_units_in_tick, 32);
      /* time_scale */
      write_u (nal_writer, sequence_param->time_scale, 32);
      /* fixed_frame_rate_flag */
      write_u (nal_writer, fixed_frame_rate_flag, 1);
    }

  /* nal_hrd_parameters_present_flag */
  write_u (nal_writer, nal_hrd_parameters_present_flag, 1);
  g_assert (!nal_hrd_parameters_present_flag);

  /* vcl_hrd_parameters_present_flag */
  write_u (nal_writer, vcl_hrd_parameters_present_flag, 1);
  g_assert (!vcl_hrd_parameters_present_flag);

  g_assert (!nal_hrd_parameters_present_flag &&
            !vcl_hrd_parameters_present_flag);

  /* pic_struct_present_flag */
  write_u (nal_writer, pic_struct_present_flag, 1);
  /* bitstream_restriction_flag */
  write_u (nal_writer, bitstream_restriction_flag, 1);
  if (bitstream_restriction_flag)
    {
      uint32_t motion_vectors_over_pic_boundaries_flag;
      uint32_t max_bytes_per_pic_denom;
      uint32_t max_bits_per_mb_denom;
      uint32_t log2_max_mv_length_horizontal;
      uint32_t log2_max_mv_length_vertical;
      uint32_t max_num_reorder_frames;
      uint32_t max_dec_frame_buffering;

      log2_max_mv_length_horizontal =
        sequence_param->vui_fields.bits.log2_max_mv_length_horizontal;
      log2_max_mv_length_vertical =
        sequence_param->vui_fields.bits.log2_max_mv_length_vertical;

      motion_vectors_over_pic_boundaries_flag = 1;
      max_bytes_per_pic_denom = 0;
      max_bits_per_mb_denom = 0;
      max_num_reorder_frames = 0;
      max_dec_frame_buffering = 1;

      /* motion_vectors_over_pic_boundaries_flag */
      write_u (nal_writer, motion_vectors_over_pic_boundaries_flag, 1);
      /* max_bytes_per_pic_denom */
      write_ue (nal_writer, max_bytes_per_pic_denom);
      /* max_bits_per_mb_denom */
      write_ue (nal_writer, max_bits_per_mb_denom);
      /* log2_max_mv_length_horizontal */
      write_ue (nal_writer, log2_max_mv_length_horizontal);
      /* log2_max_mv_length_vertical */
      write_ue (nal_writer, log2_max_mv_length_vertical);
      /* max_num_reorder_frames */
      write_ue (nal_writer, max_num_reorder_frames);
      /* max_dec_frame_buffering */
      write_ue (nal_writer, max_dec_frame_buffering);
    }
}

static void
write_sps_data (GrdNalWriter                           *nal_writer,
                const VAEncSequenceParameterBufferH264 *sequence_param)
{
  uint32_t profile_idc;
  uint32_t constraint_set0_flag;
  uint32_t constraint_set1_flag;
  uint32_t constraint_set2_flag;
  uint32_t constraint_set3_flag;
  uint32_t constraint_set4_flag;
  uint32_t constraint_set5_flag;
  uint32_t chroma_format_idc;
  uint32_t qpprime_y_zero_transform_bypass_flag;
  uint32_t seq_scaling_matrix_present_flag;
  uint32_t log2_max_frame_num_minus4;
  uint32_t pic_order_cnt_type;
  uint32_t gaps_in_frame_num_value_allowed_flag;
  uint32_t pic_height_in_map_units;
  uint32_t frame_mbs_only_flag;
  uint32_t direct_8x8_inference_flag;

  g_assert (sequence_param->picture_width_in_mbs > 0);

  frame_mbs_only_flag = sequence_param->seq_fields.bits.frame_mbs_only_flag;

  /*
   * See also 7.4.2.1.1 Sequence parameter set data semantics
   * (Rec. ITU-T H.264 (08/2021))
   */
  profile_idc = H264_PROFILE_HIGH;
  constraint_set0_flag = 0;
  constraint_set1_flag = 0;
  constraint_set2_flag = 0;
  constraint_set3_flag = 0;

  g_assert (profile_idc == H264_PROFILE_HIGH);
  g_assert (frame_mbs_only_flag == 1);
  /* frame_mbs_only_flag is equal to 1 */
  constraint_set4_flag = 1;

  g_assert (profile_idc == H264_PROFILE_HIGH);
  /* No B-slices are present in the coded video sequence */
  constraint_set5_flag = 1;

  chroma_format_idc = sequence_param->seq_fields.bits.chroma_format_idc;
  seq_scaling_matrix_present_flag =
    sequence_param->seq_fields.bits.seq_scaling_matrix_present_flag;
  log2_max_frame_num_minus4 =
    sequence_param->seq_fields.bits.log2_max_frame_num_minus4;
  pic_order_cnt_type = sequence_param->seq_fields.bits.pic_order_cnt_type;
  direct_8x8_inference_flag =
    sequence_param->seq_fields.bits.direct_8x8_inference_flag;

  qpprime_y_zero_transform_bypass_flag = 0;
  gaps_in_frame_num_value_allowed_flag = 0;

  g_assert (frame_mbs_only_flag);
  pic_height_in_map_units = sequence_param->picture_height_in_mbs;
  g_assert (pic_height_in_map_units > 0);

  /*
   * See also 7.3.2.1.1 Sequence parameter set data syntax
   * (Rec. ITU-T H.264 (08/2021))
   *
   * Not all paths are covered, only the ones relevant for GNOME Remote Desktop.
   * Unhandled branches are preceded with an assertion.
   */
  /* profile_idc */
  write_u (nal_writer, profile_idc, 8);
  /* constraint_set0_flag */
  write_u (nal_writer, constraint_set0_flag, 1);
  /* constraint_set1_flag */
  write_u (nal_writer, constraint_set1_flag, 1);
  /* constraint_set2_flag */
  write_u (nal_writer, constraint_set2_flag, 1);
  /* constraint_set3_flag */
  write_u (nal_writer, constraint_set3_flag, 1);
  /* constraint_set4_flag */
  write_u (nal_writer, constraint_set4_flag, 1);
  /* constraint_set5_flag */
  write_u (nal_writer, constraint_set5_flag, 1);
  /* reserved_zero_2bits */
  write_u (nal_writer, 0, 2);
  /* level_idc */
  write_u (nal_writer, sequence_param->level_idc, 8);
  /* seq_parameter_set_id */
  write_ue (nal_writer, sequence_param->seq_parameter_set_id);

  g_assert (profile_idc == H264_PROFILE_HIGH);
  /* chroma_format_idc */
  write_ue (nal_writer, chroma_format_idc);
  g_assert (chroma_format_idc != 3);

  /* bit_depth_luma_minus8 */
  write_ue (nal_writer, sequence_param->bit_depth_luma_minus8);
  /* bit_depth_chroma_minus8 */
  write_ue (nal_writer, sequence_param->bit_depth_chroma_minus8);
  /* qpprime_y_zero_transform_bypass_flag */
  write_u (nal_writer, qpprime_y_zero_transform_bypass_flag, 1);
  /* seq_scaling_matrix_present_flag */
  write_u (nal_writer, seq_scaling_matrix_present_flag, 1);
  g_assert (!seq_scaling_matrix_present_flag);

  /* log2_max_frame_num_minus4 */
  write_ue (nal_writer, log2_max_frame_num_minus4);
  /* pic_order_cnt_type */
  write_ue (nal_writer, pic_order_cnt_type);
  if (pic_order_cnt_type == 0)
    g_assert_not_reached ();
  else if (pic_order_cnt_type == 1)
    g_assert_not_reached ();

  /* max_num_ref_frames */
  write_ue (nal_writer, sequence_param->max_num_ref_frames);
  /* gaps_in_frame_num_value_allowed_flag */
  write_u (nal_writer, gaps_in_frame_num_value_allowed_flag, 1);
  /* pic_width_in_mbs_minus1 */
  write_ue (nal_writer, sequence_param->picture_width_in_mbs - 1);
  /* pic_height_in_map_units_minus1 */
  write_ue (nal_writer, pic_height_in_map_units - 1);
  /* frame_mbs_only_flag */
  write_u (nal_writer, frame_mbs_only_flag, 1);
  g_assert (frame_mbs_only_flag);

  /* direct_8x8_inference_flag */
  write_u (nal_writer, direct_8x8_inference_flag, 1);
  /* frame_cropping_flag */
  write_u (nal_writer, sequence_param->frame_cropping_flag, 1);
  g_assert (!sequence_param->frame_cropping_flag);

  /* vui_parameters_present_flag */
  write_u (nal_writer, sequence_param->vui_parameters_present_flag, 1);
  if (sequence_param->vui_parameters_present_flag)
    write_vui_parameters (nal_writer, sequence_param);
}

uint8_t *
grd_nal_writer_get_sps_bitstream (GrdNalWriter                           *nal_writer,
                                  const VAEncSequenceParameterBufferH264 *sequence_param,
                                  uint32_t                               *bitstream_length)
{
  uint8_t *bitstream;

  start_bitstream (nal_writer);
  write_nal_start_code_prefix (nal_writer);
  write_nal_header (nal_writer, H264_NAL_REF_IDC_HIGH, H264_NAL_UNIT_TYPE_SPS);
  write_sps_data (nal_writer, sequence_param);
  write_trailing_bits (nal_writer);

  bitstream = end_bitstream (nal_writer, bitstream_length);
  g_assert (*bitstream_length % 8 == 0);

  return bitstream;
}

static void
write_pps_data (GrdNalWriter                          *nal_writer,
                const VAEncPictureParameterBufferH264 *picture_param)
{
  uint32_t entropy_coding_mode_flag;
  uint32_t bottom_field_pic_order_in_frame_present_flag;
  uint32_t num_slice_groups_minus1;
  uint32_t weighted_pred_flag;
  uint32_t weighted_bipred_idc;
  uint32_t pic_init_qs_minus26;
  uint32_t deblocking_filter_control_present_flag;
  uint32_t constrained_intra_pred_flag;
  uint32_t redundant_pic_cnt_present_flag;
  uint32_t transform_8x8_mode_flag;
  uint32_t pic_scaling_matrix_present_flag;

  entropy_coding_mode_flag =
    picture_param->pic_fields.bits.entropy_coding_mode_flag;
  bottom_field_pic_order_in_frame_present_flag =
    picture_param->pic_fields.bits.pic_order_present_flag;
  weighted_pred_flag = picture_param->pic_fields.bits.weighted_pred_flag;
  weighted_bipred_idc = picture_param->pic_fields.bits.weighted_bipred_idc;
  deblocking_filter_control_present_flag =
    picture_param->pic_fields.bits.deblocking_filter_control_present_flag;
  constrained_intra_pred_flag =
    picture_param->pic_fields.bits.constrained_intra_pred_flag;
  redundant_pic_cnt_present_flag =
    picture_param->pic_fields.bits.redundant_pic_cnt_present_flag;
  transform_8x8_mode_flag =
    picture_param->pic_fields.bits.transform_8x8_mode_flag;
  pic_scaling_matrix_present_flag =
    picture_param->pic_fields.bits.pic_scaling_matrix_present_flag;

  num_slice_groups_minus1 = 0;
  pic_init_qs_minus26 = 0;

  /*
   * See also 7.3.2.2 Picture parameter set RBSP syntax
   * (Rec. ITU-T H.264 (08/2021))
   *
   * Not all paths are covered, only the ones relevant for GNOME Remote Desktop.
   * Unhandled branches are preceded with an assertion.
   */
  /* pic_parameter_set_id */
  write_ue (nal_writer, picture_param->pic_parameter_set_id);
  /* seq_parameter_set_id */
  write_ue (nal_writer, picture_param->seq_parameter_set_id);
  /* entropy_coding_mode_flag */
  write_u (nal_writer, entropy_coding_mode_flag, 1);
  /* bottom_field_pic_order_in_frame_present_flag */
  write_u (nal_writer, bottom_field_pic_order_in_frame_present_flag, 1);
  /* num_slice_groups_minus1 */
  write_ue (nal_writer, num_slice_groups_minus1);
  g_assert (num_slice_groups_minus1 == 0);

  /* num_ref_idx_l0_default_active_minus1 */
  write_ue (nal_writer, picture_param->num_ref_idx_l0_active_minus1);
  /* num_ref_idx_l1_default_active_minus1 */
  write_ue (nal_writer, picture_param->num_ref_idx_l1_active_minus1);
  /* weighted_pred_flag */
  write_u (nal_writer, weighted_pred_flag, 1);
  /* weighted_bipred_idc */
  write_u (nal_writer, weighted_bipred_idc, 2);
  /* pic_init_qp_minus26 */
  write_se (nal_writer, picture_param->pic_init_qp - 26);
  /* pic_init_qs_minus26 */
  write_se (nal_writer, pic_init_qs_minus26);
  /* chroma_qp_index_offset */
  write_se (nal_writer, picture_param->chroma_qp_index_offset);
  /* deblocking_filter_control_present_flag */
  write_u (nal_writer, deblocking_filter_control_present_flag, 1);
  /* constrained_intra_pred_flag */
  write_u (nal_writer, constrained_intra_pred_flag, 1);
  /* redundant_pic_cnt_present_flag */
  write_u (nal_writer, redundant_pic_cnt_present_flag, 1);

  /* more_rbsp_data */
  /* transform_8x8_mode_flag */
  write_u (nal_writer, transform_8x8_mode_flag, 1);
  /* pic_scaling_matrix_present_flag */
  write_u (nal_writer, pic_scaling_matrix_present_flag, 1);
  g_assert (!pic_scaling_matrix_present_flag);

  /* second_chroma_qp_index_offset */
  write_se (nal_writer, picture_param->second_chroma_qp_index_offset);
}

uint8_t *
grd_nal_writer_get_pps_bitstream (GrdNalWriter                          *nal_writer,
                                  const VAEncPictureParameterBufferH264 *picture_param,
                                  uint32_t                              *bitstream_length)
{
  uint8_t *bitstream;

  start_bitstream (nal_writer);
  write_nal_start_code_prefix (nal_writer);
  write_nal_header (nal_writer, H264_NAL_REF_IDC_HIGH, H264_NAL_UNIT_TYPE_PPS);
  write_pps_data (nal_writer, picture_param);
  write_trailing_bits (nal_writer);

  bitstream = end_bitstream (nal_writer, bitstream_length);
  g_assert (*bitstream_length % 8 == 0);

  return bitstream;
}

static void
write_ref_pic_list_modification (GrdNalWriter                        *nal_writer,
                                 const VAEncSliceParameterBufferH264 *slice_param)
{
  if (slice_param->slice_type != H264_SLICE_TYPE_I)
    {
      uint32_t ref_pic_list_modification_flag_l0;

      ref_pic_list_modification_flag_l0 = 0;

      /* ref_pic_list_modification_flag_l0 */
      write_u (nal_writer, ref_pic_list_modification_flag_l0, 1);
      g_assert (!ref_pic_list_modification_flag_l0);
    }
  g_assert (slice_param->slice_type != H264_SLICE_TYPE_B);
}

static void
write_dec_ref_pic_marking (GrdNalWriter                          *nal_writer,
                           const VAEncPictureParameterBufferH264 *picture_param)
{
  if (picture_param->pic_fields.bits.idr_pic_flag)
    {
      uint32_t no_output_of_prior_pics_flag;
      uint32_t long_term_reference_flag;

      no_output_of_prior_pics_flag = 0;
      long_term_reference_flag = 0;

      /* no_output_of_prior_pics_flag */
      write_u (nal_writer, no_output_of_prior_pics_flag, 1);
      /* long_term_reference_flag */
      write_u (nal_writer, long_term_reference_flag, 1);
    }
  else
    {
      uint32_t adaptive_ref_pic_marking_mode_flag;

      adaptive_ref_pic_marking_mode_flag = 0;

      /* adaptive_ref_pic_marking_mode_flag */
      write_u (nal_writer, adaptive_ref_pic_marking_mode_flag, 1);
      g_assert (!adaptive_ref_pic_marking_mode_flag);
    }
}

static void
write_slice_header (GrdNalWriter                           *nal_writer,
                    const VAEncSliceParameterBufferH264    *slice_param,
                    const VAEncSequenceParameterBufferH264 *sequence_param,
                    const VAEncPictureParameterBufferH264  *picture_param,
                    const uint8_t                           nal_ref_idc)
{
  uint32_t separate_colour_plane_flag;
  uint32_t log2_max_frame_num;
  uint16_t frame_num;
  uint32_t frame_mbs_only_flag;
  uint32_t pic_order_cnt_type;
  uint32_t redundant_pic_cnt_present_flag;
  uint32_t weighted_pred_flag;
  uint32_t weighted_bipred_idc;
  uint32_t entropy_coding_mode_flag;
  uint32_t deblocking_filter_control_present_flag;
  uint32_t num_slice_groups_minus1;

  frame_num = picture_param->frame_num;
  log2_max_frame_num =
    sequence_param->seq_fields.bits.log2_max_frame_num_minus4 + 4;
  frame_mbs_only_flag = sequence_param->seq_fields.bits.frame_mbs_only_flag;
  pic_order_cnt_type = sequence_param->seq_fields.bits.pic_order_cnt_type;
  redundant_pic_cnt_present_flag =
    picture_param->pic_fields.bits.redundant_pic_cnt_present_flag;
  weighted_pred_flag = picture_param->pic_fields.bits.weighted_pred_flag;
  weighted_bipred_idc = picture_param->pic_fields.bits.weighted_bipred_idc;
  entropy_coding_mode_flag =
    picture_param->pic_fields.bits.entropy_coding_mode_flag;
  deblocking_filter_control_present_flag =
    picture_param->pic_fields.bits.deblocking_filter_control_present_flag;

  separate_colour_plane_flag = 0;
  num_slice_groups_minus1 = 0;

  /*
   * See also 7.3.3 Slice header syntax (Rec. ITU-T H.264 (08/2021))
   *
   * Not all paths are covered, only the ones relevant for GNOME Remote Desktop.
   * Unhandled branches are preceded with an assertion.
   */
  /* first_mb_in_slice */
  write_ue (nal_writer, slice_param->macroblock_address);
  /* slice_type */
  write_ue (nal_writer, slice_param->slice_type);
  /* pic_parameter_set_id */
  write_ue (nal_writer, slice_param->pic_parameter_set_id);
  g_assert (!separate_colour_plane_flag);

  /* frame_num */
  write_u (nal_writer, frame_num, log2_max_frame_num);
  g_assert (frame_mbs_only_flag);

  if (picture_param->pic_fields.bits.idr_pic_flag)
    {
      /* idr_pic_id */
      write_ue (nal_writer, slice_param->idr_pic_id);
    }
  if (pic_order_cnt_type == 0)
    g_assert_not_reached ();
  if (pic_order_cnt_type == 1)
    g_assert_not_reached ();

  g_assert (!redundant_pic_cnt_present_flag);
  g_assert (slice_param->slice_type != H264_SLICE_TYPE_B);

  if (slice_param->slice_type == H264_SLICE_TYPE_P)
    {
      /* num_ref_idx_active_override_flag */
      write_u (nal_writer, slice_param->num_ref_idx_active_override_flag, 1);
      g_assert (!slice_param->num_ref_idx_active_override_flag);
    }

  write_ref_pic_list_modification (nal_writer, slice_param);

  g_assert (!weighted_pred_flag && !weighted_bipred_idc);

  if (nal_ref_idc)
    write_dec_ref_pic_marking (nal_writer, picture_param);

  if (entropy_coding_mode_flag && slice_param->slice_type != H264_SLICE_TYPE_I)
    {
      /* cabac_init_idc */
      write_ue (nal_writer, slice_param->cabac_init_idc);
    }

  /* slice_qp_delta */
  write_se (nal_writer, slice_param->slice_qp_delta);

  g_assert (slice_param->slice_type == H264_SLICE_TYPE_I ||
            slice_param->slice_type == H264_SLICE_TYPE_P);

  g_assert (!deblocking_filter_control_present_flag);
  g_assert (num_slice_groups_minus1 == 0);
}

static void
get_nal_header_parameters (const VAEncSliceParameterBufferH264   *slice_param,
                           const VAEncPictureParameterBufferH264 *picture_param,
                           uint8_t                               *nal_ref_idc,
                           uint8_t                               *nal_unit_type)
{
  switch (slice_param->slice_type)
    {
    case H264_SLICE_TYPE_I:
      *nal_ref_idc = H264_NAL_REF_IDC_HIGH;
      if (picture_param->pic_fields.bits.idr_pic_flag)
        *nal_unit_type = H264_NAL_UNIT_TYPE_SLICE_IDR;
      else
        *nal_unit_type = H264_NAL_UNIT_TYPE_SLICE_NON_IDR;
      break;
    case H264_SLICE_TYPE_P:
      *nal_ref_idc = H264_NAL_REF_IDC_MEDIUM;
      *nal_unit_type = H264_NAL_UNIT_TYPE_SLICE_NON_IDR;
      break;
    default:
      g_assert_not_reached ();
    }
}

uint8_t *
grd_nal_writer_get_slice_header_bitstream (GrdNalWriter                           *nal_writer,
                                           const VAEncSliceParameterBufferH264    *slice_param,
                                           const VAEncSequenceParameterBufferH264 *sequence_param,
                                           const VAEncPictureParameterBufferH264  *picture_param,
                                           uint32_t                               *bitstream_length)
{
  uint8_t nal_ref_idc;
  uint8_t nal_unit_type;

  get_nal_header_parameters (slice_param, picture_param,
                             &nal_ref_idc, &nal_unit_type);

  start_bitstream (nal_writer);
  write_nal_start_code_prefix (nal_writer);
  write_nal_header (nal_writer, nal_ref_idc, nal_unit_type);
  write_slice_header (nal_writer, slice_param, sequence_param, picture_param,
                      nal_ref_idc);

  return end_bitstream (nal_writer, bitstream_length);
}

GrdNalWriter *
grd_nal_writer_new (void)
{
  return g_object_new (GRD_TYPE_NAL_WRITER, NULL);
}

static void
grd_nal_writer_dispose (GObject *object)
{
  GrdNalWriter *nal_writer = GRD_NAL_WRITER (object);

  g_assert (!nal_writer->nal_bitstream);

  G_OBJECT_CLASS (grd_nal_writer_parent_class)->dispose (object);
}

static void
grd_nal_writer_init (GrdNalWriter *nal_writer)
{
}

static void
grd_nal_writer_class_init (GrdNalWriterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_nal_writer_dispose;
}
