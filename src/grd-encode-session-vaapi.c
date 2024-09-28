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

#include "config.h"

#include "grd-encode-session-vaapi.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <va/va_drmcommon.h>

#include "grd-avc-frame-info.h"
#include "grd-bitstream.h"
#include "grd-debug.h"
#include "grd-image-view-nv12.h"
#include "grd-nal-writer.h"
#include "grd-utils.h"
#include "grd-vk-image.h"
#include "grd-vk-utils.h"

/*
 * See also Table 7-6 – Name association to slice_type
 * (Rec. ITU-T H.264 (08/2021))
 */
#define H264_SLICE_TYPE_P 0
#define H264_SLICE_TYPE_I 2

/* See also E.2.1 VUI parameters semantics (Rec. ITU-T H.264 (08/2021)) */
#define H264_Extended_SAR 255

#define LOG2_MAX_FRAME_NUM 8

/*
 * One surface is needed, when encoding a frame,
 * one is needed, when preparing the view,
 * one is needed for the submitted pending frame in the renderer,
 * and one is needed to prepare a new pending frame before it is submitted to
 * the renderer, where it then replaces the old pending frame
 *
 * In total, this makes four needed source surfaces per view
 */
#define N_SRC_SURFACES_PER_VIEW 4
#define N_SRC_SURFACES (N_SRC_SURFACES_PER_VIEW * 2)

typedef struct
{
  GrdEncodeSessionVaapi *encode_session_vaapi;

  VASurfaceID va_surface;

  int32_t frame_num;
  gboolean is_idr;
} VAAPIPicture;

typedef struct
{
  GrdEncodeSessionVaapi *encode_session_vaapi;

  VAAPIPicture *reconstructed_picture;

  VABufferID sequence_buffer;
  VABufferID picture_buffer;
  VABufferID slice_buffer;

  GList *packed_header_buffers;
  GList *misc_buffers;

  uint32_t aud_header_length;
  uint32_t sequence_header_length;
  uint32_t picture_header_length;
  uint32_t slice_header_length;

  VASurfaceID src_surface;
  VABufferID bitstream_buffer;

  GrdAVCFrameInfo *avc_frame_info;

  int64_t timestamps[3];
} H264Frame;

typedef struct
{
  GrdEncodeSessionVaapi *encode_session_vaapi;

  VASurfaceID va_surface;
  GrdImageView *image_view;

  int fds[2];
} NV12VkVASurface;

struct _GrdEncodeSessionVaapi
{
  GrdEncodeSession parent;

  GrdVkDevice *vk_device;
  VADisplay va_display;

  gboolean supports_quality_level;
  gboolean debug_va_times;

  VAConfigID va_config;
  VAContextID va_context;

  uint32_t surface_width;
  uint32_t surface_height;
  uint32_t refresh_rate;
  uint8_t level_idc;

  gboolean pending_idr_frame;

  GrdNalWriter *nal_writer;
  GHashTable *surfaces;

  VAAPIPicture *reference_picture;
  uint16_t frame_num;

  VAEncPictureParameterBufferH264 picture_param;
  VAEncSequenceParameterBufferH264 sequence_param;

  GMutex pending_frames_mutex;
  GHashTable *pending_frames;

  GMutex bitstreams_mutex;
  GHashTable *bitstreams;
};

G_DEFINE_TYPE (GrdEncodeSessionVaapi, grd_encode_session_vaapi,
               GRD_TYPE_ENCODE_SESSION)

static void
vaapi_picture_free (VAAPIPicture *picture);

static void
h264_frame_free (H264Frame *h264_frame);

static void
nv12_vkva_surface_free (NV12VkVASurface *vkva_surface);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (VAAPIPicture, vaapi_picture_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (H264Frame, h264_frame_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NV12VkVASurface, nv12_vkva_surface_free)

static VAAPIPicture *
vaapi_picture_new (GrdEncodeSessionVaapi  *encode_session_vaapi,
                   uint32_t                surface_width,
                   uint32_t                surface_height,
                   int32_t                 frame_num,
                   gboolean                is_idr,
                   GError                **error)
{
  g_autoptr (VAAPIPicture) picture = NULL;
  VAStatus va_status;

  picture = g_new0 (VAAPIPicture, 1);
  picture->encode_session_vaapi = encode_session_vaapi;
  picture->va_surface = VA_INVALID_SURFACE;
  picture->frame_num = frame_num;
  picture->is_idr = is_idr;

  va_status = vaCreateSurfaces (encode_session_vaapi->va_display,
                                VA_RT_FORMAT_YUV420,
                                surface_width, surface_height,
                                &picture->va_surface, 1,
                                NULL, 0);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create surface for reconstructed picture: %s",
                   vaErrorStr (va_status));
      return NULL;
    }
  g_assert (picture->va_surface != VA_INVALID_SURFACE);

  return g_steal_pointer (&picture);
}

static void
vaapi_picture_free (VAAPIPicture *picture)
{
  GrdEncodeSessionVaapi *encode_session_vaapi = picture->encode_session_vaapi;
  VADisplay va_display = encode_session_vaapi->va_display;

  if (picture->va_surface != VA_INVALID_SURFACE)
    vaDestroySurfaces (va_display, &picture->va_surface, 1);

  g_free (picture);
}

static H264Frame *
h264_frame_new (GrdEncodeSessionVaapi  *encode_session_vaapi,
                GError                **error)
{
  g_autoptr (H264Frame) h264_frame = NULL;
  uint32_t surface_width = encode_session_vaapi->surface_width;
  uint32_t surface_height = encode_session_vaapi->surface_height;

  g_assert (surface_width % 16 == 0);
  g_assert (surface_height % 16 == 0);
  g_assert (surface_width >= 16);
  g_assert (surface_height >= 16);

  h264_frame = g_new0 (H264Frame, 1);
  h264_frame->encode_session_vaapi = encode_session_vaapi;
  h264_frame->sequence_buffer = VA_INVALID_ID;
  h264_frame->picture_buffer = VA_INVALID_ID;
  h264_frame->slice_buffer = VA_INVALID_ID;
  h264_frame->bitstream_buffer = VA_INVALID_ID;

  h264_frame->reconstructed_picture =
    vaapi_picture_new (encode_session_vaapi,
                       surface_width, surface_height,
                       encode_session_vaapi->frame_num,
                       encode_session_vaapi->pending_idr_frame,
                       error);
  if (!h264_frame->reconstructed_picture)
    return NULL;

  return g_steal_pointer (&h264_frame);
}

static void
h264_frame_free (H264Frame *h264_frame)
{
  GrdEncodeSessionVaapi *encode_session_vaapi =
    h264_frame->encode_session_vaapi;
  VADisplay va_display = encode_session_vaapi->va_display;
  GList *l;

  for (l = h264_frame->misc_buffers; l; l = l->next)
    vaDestroyBuffer (va_display, GPOINTER_TO_UINT (l->data));
  g_clear_pointer (&h264_frame->misc_buffers, g_list_free);

  for (l = h264_frame->packed_header_buffers; l; l = l->next)
    vaDestroyBuffer (va_display, GPOINTER_TO_UINT (l->data));
  g_clear_pointer (&h264_frame->packed_header_buffers, g_list_free);

  if (h264_frame->bitstream_buffer != VA_INVALID_ID)
    vaDestroyBuffer (va_display, h264_frame->bitstream_buffer);
  if (h264_frame->slice_buffer != VA_INVALID_ID)
    vaDestroyBuffer (va_display, h264_frame->slice_buffer);
  if (h264_frame->picture_buffer != VA_INVALID_ID)
    vaDestroyBuffer (va_display, h264_frame->picture_buffer);
  if (h264_frame->sequence_buffer != VA_INVALID_ID)
    vaDestroyBuffer (va_display, h264_frame->sequence_buffer);

  g_clear_pointer (&h264_frame->avc_frame_info, g_free);
  g_clear_pointer (&h264_frame->reconstructed_picture, vaapi_picture_free);

  g_free (h264_frame);
}

static VABufferID *
h264_frame_get_buffers (H264Frame *h264_frame,
                        uint32_t  *n_buffers)
{
  VABufferID *va_buffers;
  uint32_t i = 0;
  GList *l;

  *n_buffers = 0;

  if (h264_frame->sequence_buffer != VA_INVALID_ID)
    *n_buffers += 1;

  g_assert (h264_frame->picture_buffer != VA_INVALID_ID);
  g_assert (h264_frame->slice_buffer != VA_INVALID_ID);
  *n_buffers += 2;

  *n_buffers += g_list_length (h264_frame->packed_header_buffers);
  *n_buffers += g_list_length (h264_frame->misc_buffers);

  g_assert (*n_buffers > 0);
  va_buffers = g_new0 (VABufferID, *n_buffers);

  if (h264_frame->sequence_buffer != VA_INVALID_ID)
    va_buffers[i++] = h264_frame->sequence_buffer;

  va_buffers[i++] = h264_frame->picture_buffer;

  for (l = h264_frame->packed_header_buffers; l; l = l->next)
    {
      g_assert (i < *n_buffers);

      va_buffers[i++] = GPOINTER_TO_UINT (l->data);
    }
  for (l = h264_frame->misc_buffers; l; l = l->next)
    {
      g_assert (i < *n_buffers);

      va_buffers[i++] = GPOINTER_TO_UINT (l->data);
    }
  va_buffers[i++] = h264_frame->slice_buffer;
  g_assert (i == *n_buffers);

  return va_buffers;
}

static NV12VkVASurface *
nv12_vkva_surface_new (GrdEncodeSessionVaapi  *encode_session_vaapi,
                       uint32_t                surface_width,
                       uint32_t                surface_height,
                       GError                **error)
{
  g_autoptr (NV12VkVASurface) vkva_surface = NULL;
  VASurfaceAttrib surface_attributes[2] = {};
  VADRMPRIMESurfaceDescriptor prime_descriptor = {};
  g_autoptr (GrdVkImage) vk_y_layer = NULL;
  g_autoptr (GrdVkImage) vk_uv_layer = NULL;
  GrdImageViewNV12 *image_view_nv12;
  uint32_t object_idx;
  int fd;
  uint64_t drm_format_modifier;
  VAStatus va_status;

  g_assert (surface_width % 16 == 0);
  g_assert (surface_height % 16 == 0);
  g_assert (surface_width >= 16);
  g_assert (surface_height >= 16);

  vkva_surface = g_new0 (NV12VkVASurface, 1);
  vkva_surface->encode_session_vaapi = encode_session_vaapi;
  vkva_surface->va_surface = VA_INVALID_SURFACE;
  vkva_surface->fds[0] = -1;
  vkva_surface->fds[1] = -1;

  surface_attributes[0].type = VASurfaceAttribPixelFormat;
  surface_attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
  surface_attributes[0].value.type = VAGenericValueTypeInteger;
  surface_attributes[0].value.value.i = VA_FOURCC_NV12;

  surface_attributes[1].type = VASurfaceAttribUsageHint;
  surface_attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
  surface_attributes[1].value.type = VAGenericValueTypeInteger;
  surface_attributes[1].value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT;

  va_status = vaCreateSurfaces (encode_session_vaapi->va_display,
                                VA_RT_FORMAT_YUV420,
                                surface_width, surface_height,
                                &vkva_surface->va_surface, 1,
                                surface_attributes,
                                G_N_ELEMENTS (surface_attributes));
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create NV12 surface: %s", vaErrorStr (va_status));
      return NULL;
    }
  g_assert (vkva_surface->va_surface != VA_INVALID_SURFACE);

  va_status = vaExportSurfaceHandle (encode_session_vaapi->va_display,
                                     vkva_surface->va_surface,
                                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                     VA_EXPORT_SURFACE_WRITE_ONLY |
                                     VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                     &prime_descriptor);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to export surface handle: %s",
                   vaErrorStr (va_status));
      return NULL;
    }
  g_assert (prime_descriptor.num_layers == 2);
  g_assert (prime_descriptor.layers[0].num_planes == 1);
  g_assert (prime_descriptor.layers[1].num_planes == 1);

  g_assert (prime_descriptor.num_objects > 0);
  g_assert (prime_descriptor.num_objects <= 2);
  g_assert (prime_descriptor.objects[0].fd != -1);
  if (prime_descriptor.num_objects > 1)
    g_assert (prime_descriptor.objects[1].fd != -1);

  vkva_surface->fds[0] = prime_descriptor.objects[0].fd;
  if (prime_descriptor.num_objects > 1)
    vkva_surface->fds[1] = prime_descriptor.objects[1].fd;

  object_idx = prime_descriptor.layers[0].object_index[0];
  fd = prime_descriptor.objects[object_idx].fd;
  drm_format_modifier =
    prime_descriptor.objects[object_idx].drm_format_modifier;

  vk_y_layer =
    grd_vk_dma_buf_image_new (encode_session_vaapi->vk_device,
                              VK_FORMAT_R8_UNORM,
                              surface_width, surface_height,
                              VK_IMAGE_USAGE_STORAGE_BIT, fd,
                              prime_descriptor.layers[0].offset[0],
                              prime_descriptor.layers[0].pitch[0],
                              drm_format_modifier, error);
  if (!vk_y_layer)
    return NULL;

  object_idx = prime_descriptor.layers[1].object_index[0];
  fd = prime_descriptor.objects[object_idx].fd;
  drm_format_modifier =
    prime_descriptor.objects[object_idx].drm_format_modifier;

  vk_uv_layer =
    grd_vk_dma_buf_image_new (encode_session_vaapi->vk_device,
                              VK_FORMAT_R8G8_UNORM,
                              surface_width / 2, surface_height / 2,
                              VK_IMAGE_USAGE_STORAGE_BIT, fd,
                              prime_descriptor.layers[1].offset[0],
                              prime_descriptor.layers[1].pitch[0],
                              drm_format_modifier, error);
  if (!vk_uv_layer)
    return NULL;

  image_view_nv12 =
    grd_image_view_nv12_new (g_steal_pointer (&vk_y_layer),
                             g_steal_pointer (&vk_uv_layer));
  vkva_surface->image_view = GRD_IMAGE_VIEW (image_view_nv12);

  g_clear_fd (&vkva_surface->fds[1], NULL);
  g_clear_fd (&vkva_surface->fds[0], NULL);

  return g_steal_pointer (&vkva_surface);
}

static void
nv12_vkva_surface_free (NV12VkVASurface *vkva_surface)
{
  GrdEncodeSessionVaapi *encode_session_vaapi =
    vkva_surface->encode_session_vaapi;
  VADisplay va_display = encode_session_vaapi->va_display;

  g_clear_object (&vkva_surface->image_view);

  g_clear_fd (&vkva_surface->fds[1], NULL);
  g_clear_fd (&vkva_surface->fds[0], NULL);

  if (vkva_surface->va_surface != VA_INVALID_SURFACE)
    vaDestroySurfaces (va_display, &vkva_surface->va_surface, 1);

  g_free (vkva_surface);
}

static void
grd_encode_session_vaapi_get_surface_size (GrdEncodeSession *encode_session,
                                           uint32_t         *surface_width,
                                           uint32_t         *surface_height)
{
  GrdEncodeSessionVaapi *encode_session_vaapi =
    GRD_ENCODE_SESSION_VAAPI (encode_session);

  g_assert (encode_session_vaapi->surface_width % 16 == 0);
  g_assert (encode_session_vaapi->surface_height % 16 == 0);
  g_assert (encode_session_vaapi->surface_width >= 16);
  g_assert (encode_session_vaapi->surface_height >= 16);

  *surface_width = encode_session_vaapi->surface_width;
  *surface_height = encode_session_vaapi->surface_height;
}

static GList *
grd_encode_session_vaapi_get_image_views (GrdEncodeSession *encode_session)
{
  GrdEncodeSessionVaapi *encode_session_vaapi =
    GRD_ENCODE_SESSION_VAAPI (encode_session);

  return g_hash_table_get_keys (encode_session_vaapi->surfaces);
}

static gboolean
grd_encode_session_vaapi_has_pending_frames (GrdEncodeSession *encode_session)
{
  GrdEncodeSessionVaapi *encode_session_vaapi =
    GRD_ENCODE_SESSION_VAAPI (encode_session);
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&encode_session_vaapi->pending_frames_mutex);
  return g_hash_table_size (encode_session_vaapi->pending_frames) > 0;
}

static gboolean
create_packed_header_buffers (GrdEncodeSessionVaapi  *encode_session_vaapi,
                              H264Frame              *h264_frame,
                              VAEncPackedHeaderType   header_type,
                              uint8_t                *header_data,
                              uint32_t                header_length,
                              GError                **error)
{
  VAEncPackedHeaderParameterBuffer packed_header_param = {};
  VABufferID packed_header_buffer = VA_INVALID_ID;
  VABufferID packed_header_data_buffer = VA_INVALID_ID;
  VAStatus va_status;

  packed_header_param.type = header_type;
  packed_header_param.bit_length = header_length;
  packed_header_param.has_emulation_bytes = 0;

  va_status = vaCreateBuffer (encode_session_vaapi->va_display,
                              encode_session_vaapi->va_context,
                              VAEncPackedHeaderParameterBufferType,
                              sizeof (VAEncPackedHeaderParameterBuffer), 1,
                              &packed_header_param,
                              &packed_header_buffer);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create packed header parameter buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (packed_header_buffer != VA_INVALID_ID);

  h264_frame->packed_header_buffers =
    g_list_append (h264_frame->packed_header_buffers,
                   GUINT_TO_POINTER (packed_header_buffer));

  va_status = vaCreateBuffer (encode_session_vaapi->va_display,
                              encode_session_vaapi->va_context,
                              VAEncPackedHeaderDataBufferType,
                              (header_length + 7) / 8, 1,
                              header_data,
                              &packed_header_data_buffer);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create packed header data buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (packed_header_data_buffer != VA_INVALID_ID);

  h264_frame->packed_header_buffers =
    g_list_append (h264_frame->packed_header_buffers,
                   GUINT_TO_POINTER (packed_header_data_buffer));

  return TRUE;
}

static gboolean
ensure_access_unit_delimiter (GrdEncodeSessionVaapi  *encode_session_vaapi,
                              H264Frame              *h264_frame,
                              GError                **error)
{
  GrdNalWriter *nal_writer = encode_session_vaapi->nal_writer;
  g_autofree uint8_t *header_data = NULL;
  uint32_t header_length = 0;

  header_data = grd_nal_writer_get_aud_bitstream (nal_writer, &header_length);
  if (!create_packed_header_buffers (encode_session_vaapi, h264_frame,
                                     VAEncPackedHeaderRawData,
                                     header_data, header_length, error))
    return FALSE;

  h264_frame->aud_header_length = header_length;

  return TRUE;
}

static void
fill_avc_sequence (GrdEncodeSessionVaapi            *encode_session_vaapi,
                   VAEncSequenceParameterBufferH264 *sequence_param)
{
  uint32_t surface_width = encode_session_vaapi->surface_width;
  uint32_t surface_height = encode_session_vaapi->surface_height;

  g_assert (surface_width % 16 == 0);
  g_assert (surface_height % 16 == 0);
  g_assert (surface_width >= 16);
  g_assert (surface_height >= 16);

  sequence_param->seq_parameter_set_id = 0;
  sequence_param->level_idc = encode_session_vaapi->level_idc;
  sequence_param->intra_period = 0;
  sequence_param->intra_idr_period = sequence_param->intra_period;
  sequence_param->ip_period = 1; /* 1 + B-Frames */

  sequence_param->bits_per_second = 0;
  sequence_param->max_num_ref_frames = 1;
  sequence_param->picture_width_in_mbs = surface_width / 16;
  sequence_param->picture_height_in_mbs = surface_height / 16;

  sequence_param->seq_fields.value = 0;
  sequence_param->seq_fields.bits.chroma_format_idc = 1;
  sequence_param->seq_fields.bits.frame_mbs_only_flag = 1;
  sequence_param->seq_fields.bits.direct_8x8_inference_flag = 1;
  sequence_param->seq_fields.bits.log2_max_frame_num_minus4 =
    LOG2_MAX_FRAME_NUM - 4;
  sequence_param->seq_fields.bits.pic_order_cnt_type = 2;

  sequence_param->vui_parameters_present_flag = 1;
  sequence_param->vui_fields.value = 0;
  sequence_param->vui_fields.bits.aspect_ratio_info_present_flag = 1;
  sequence_param->vui_fields.bits.timing_info_present_flag = 1;
  sequence_param->vui_fields.bits.bitstream_restriction_flag = 1;
  sequence_param->vui_fields.bits.log2_max_mv_length_horizontal = 15;
  sequence_param->vui_fields.bits.log2_max_mv_length_vertical = 15;
  sequence_param->aspect_ratio_idc = H264_Extended_SAR;
  sequence_param->sar_width = 1;
  sequence_param->sar_height = 1;
  sequence_param->num_units_in_tick = 1 * 1000;
  sequence_param->time_scale = 2 * encode_session_vaapi->refresh_rate * 1000;
}

static gboolean
create_sequence_buffer (GrdEncodeSessionVaapi  *encode_session_vaapi,
                        H264Frame              *h264_frame,
                        GError                **error)
{
  VAEncSequenceParameterBufferH264 *sequence_param =
    &encode_session_vaapi->sequence_param;
  VAStatus va_status;

  va_status = vaCreateBuffer (encode_session_vaapi->va_display,
                              encode_session_vaapi->va_context,
                              VAEncSequenceParameterBufferType,
                              sizeof (VAEncSequenceParameterBufferH264), 1,
                              sequence_param,
                              &h264_frame->sequence_buffer);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create sequence parameter buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (h264_frame->sequence_buffer != VA_INVALID_ID);

  return TRUE;
}

static gboolean
maybe_ensure_sequence (GrdEncodeSessionVaapi  *encode_session_vaapi,
                       H264Frame              *h264_frame,
                       GError                **error)
{
  GrdNalWriter *nal_writer = encode_session_vaapi->nal_writer;
  VAAPIPicture *picture = h264_frame->reconstructed_picture;
  VAEncSequenceParameterBufferH264 *sequence_param =
    &encode_session_vaapi->sequence_param;
  g_autofree uint8_t *header_data = NULL;
  uint32_t header_length = 0;

  if (!picture->is_idr)
    return TRUE;

  *sequence_param = (VAEncSequenceParameterBufferH264) {};
  fill_avc_sequence (encode_session_vaapi, sequence_param);

  if (!create_sequence_buffer (encode_session_vaapi, h264_frame, error))
    return FALSE;

  header_data = grd_nal_writer_get_sps_bitstream (nal_writer, sequence_param,
                                                  &header_length);
  if (!create_packed_header_buffers (encode_session_vaapi, h264_frame,
                                     VAEncPackedHeaderSequence,
                                     header_data, header_length, error))
    return FALSE;

  h264_frame->sequence_header_length = header_length;

  return TRUE;
}

static gboolean
ensure_misc_param_quality_level (GrdEncodeSessionVaapi  *encode_session_vaapi,
                                 H264Frame              *h264_frame,
                                 GError                **error)
{
  VAEncMiscParameterBuffer *misc_param = NULL;
  VAEncMiscParameterBufferQualityLevel *misc_param_quality_level;
  VABufferID misc_param_buffer = VA_INVALID_ID;
  uint32_t total_size;
  VAStatus va_status;

  total_size = sizeof (VAEncMiscParameterBuffer) +
               sizeof (VAEncMiscParameterBufferQualityLevel);

  va_status = vaCreateBuffer (encode_session_vaapi->va_display,
                              encode_session_vaapi->va_context,
                              VAEncMiscParameterBufferType,
                              total_size, 1,
                              NULL,
                              &misc_param_buffer);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create misc parameter buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (misc_param_buffer != VA_INVALID_ID);

  va_status = vaMapBuffer (encode_session_vaapi->va_display,
                           misc_param_buffer,
                           (void **) &misc_param);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to map misc parameter buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (misc_param);

  misc_param->type = VAEncMiscParameterTypeQualityLevel;

  misc_param_quality_level =
    (VAEncMiscParameterBufferQualityLevel *) misc_param->data;
  *misc_param_quality_level = (VAEncMiscParameterBufferQualityLevel) {};

  misc_param_quality_level->quality_level = 0;

  va_status = vaUnmapBuffer (encode_session_vaapi->va_display,
                             misc_param_buffer);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to unmap misc parameter buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }

  h264_frame->misc_buffers =
    g_list_append (h264_frame->misc_buffers,
                   GUINT_TO_POINTER (misc_param_buffer));

  return TRUE;
}

static gboolean
ensure_misc_param_sub_mb_part_pel_h264 (GrdEncodeSessionVaapi  *encode_session_vaapi,
                                        H264Frame              *h264_frame,
                                        GError                **error)
{
  VAEncMiscParameterBuffer *misc_param = NULL;
  VAEncMiscParameterSubMbPartPelH264 *misc_param_sub_mb_part_pel_h264;
  VABufferID misc_param_buffer = VA_INVALID_ID;
  uint32_t total_size;
  VAStatus va_status;

  total_size = sizeof (VAEncMiscParameterBuffer) +
               sizeof (VAEncMiscParameterSubMbPartPelH264);

  va_status = vaCreateBuffer (encode_session_vaapi->va_display,
                              encode_session_vaapi->va_context,
                              VAEncMiscParameterBufferType,
                              total_size, 1,
                              NULL,
                              &misc_param_buffer);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create misc parameter buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (misc_param_buffer != VA_INVALID_ID);

  va_status = vaMapBuffer (encode_session_vaapi->va_display,
                           misc_param_buffer,
                           (void **) &misc_param);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to map misc parameter buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (misc_param);

  misc_param->type = VAEncMiscParameterTypeSubMbPartPel;

  misc_param_sub_mb_part_pel_h264 =
    (VAEncMiscParameterSubMbPartPelH264 *) misc_param->data;
  *misc_param_sub_mb_part_pel_h264 = (VAEncMiscParameterSubMbPartPelH264) {};

  misc_param_sub_mb_part_pel_h264->enable_sub_pel_mode = 1;
  misc_param_sub_mb_part_pel_h264->sub_pel_mode = 3;

  va_status = vaUnmapBuffer (encode_session_vaapi->va_display,
                             misc_param_buffer);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to unmap misc parameter buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }

  h264_frame->misc_buffers =
    g_list_append (h264_frame->misc_buffers,
                   GUINT_TO_POINTER (misc_param_buffer));

  return TRUE;
}

static gboolean
ensure_misc_params (GrdEncodeSessionVaapi  *encode_session_vaapi,
                    H264Frame              *h264_frame,
                    GError                **error)
{
  VAAPIPicture *picture = h264_frame->reconstructed_picture;

  if (picture->is_idr &&
      encode_session_vaapi->supports_quality_level &&
      !ensure_misc_param_quality_level (encode_session_vaapi, h264_frame, error))
    return FALSE;
  if (!ensure_misc_param_sub_mb_part_pel_h264 (encode_session_vaapi, h264_frame,
                                               error))
    return FALSE;

  return TRUE;
}

static void
fill_avc_picture (GrdEncodeSessionVaapi           *encode_session_vaapi,
                  VAEncPictureParameterBufferH264 *picture_param,
                  H264Frame                       *h264_frame)
{
  VAAPIPicture *picture = h264_frame->reconstructed_picture;
  uint8_t i = 0;

  picture_param->CurrPic.picture_id = picture->va_surface;
  picture_param->CurrPic.frame_idx = picture->frame_num;
  picture_param->CurrPic.TopFieldOrderCnt = picture->frame_num * 2;
  picture_param->CurrPic.BottomFieldOrderCnt = picture->frame_num * 2;

  if (!picture->is_idr)
    {
      VAPictureH264 *h264_picture = &picture_param->ReferenceFrames[0];
      VAAPIPicture *reference = encode_session_vaapi->reference_picture;

      g_assert (reference);

      h264_picture->picture_id = reference->va_surface;
      h264_picture->frame_idx = reference->frame_num;
      h264_picture->flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
      h264_picture->TopFieldOrderCnt = reference->frame_num * 2;
      h264_picture->BottomFieldOrderCnt = reference->frame_num * 2;
      ++i;
    }
  for (; i < G_N_ELEMENTS (picture_param->ReferenceFrames); ++i)
    {
      picture_param->ReferenceFrames[i].picture_id = VA_INVALID_ID;
      picture_param->ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }

  picture_param->pic_parameter_set_id = 0;
  picture_param->seq_parameter_set_id = 0;

  picture_param->last_picture = 0;

  picture_param->frame_num = picture->frame_num;
  picture_param->pic_init_qp = 22;
  picture_param->num_ref_idx_l0_active_minus1 = 0;
  picture_param->num_ref_idx_l1_active_minus1 = 0;

  picture_param->chroma_qp_index_offset = 0;
  picture_param->second_chroma_qp_index_offset = 0;

  picture_param->pic_fields.value = 0;

  if (picture->is_idr)
    picture_param->pic_fields.bits.idr_pic_flag = 1;
  picture_param->pic_fields.bits.reference_pic_flag = 1; /* No B-Frames are present */
  picture_param->pic_fields.bits.entropy_coding_mode_flag = 1; /* CABAC */
  picture_param->pic_fields.bits.transform_8x8_mode_flag = 1;
}

static gboolean
prepare_picture (GrdEncodeSessionVaapi  *encode_session_vaapi,
                 H264Frame              *h264_frame,
                 GError                **error)
{
  GrdNalWriter *nal_writer = encode_session_vaapi->nal_writer;
  VAAPIPicture *picture = h264_frame->reconstructed_picture;
  VAEncPictureParameterBufferH264 *picture_param =
    &encode_session_vaapi->picture_param;
  g_autofree uint8_t *header_data = NULL;
  uint32_t header_length = 0;

  *picture_param = (VAEncPictureParameterBufferH264) {};
  fill_avc_picture (encode_session_vaapi, picture_param, h264_frame);

  if (!picture->is_idr)
    return TRUE;

  header_data = grd_nal_writer_get_pps_bitstream (nal_writer, picture_param,
                                                  &header_length);
  if (!create_packed_header_buffers (encode_session_vaapi, h264_frame,
                                     VAEncPackedHeaderPicture,
                                     header_data, header_length, error))
    return FALSE;

  h264_frame->picture_header_length = header_length;

  return TRUE;
}

static void
fill_avc_slice (GrdEncodeSessionVaapi         *encode_session_vaapi,
                VAEncSliceParameterBufferH264 *slice_param,
                H264Frame                     *h264_frame)
{
  VAAPIPicture *picture = h264_frame->reconstructed_picture;
  uint32_t surface_width = encode_session_vaapi->surface_width;
  uint32_t surface_height = encode_session_vaapi->surface_height;
  uint32_t width_in_mbs;
  uint32_t height_in_mbs;
  uint32_t n_macroblocks;
  uint8_t i = 0;

  g_assert (G_N_ELEMENTS (slice_param->RefPicList0) < 255);
  g_assert (G_N_ELEMENTS (slice_param->RefPicList1) < 255);

  g_assert (surface_width % 16 == 0);
  g_assert (surface_height % 16 == 0);
  g_assert (surface_width >= 16);
  g_assert (surface_height >= 16);

  width_in_mbs = surface_width / 16;
  height_in_mbs = surface_height / 16;
  n_macroblocks = width_in_mbs * height_in_mbs;

  slice_param->macroblock_address = 0;
  slice_param->num_macroblocks = n_macroblocks;
  slice_param->macroblock_info = VA_INVALID_ID;
  slice_param->slice_type = picture->is_idr ? H264_SLICE_TYPE_I
                                            : H264_SLICE_TYPE_P;
  slice_param->pic_parameter_set_id = 0;
  slice_param->idr_pic_id = 0;

  slice_param->pic_order_cnt_lsb = picture->frame_num * 2;

  if (!picture->is_idr)
    {
      VAAPIPicture *reference = encode_session_vaapi->reference_picture;

      g_assert (reference);

      slice_param->RefPicList0[0].picture_id = reference->va_surface;
      slice_param->RefPicList0[0].frame_idx = reference->frame_num;
      slice_param->RefPicList0[0].TopFieldOrderCnt = reference->frame_num * 2;
      slice_param->RefPicList0[0].BottomFieldOrderCnt = reference->frame_num * 2;
      slice_param->RefPicList0[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
      ++i;
    }
  for (; i < G_N_ELEMENTS (slice_param->RefPicList0); ++i)
    {
      slice_param->RefPicList0[i].picture_id = VA_INVALID_SURFACE;
      slice_param->RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
    }
  /* B slices are not used here */
  for (i = 0; i < G_N_ELEMENTS (slice_param->RefPicList1); ++i)
    {
      slice_param->RefPicList1[i].picture_id = VA_INVALID_SURFACE;
      slice_param->RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
    }

  slice_param->slice_qp_delta = 0;
}

static gboolean
create_slice_buffer (GrdEncodeSessionVaapi          *encode_session_vaapi,
                     H264Frame                      *h264_frame,
                     VAEncSliceParameterBufferH264  *slice_param,
                     GError                        **error)
{
  VAStatus va_status;

  va_status = vaCreateBuffer (encode_session_vaapi->va_display,
                              encode_session_vaapi->va_context,
                              VAEncSliceParameterBufferType,
                              sizeof (VAEncSliceParameterBufferH264), 1,
                              slice_param,
                              &h264_frame->slice_buffer);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create slice parameter buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (h264_frame->slice_buffer != VA_INVALID_ID);

  return TRUE;
}

static gboolean
ensure_slice (GrdEncodeSessionVaapi  *encode_session_vaapi,
              H264Frame              *h264_frame,
              GError                **error)
{
  GrdNalWriter *nal_writer = encode_session_vaapi->nal_writer;
  VAEncSliceParameterBufferH264 slice_param = {};
  g_autofree uint8_t *header_data = NULL;
  uint32_t header_length = 0;

  fill_avc_slice (encode_session_vaapi, &slice_param, h264_frame);
  if (!create_slice_buffer (encode_session_vaapi, h264_frame, &slice_param,
                            error))
    return FALSE;

  header_data =
    grd_nal_writer_get_slice_header_bitstream (nal_writer, &slice_param,
                                               &encode_session_vaapi->sequence_param,
                                               &encode_session_vaapi->picture_param,
                                               &header_length);
  if (!create_packed_header_buffers (encode_session_vaapi, h264_frame,
                                     VAEncPackedHeaderSlice,
                                     header_data, header_length, error))
    return FALSE;

  h264_frame->slice_header_length = header_length;

  return TRUE;
}

static gboolean
ensure_bitstream_buffer (GrdEncodeSessionVaapi  *encode_session_vaapi,
                         H264Frame              *h264_frame,
                         GError                **error)
{
  VAEncPictureParameterBufferH264 *picture_param =
    &encode_session_vaapi->picture_param;
  uint32_t surface_width = encode_session_vaapi->surface_width;
  uint32_t surface_height = encode_session_vaapi->surface_height;
  uint32_t bitstream_buffer_size;
  uint32_t header_lengths = 0;
  uint32_t width_in_mbs;
  uint32_t height_in_mbs;
  VAStatus va_status;

  g_assert (surface_width % 16 == 0);
  g_assert (surface_height % 16 == 0);
  g_assert (surface_width >= 16);
  g_assert (surface_height >= 16);

  width_in_mbs = surface_width / 16;
  height_in_mbs = surface_height / 16;

  bitstream_buffer_size = width_in_mbs * height_in_mbs * 400;

  header_lengths += h264_frame->aud_header_length;
  header_lengths += h264_frame->sequence_header_length;
  header_lengths += h264_frame->picture_header_length;
  header_lengths += h264_frame->slice_header_length;
  bitstream_buffer_size += (header_lengths + 7) / 8;

  va_status = vaCreateBuffer (encode_session_vaapi->va_display,
                              encode_session_vaapi->va_context,
                              VAEncCodedBufferType,
                              bitstream_buffer_size, 1,
                              NULL,
                              &h264_frame->bitstream_buffer);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create bitstream buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (h264_frame->bitstream_buffer != VA_INVALID_ID);

  picture_param->coded_buf = h264_frame->bitstream_buffer;

  return TRUE;
}

static gboolean
create_picture_buffer (GrdEncodeSessionVaapi  *encode_session_vaapi,
                       H264Frame              *h264_frame,
                       GError                **error)
{
  VAEncPictureParameterBufferH264 *picture_param =
    &encode_session_vaapi->picture_param;
  VAStatus va_status;

  va_status = vaCreateBuffer (encode_session_vaapi->va_display,
                              encode_session_vaapi->va_context,
                              VAEncPictureParameterBufferType,
                              sizeof (VAEncPictureParameterBufferH264), 1,
                              picture_param,
                              &h264_frame->picture_buffer);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create picture parameter buffer: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (h264_frame->picture_buffer != VA_INVALID_ID);

  return TRUE;
}

static gboolean
render_picture (GrdEncodeSessionVaapi  *encode_session_vaapi,
                VAContextID             va_context,
                VASurfaceID             va_render_target,
                VABufferID             *va_buffers,
                uint32_t                n_buffers,
                GError                **error)
{
  VAStatus va_status;

  va_status = vaBeginPicture (encode_session_vaapi->va_display, va_context,
                              va_render_target);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to begin picture: %s", vaErrorStr (va_status));
      return FALSE;
    }

  va_status = vaRenderPicture (encode_session_vaapi->va_display, va_context,
                               va_buffers, n_buffers);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to submit buffers: %s", vaErrorStr (va_status));
      return FALSE;
    }

  va_status = vaEndPicture (encode_session_vaapi->va_display, va_context);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to end picture: %s", vaErrorStr (va_status));
      return FALSE;
    }

  return TRUE;
}

static void
prepare_avc_frame_info (GrdEncodeSessionVaapi *encode_session_vaapi,
                        H264Frame             *h264_frame)
{
  VAAPIPicture *picture = h264_frame->reconstructed_picture;
  VAEncPictureParameterBufferH264 *picture_param =
    &encode_session_vaapi->picture_param;

  h264_frame->avc_frame_info =
    grd_avc_frame_info_new (picture->is_idr ? GRD_AVC_FRAME_TYPE_I
                                            : GRD_AVC_FRAME_TYPE_P,
                            picture_param->pic_init_qp,
                            100);
}

static H264Frame *
encode_frame (GrdEncodeSessionVaapi  *encode_session_vaapi,
              VASurfaceID             src_surface,
              GError                **error)
{
  g_autoptr (H264Frame) h264_frame = NULL;
  g_autofree VABufferID *va_buffers = NULL;
  uint32_t n_buffers = 0;

  h264_frame = h264_frame_new (encode_session_vaapi, error);
  if (!h264_frame)
    return NULL;

  if (!ensure_access_unit_delimiter (encode_session_vaapi, h264_frame, error))
    return NULL;
  if (!maybe_ensure_sequence (encode_session_vaapi, h264_frame, error))
    return NULL;
  if (!ensure_misc_params (encode_session_vaapi, h264_frame, error))
    return NULL;
  if (!prepare_picture (encode_session_vaapi, h264_frame, error))
    return NULL;
  if (!ensure_slice (encode_session_vaapi, h264_frame, error))
    return NULL;
  if (!ensure_bitstream_buffer (encode_session_vaapi, h264_frame, error))
    return NULL;
  if (!create_picture_buffer (encode_session_vaapi, h264_frame, error))
    return NULL;

  if (encode_session_vaapi->debug_va_times)
    h264_frame->timestamps[0] = g_get_monotonic_time ();

  va_buffers = h264_frame_get_buffers (h264_frame, &n_buffers);
  if (!render_picture (encode_session_vaapi, encode_session_vaapi->va_context,
                       src_surface, va_buffers, n_buffers, error))
    return NULL;

  h264_frame->src_surface = src_surface;
  prepare_avc_frame_info (encode_session_vaapi, h264_frame);

  if (encode_session_vaapi->debug_va_times)
    h264_frame->timestamps[1] = g_get_monotonic_time ();

  return g_steal_pointer (&h264_frame);
}

static void
prepare_next_frame (GrdEncodeSessionVaapi *encode_session_vaapi,
                    H264Frame             *current_h264_frame)
{
  uint16_t max_frame_num;

  g_clear_pointer (&encode_session_vaapi->reference_picture, vaapi_picture_free);
  encode_session_vaapi->reference_picture =
    g_steal_pointer (&current_h264_frame->reconstructed_picture);

  max_frame_num = 1 << LOG2_MAX_FRAME_NUM;

  ++encode_session_vaapi->frame_num;
  encode_session_vaapi->frame_num %= max_frame_num + 1;

  encode_session_vaapi->pending_idr_frame = FALSE;
}

static gboolean
grd_encode_session_vaapi_encode_frame (GrdEncodeSession  *encode_session,
                                       GrdImageView      *image_view,
                                       GError           **error)
{
  GrdEncodeSessionVaapi *encode_session_vaapi =
    GRD_ENCODE_SESSION_VAAPI (encode_session);
  NV12VkVASurface *vkva_surface = NULL;
  H264Frame *h264_frame = NULL;

  g_mutex_lock (&encode_session_vaapi->pending_frames_mutex);
  g_assert (!g_hash_table_contains (encode_session_vaapi->pending_frames,
                                    image_view));
  g_mutex_unlock (&encode_session_vaapi->pending_frames_mutex);

  if (!g_hash_table_lookup_extended (encode_session_vaapi->surfaces,
                                     image_view,
                                     NULL, (gpointer *) &vkva_surface))
    g_assert_not_reached ();

  g_assert (vkva_surface);

  h264_frame = encode_frame (encode_session_vaapi, vkva_surface->va_surface,
                             error);
  if (!h264_frame)
    return FALSE;

  g_mutex_lock (&encode_session_vaapi->pending_frames_mutex);
  g_hash_table_insert (encode_session_vaapi->pending_frames,
                       image_view, h264_frame);
  g_mutex_unlock (&encode_session_vaapi->pending_frames_mutex);

  prepare_next_frame (encode_session_vaapi, h264_frame);

  return TRUE;
}

static gboolean
get_bitstream (GrdEncodeSessionVaapi  *encode_session_vaapi,
               VABufferID              output_buf,
               GrdBitstream          **bitstream,
               GError                **error)
{
  VACodedBufferSegment *va_segment = NULL;
  VAStatus va_status;

  va_status = vaMapBuffer (encode_session_vaapi->va_display,
                           output_buf,
                           (void **) &va_segment);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to map output buffer: %s", vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (va_segment);

  *bitstream = grd_bitstream_new (va_segment->buf, va_segment->size);

  return TRUE;
}

static gboolean
finish_frame_encoding (GrdEncodeSessionVaapi  *encode_session_vaapi,
                       H264Frame              *h264_frame,
                       GrdBitstream          **bitstream,
                       GError                **error)
{
  int64_t *timestamps = h264_frame->timestamps;
  VAStatus va_status;

  va_status = vaSyncSurface (encode_session_vaapi->va_display,
                             h264_frame->src_surface);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to sync surface: %s", vaErrorStr (va_status));
      return FALSE;
    }

  if (encode_session_vaapi->debug_va_times)
    timestamps[2] = g_get_monotonic_time ();

  if (!get_bitstream (encode_session_vaapi, h264_frame->bitstream_buffer,
                      bitstream, error))
    return FALSE;

  grd_bitstream_set_avc_frame_info (*bitstream,
                                    g_steal_pointer (&h264_frame->avc_frame_info));

  if (encode_session_vaapi->debug_va_times)
    {
      g_debug ("[HWAccel.VAAPI] EncodeFrame[Times]: submission: %liµs, "
               "waitForBitstream: %liµs, total: %liµs",
               timestamps[1] - timestamps[0],
               timestamps[2] - timestamps[1],
               timestamps[2] - timestamps[0]);
    }

  return TRUE;
}

static GrdBitstream *
grd_encode_session_vaapi_lock_bitstream (GrdEncodeSession  *encode_session,
                                         GrdImageView      *image_view,
                                         GError           **error)
{
  GrdEncodeSessionVaapi *encode_session_vaapi =
    GRD_ENCODE_SESSION_VAAPI (encode_session);
  g_autoptr (H264Frame) h264_frame = NULL;
  g_autoptr (GMutexLocker) locker = NULL;
  GrdBitstream *bitstream = NULL;

  g_mutex_lock (&encode_session_vaapi->pending_frames_mutex);
  if (!g_hash_table_steal_extended (encode_session_vaapi->pending_frames,
                                    image_view,
                                    NULL, (gpointer *) &h264_frame))
    g_assert_not_reached ();

  g_mutex_unlock (&encode_session_vaapi->pending_frames_mutex);
  g_assert (h264_frame);

  if (!finish_frame_encoding (encode_session_vaapi, h264_frame, &bitstream,
                              error))
    return NULL;

  locker = g_mutex_locker_new (&encode_session_vaapi->bitstreams_mutex);
  g_hash_table_insert (encode_session_vaapi->bitstreams,
                       bitstream, g_steal_pointer (&h264_frame));

  return bitstream;
}

static gboolean
grd_encode_session_vaapi_unlock_bitstream (GrdEncodeSession  *encode_session,
                                           GrdBitstream      *bitstream,
                                           GError           **error)
{
  GrdEncodeSessionVaapi *encode_session_vaapi =
    GRD_ENCODE_SESSION_VAAPI (encode_session);
  g_autoptr (H264Frame) h264_frame = NULL;
  g_autoptr (GMutexLocker) locker = NULL;
  VAStatus va_status;

  locker = g_mutex_locker_new (&encode_session_vaapi->bitstreams_mutex);
  if (!g_hash_table_steal_extended (encode_session_vaapi->bitstreams,
                                    bitstream,
                                    NULL, (gpointer *) &h264_frame))
    g_assert_not_reached ();

  g_clear_pointer (&locker, g_mutex_locker_free);
  g_assert (h264_frame);

  va_status = vaUnmapBuffer (encode_session_vaapi->va_display,
                             h264_frame->bitstream_buffer);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to unmap output buffer: %s", vaErrorStr (va_status));
      return FALSE;
    }

  grd_bitstream_free (bitstream);

  return TRUE;
}

static gboolean
get_avc_level_idc_for_mbps (uint32_t   mbps,
                            uint8_t   *level_idc,
                            GError   **error)
{
  *level_idc = 0;

  /* See also Table A-1 - Level limits (Rec. ITU-T H.264 (08/2021)) */
  if (mbps <= 1485)
    *level_idc = 10;
  else if (mbps <= 3000)
    *level_idc = 11;
  else if (mbps <= 6000)
    *level_idc = 12;
  else if (mbps <= 11880)
    *level_idc = 13;
  else if (mbps <= 19800)
    *level_idc = 21;
  else if (mbps <= 20250)
    *level_idc = 22;
  else if (mbps <= 40500)
    *level_idc = 30;
  else if (mbps <= 108000)
    *level_idc = 31;
  else if (mbps <= 216000)
    *level_idc = 32;
  else if (mbps <= 245760)
    *level_idc = 40;
  else if (mbps <= 522240)
    *level_idc = 42;
  else if (mbps <= 589824)
    *level_idc = 50;
  else if (mbps <= 983040)
    *level_idc = 51;
  else if (mbps <= 2073600)
    *level_idc = 52;
  else if (mbps <= 4177920)
    *level_idc = 60;
  else if (mbps <= 8355840)
    *level_idc = 61;
  else if (mbps <= 16711680)
    *level_idc = 62;

  if (*level_idc == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to determine level_idc");
      return FALSE;
    }

  return TRUE;
}

static gboolean
determine_avc_level_idc (GrdEncodeSessionVaapi  *encode_session_vaapi,
                         GError                **error)
{
  uint32_t surface_width = encode_session_vaapi->surface_width;
  uint32_t surface_height = encode_session_vaapi->surface_height;
  uint32_t width_in_mbs;
  uint32_t height_in_mbs;
  uint32_t mbs_per_sec;

  g_assert (surface_width % 16 == 0);
  g_assert (surface_height % 16 == 0);
  g_assert (surface_width >= 16);
  g_assert (surface_height >= 16);

  width_in_mbs = surface_width / 16;
  height_in_mbs = surface_height / 16;

  mbs_per_sec = width_in_mbs * height_in_mbs *
                encode_session_vaapi->refresh_rate;
  if (!get_avc_level_idc_for_mbps (mbs_per_sec,
                                   &encode_session_vaapi->level_idc,
                                   error))
    return FALSE;

  return TRUE;
}

static gboolean
get_surface_constraints (GrdEncodeSessionVaapi  *encode_session_vaapi,
                         gboolean               *supports_nv12,
                         int32_t                *min_width,
                         int32_t                *max_width,
                         int32_t                *min_height,
                         int32_t                *max_height,
                         GError                **error)
{
  g_autofree VASurfaceAttrib *surface_attributes = NULL;
  unsigned int n_attributes = 0;
  gboolean found_min_width = FALSE;
  gboolean found_max_width = FALSE;
  gboolean found_min_height = FALSE;
  gboolean found_max_height = FALSE;
  VAStatus va_status;
  unsigned int i;

  *supports_nv12 = FALSE;

  va_status = vaQuerySurfaceAttributes (encode_session_vaapi->va_display,
                                        encode_session_vaapi->va_config,
                                        NULL, &n_attributes);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to query num surface attributes: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }

  if (n_attributes == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid max num surface attributes");
      return FALSE;
    }
  surface_attributes = g_new0 (VASurfaceAttrib, n_attributes);

  va_status = vaQuerySurfaceAttributes (encode_session_vaapi->va_display,
                                        encode_session_vaapi->va_config,
                                        surface_attributes, &n_attributes);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to query surface attributes: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }

  for (i = 0; i < n_attributes; ++i)
    {
      if (!(surface_attributes[i].flags & VA_SURFACE_ATTRIB_GETTABLE) ||
          surface_attributes[i].value.type != VAGenericValueTypeInteger)
        continue;

      if (surface_attributes[i].type == VASurfaceAttribPixelFormat &&
          surface_attributes[i].value.value.i == VA_FOURCC_NV12)
        *supports_nv12 = TRUE;

      if (surface_attributes[i].type == VASurfaceAttribMinWidth)
        {
          *min_width = surface_attributes[i].value.value.i;
          found_min_width = TRUE;
        }
      if (surface_attributes[i].type == VASurfaceAttribMaxWidth)
        {
          *max_width = surface_attributes[i].value.value.i;
          found_max_width = TRUE;
        }
      if (surface_attributes[i].type == VASurfaceAttribMinHeight)
        {
          *min_height = surface_attributes[i].value.value.i;
          found_min_height = TRUE;
        }
      if (surface_attributes[i].type == VASurfaceAttribMaxHeight)
        {
          *max_height = surface_attributes[i].value.value.i;
          found_max_height = TRUE;
        }
    }

  if (found_min_width == FALSE || found_max_width == FALSE ||
      found_min_height == FALSE || found_max_height == FALSE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to query surface size constraints");
      return FALSE;
    }

  return TRUE;
}

static gboolean
create_avc420_encode_session (GrdEncodeSessionVaapi  *encode_session_vaapi,
                              GError                **error)
{
  uint32_t surface_width = encode_session_vaapi->surface_width;
  uint32_t surface_height = encode_session_vaapi->surface_height;
  VAConfigAttrib config_attributes[3] = {};
  gboolean supports_nv12 = FALSE;
  int32_t min_surface_width = 0;
  int32_t max_surface_width = 0;
  int32_t min_surface_height = 0;
  int32_t max_surface_height = 0;
  VAStatus va_status;
  uint32_t i;

  g_assert (surface_width % 16 == 0);
  g_assert (surface_height % 16 == 0);
  g_assert (surface_width >= 16);
  g_assert (surface_height >= 16);

  if (!determine_avc_level_idc (encode_session_vaapi, error))
    return FALSE;

  config_attributes[0].type = VAConfigAttribRTFormat;
  config_attributes[0].value = VA_RT_FORMAT_YUV420;

  config_attributes[1].type = VAConfigAttribRateControl;
  config_attributes[1].value = VA_RC_CQP;

  config_attributes[2].type = VAConfigAttribEncPackedHeaders;
  config_attributes[2].value = VA_ENC_PACKED_HEADER_SEQUENCE |
                              VA_ENC_PACKED_HEADER_PICTURE |
                              VA_ENC_PACKED_HEADER_SLICE |
                              VA_ENC_PACKED_HEADER_RAW_DATA;

  va_status = vaCreateConfig (encode_session_vaapi->va_display,
                              VAProfileH264High, VAEntrypointEncSlice,
                              config_attributes,
                              G_N_ELEMENTS (config_attributes),
                              &encode_session_vaapi->va_config);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create AVC420 encode session config: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (encode_session_vaapi->va_config != VA_INVALID_ID);

  if (!get_surface_constraints (encode_session_vaapi, &supports_nv12,
                                &min_surface_width, &max_surface_width,
                                &min_surface_height, &max_surface_height,
                                error))
    return FALSE;

  if (!supports_nv12)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Surface does not support NV12 as pixel format");
      return FALSE;
    }

  if (surface_width < min_surface_width || surface_width > max_surface_width ||
      surface_height < min_surface_height || surface_height > max_surface_height)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Surface size does not meet encoder constraints. Requested "
                   "size: [%i, %i], Size constraints: [[%i, %i], [%i, %i]]",
                   surface_width, surface_height,
                   min_surface_width, max_surface_width,
                   min_surface_height, max_surface_height);
      return FALSE;
    }

  g_debug ("[HWAccel.VAAPI] Requested surface size: [%i, %i], Size "
           "constraints: [[%i, %i], [%i, %i]]",
           surface_width, surface_height,
           min_surface_width, max_surface_width,
           min_surface_height, max_surface_height);

  va_status = vaCreateContext (encode_session_vaapi->va_display,
                               encode_session_vaapi->va_config,
                               surface_width, surface_height, VA_PROGRESSIVE,
                               NULL, 0,
                               &encode_session_vaapi->va_context);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create AVC420 encode session context: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }
  g_assert (encode_session_vaapi->va_context != VA_INVALID_ID);

  for (i = 0; i < N_SRC_SURFACES; ++i)
    {
      NV12VkVASurface *surface;

      surface = nv12_vkva_surface_new (encode_session_vaapi,
                                       surface_width, surface_height,
                                       error);
      if (!surface)
        return FALSE;

      g_hash_table_insert (encode_session_vaapi->surfaces,
                           surface->image_view, surface);
    }

  encode_session_vaapi->nal_writer = grd_nal_writer_new ();

  return TRUE;
}

GrdEncodeSessionVaapi *
grd_encode_session_vaapi_new (GrdVkDevice  *vk_device,
                              VADisplay     va_display,
                              gboolean      supports_quality_level,
                              uint32_t      source_width,
                              uint32_t      source_height,
                              uint32_t      refresh_rate,
                              GError      **error)
{
  g_autoptr (GrdEncodeSessionVaapi) encode_session_vaapi = NULL;
  uint8_t level_idc;

  encode_session_vaapi = g_object_new (GRD_TYPE_ENCODE_SESSION_VAAPI, NULL);
  encode_session_vaapi->vk_device = vk_device;
  encode_session_vaapi->va_display = va_display;
  encode_session_vaapi->supports_quality_level = supports_quality_level;

  encode_session_vaapi->surface_width = grd_get_aligned_size (source_width, 16);
  encode_session_vaapi->surface_height = grd_get_aligned_size (source_height, 16);
  encode_session_vaapi->refresh_rate = refresh_rate;

  if (!create_avc420_encode_session (encode_session_vaapi, error))
    return NULL;

  level_idc = encode_session_vaapi->level_idc;
  g_debug ("[HWAccel.VAAPI] Using level_idc %u.%u for AVC encode session",
           level_idc / 10, level_idc - (level_idc / 10) * 10);

  return g_steal_pointer (&encode_session_vaapi);
}

static void
grd_encode_session_vaapi_dispose (GObject *object)
{
  GrdEncodeSessionVaapi *encode_session_vaapi =
    GRD_ENCODE_SESSION_VAAPI (object);
  VADisplay va_display = encode_session_vaapi->va_display;

  g_assert (g_hash_table_size (encode_session_vaapi->bitstreams) == 0);
  g_assert (g_hash_table_size (encode_session_vaapi->pending_frames) == 0);

  g_clear_pointer (&encode_session_vaapi->reference_picture, vaapi_picture_free);
  g_clear_pointer (&encode_session_vaapi->surfaces, g_hash_table_unref);

  g_clear_object (&encode_session_vaapi->nal_writer);

  if (encode_session_vaapi->va_context != VA_INVALID_ID)
    vaDestroyContext (va_display, encode_session_vaapi->va_context);
  if (encode_session_vaapi->va_config != VA_INVALID_ID)
    vaDestroyConfig (va_display, encode_session_vaapi->va_config);

  G_OBJECT_CLASS (grd_encode_session_vaapi_parent_class)->dispose (object);
}

static void
grd_encode_session_vaapi_finalize (GObject *object)
{
  GrdEncodeSessionVaapi *encode_session_vaapi =
    GRD_ENCODE_SESSION_VAAPI (object);

  g_mutex_clear (&encode_session_vaapi->bitstreams_mutex);
  g_mutex_clear (&encode_session_vaapi->pending_frames_mutex);

  g_clear_pointer (&encode_session_vaapi->bitstreams, g_hash_table_unref);
  g_clear_pointer (&encode_session_vaapi->pending_frames, g_hash_table_unref);

  G_OBJECT_CLASS (grd_encode_session_vaapi_parent_class)->finalize (object);
}

static void
grd_encode_session_vaapi_init (GrdEncodeSessionVaapi *encode_session_vaapi)
{
  if (grd_get_debug_flags () & GRD_DEBUG_VA_TIMES)
    encode_session_vaapi->debug_va_times = TRUE;

  encode_session_vaapi->va_config = VA_INVALID_ID;
  encode_session_vaapi->va_context = VA_INVALID_ID;
  encode_session_vaapi->pending_idr_frame = TRUE;

  encode_session_vaapi->surfaces =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) nv12_vkva_surface_free);
  encode_session_vaapi->pending_frames = g_hash_table_new (NULL, NULL);
  encode_session_vaapi->bitstreams = g_hash_table_new (NULL, NULL);

  g_mutex_init (&encode_session_vaapi->pending_frames_mutex);
  g_mutex_init (&encode_session_vaapi->bitstreams_mutex);
}

static void
grd_encode_session_vaapi_class_init (GrdEncodeSessionVaapiClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdEncodeSessionClass *encode_session_class =
    GRD_ENCODE_SESSION_CLASS (klass);

  object_class->dispose = grd_encode_session_vaapi_dispose;
  object_class->finalize = grd_encode_session_vaapi_finalize;

  encode_session_class->get_surface_size =
    grd_encode_session_vaapi_get_surface_size;
  encode_session_class->get_image_views =
    grd_encode_session_vaapi_get_image_views;
  encode_session_class->has_pending_frames =
    grd_encode_session_vaapi_has_pending_frames;
  encode_session_class->encode_frame =
    grd_encode_session_vaapi_encode_frame;
  encode_session_class->lock_bitstream =
    grd_encode_session_vaapi_lock_bitstream;
  encode_session_class->unlock_bitstream =
    grd_encode_session_vaapi_unlock_bitstream;
}
