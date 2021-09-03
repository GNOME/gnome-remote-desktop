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

#include "config.h"

#include "grd-rdp-nvenc.h"

#include <ffnvcodec/dynlink_loader.h>

typedef struct _NvEncEncodeSession
{
  void *encoder;

  uint16_t enc_width;
  uint16_t enc_height;

  NV_ENC_OUTPUT_PTR buffer_out;
} NvEncEncodeSession;

struct _GrdRdpNvenc
{
  GObject parent;

  CudaFunctions *cuda_funcs;
  NvencFunctions *nvenc_funcs;
  NV_ENCODE_API_FUNCTION_LIST nvenc_api;

  CUdevice cu_device;
  CUcontext cu_context;
  gboolean initialized;

  CUmodule cu_module_avc_utils;
  CUfunction cu_bgrx_to_yuv420;

  GHashTable *encode_sessions;

  uint32_t next_encode_session_id;
};

G_DEFINE_TYPE (GrdRdpNvenc, grd_rdp_nvenc, G_TYPE_OBJECT);

static uint32_t
get_next_free_encode_session_id (GrdRdpNvenc *rdp_nvenc)
{
  uint32_t encode_session_id = rdp_nvenc->next_encode_session_id;

  while (g_hash_table_contains (rdp_nvenc->encode_sessions,
                                GUINT_TO_POINTER (encode_session_id)))
    ++encode_session_id;

  rdp_nvenc->next_encode_session_id = encode_session_id + 1;

  return encode_session_id;
}

gboolean
grd_rdp_nvenc_create_encode_session (GrdRdpNvenc *rdp_nvenc,
                                     uint32_t    *encode_session_id,
                                     uint16_t     surface_width,
                                     uint16_t     surface_height,
                                     uint16_t     refresh_rate)
{
  NvEncEncodeSession *encode_session;
  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_params = {0};
  NV_ENC_INITIALIZE_PARAMS init_params = {0};
  NV_ENC_CONFIG encode_config = {0};
  NV_ENC_CREATE_BITSTREAM_BUFFER create_bitstream_buffer = {0};
  uint16_t aligned_width;
  uint16_t aligned_height;

  aligned_width = surface_width + (surface_width % 16 ? 16 - surface_width % 16 : 0);
  aligned_height = surface_height + (surface_height % 64 ? 64 - surface_height % 64 : 0);

  *encode_session_id = get_next_free_encode_session_id (rdp_nvenc);
  encode_session = g_malloc0 (sizeof (NvEncEncodeSession));
  encode_session->enc_width = aligned_width;
  encode_session->enc_height = aligned_height;

  open_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  open_params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  open_params.device = rdp_nvenc->cu_context;
  open_params.apiVersion = NVENCAPI_VERSION;

  if (rdp_nvenc->nvenc_api.nvEncOpenEncodeSessionEx (
        &open_params, &encode_session->encoder) != NV_ENC_SUCCESS)
    {
      g_debug ("[HWAccel.NVENC] Failed to open encode session");
      g_free (encode_session);
      return FALSE;
    }

  encode_config.version = NV_ENC_CONFIG_VER;
  encode_config.profileGUID = NV_ENC_H264_PROFILE_PROGRESSIVE_HIGH_GUID;
  encode_config.gopLength = NVENC_INFINITE_GOPLENGTH;
  encode_config.frameIntervalP = 1;
  encode_config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_MBAFF;
  encode_config.mvPrecision = NV_ENC_MV_PRECISION_QUARTER_PEL;
  encode_config.rcParams.version = NV_ENC_RC_PARAMS_VER;
  encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
  encode_config.rcParams.averageBitRate = 0;
  encode_config.rcParams.maxBitRate = 0;
  encode_config.rcParams.targetQuality = 22;
  encode_config.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
  encode_config.encodeCodecConfig.h264Config.chromaFormatIDC = 1;

  init_params.version = NV_ENC_INITIALIZE_PARAMS_VER;
  init_params.encodeGUID = NV_ENC_CODEC_H264_GUID;
  init_params.encodeWidth = aligned_width;
  init_params.encodeHeight = aligned_height;
  init_params.darWidth = surface_width;
  init_params.darHeight = surface_height;
  init_params.frameRateNum = refresh_rate;
  init_params.frameRateDen = 1;
  init_params.enablePTD = 1;
  init_params.encodeConfig = &encode_config;
  if (rdp_nvenc->nvenc_api.nvEncInitializeEncoder (
        encode_session->encoder, &init_params) != NV_ENC_SUCCESS)
    {
      NV_ENC_PIC_PARAMS pic_params = {0};

      g_warning ("[HWAccel.NVENC] Failed to initialize encoder");
      pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
      rdp_nvenc->nvenc_api.nvEncEncodePicture (encode_session->encoder,
                                               &pic_params);
      rdp_nvenc->nvenc_api.nvEncDestroyEncoder (encode_session->encoder);

      g_free (encode_session);
      return FALSE;
    }

  create_bitstream_buffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
  if (rdp_nvenc->nvenc_api.nvEncCreateBitstreamBuffer (
        encode_session->encoder, &create_bitstream_buffer) != NV_ENC_SUCCESS)
    {
      NV_ENC_PIC_PARAMS pic_params = {0};

      g_warning ("[HWAccel.NVENC] Failed to create bitstream buffer");
      pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
      rdp_nvenc->nvenc_api.nvEncEncodePicture (encode_session->encoder,
                                               &pic_params);
      rdp_nvenc->nvenc_api.nvEncDestroyEncoder (encode_session->encoder);

      g_free (encode_session);
      return FALSE;
    }
  encode_session->buffer_out = create_bitstream_buffer.bitstreamBuffer;

  g_hash_table_insert (rdp_nvenc->encode_sessions,
                       GUINT_TO_POINTER (*encode_session_id),
                       encode_session);

  return TRUE;
}

void
grd_rdp_nvenc_free_encode_session (GrdRdpNvenc *rdp_nvenc,
                                   uint32_t     encode_session_id)
{
  NvEncEncodeSession *encode_session;
  NV_ENC_PIC_PARAMS pic_params = {0};

  if (!g_hash_table_steal_extended (rdp_nvenc->encode_sessions,
                                    GUINT_TO_POINTER (encode_session_id),
                                    NULL, (gpointer *) &encode_session))
    return;

  rdp_nvenc->nvenc_api.nvEncDestroyBitstreamBuffer (encode_session->encoder,
                                                    encode_session->buffer_out);

  pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
  rdp_nvenc->nvenc_api.nvEncEncodePicture (encode_session->encoder,
                                           &pic_params);
  rdp_nvenc->nvenc_api.nvEncDestroyEncoder (encode_session->encoder);

  g_free (encode_session);
}

gboolean
grd_rdp_nvenc_avc420_encode_bgrx_frame (GrdRdpNvenc  *rdp_nvenc,
                                        uint32_t      encode_session_id,
                                        uint8_t      *src_data,
                                        uint16_t      src_width,
                                        uint16_t      src_height,
                                        uint16_t      aligned_width,
                                        uint16_t      aligned_height,
                                        uint8_t     **bitstream,
                                        uint32_t     *bitstream_size)
{
  NvEncEncodeSession *encode_session;
  CUDA_MEMCPY2D cu_memcpy_2d = {0};
  NV_ENC_REGISTER_RESOURCE register_res = {0};
  NV_ENC_MAP_INPUT_RESOURCE map_input_res = {0};
  NV_ENC_PIC_PARAMS pic_params = {0};
  NV_ENC_LOCK_BITSTREAM lock_bitstream = {0};
  CUstream cu_stream = NULL;
  CUdeviceptr bgrx_buffer = 0, nv12_buffer = 0;
  size_t bgrx_pitch = 0, nv12_pitch = 0;
  unsigned int grid_dim_x, grid_dim_y, grid_dim_z;
  unsigned int block_dim_x, block_dim_y, block_dim_z;
  void *args[8];

  if (!g_hash_table_lookup_extended (rdp_nvenc->encode_sessions,
                                     GUINT_TO_POINTER (encode_session_id),
                                     NULL, (gpointer *) &encode_session))
    return FALSE;

  g_assert (encode_session->enc_width == aligned_width);
  g_assert (encode_session->enc_height == aligned_height);

  if (rdp_nvenc->cuda_funcs->cuStreamCreate (&cu_stream, 0) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to create stream");
      return FALSE;
    }

  if (rdp_nvenc->cuda_funcs->cuMemAllocPitch (
        &bgrx_buffer, &bgrx_pitch, src_width * 4, src_height, 4) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to allocate BGRX buffer");
      rdp_nvenc->cuda_funcs->cuStreamDestroy (cu_stream);
      return FALSE;
    }

  cu_memcpy_2d.srcMemoryType = CU_MEMORYTYPE_HOST;
  cu_memcpy_2d.srcHost = src_data;
  cu_memcpy_2d.srcPitch = src_width * 4;

  cu_memcpy_2d.dstMemoryType = CU_MEMORYTYPE_DEVICE;
  cu_memcpy_2d.dstDevice = bgrx_buffer;
  cu_memcpy_2d.dstPitch = bgrx_pitch;

  cu_memcpy_2d.WidthInBytes = src_width * 4;
  cu_memcpy_2d.Height = src_height;

  if (rdp_nvenc->cuda_funcs->cuMemcpy2DAsync (
        &cu_memcpy_2d, cu_stream) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to initiate H2D copy");
      rdp_nvenc->cuda_funcs->cuMemFree (bgrx_buffer);
      rdp_nvenc->cuda_funcs->cuStreamDestroy (cu_stream);
      return FALSE;
    }

  if (rdp_nvenc->cuda_funcs->cuMemAllocPitch (
        &nv12_buffer, &nv12_pitch,
        aligned_width, aligned_height + aligned_height / 2, 4) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to allocate NV12 buffer");
      rdp_nvenc->cuda_funcs->cuStreamSynchronize (cu_stream);
      rdp_nvenc->cuda_funcs->cuMemFree (bgrx_buffer);
      rdp_nvenc->cuda_funcs->cuStreamDestroy (cu_stream);
      return FALSE;
    }

  /* Threads per blocks */
  block_dim_x = 32;
  block_dim_y = 8;
  block_dim_z = 1;
  /* Amount of blocks per grid */
  grid_dim_x = aligned_width / 2 / block_dim_x +
               (aligned_width / 2 % block_dim_x ? 1 : 0);
  grid_dim_y = aligned_height / 2 / block_dim_y +
               (aligned_height / 2 % block_dim_y ? 1 : 0);
  grid_dim_z = 1;

  args[0] = &nv12_buffer;
  args[1] = &bgrx_buffer;
  args[2] = &src_width;
  args[3] = &src_height;
  args[4] = &bgrx_pitch;
  args[5] = &aligned_width;
  args[6] = &aligned_height;
  args[7] = &aligned_width;

  if (rdp_nvenc->cuda_funcs->cuLaunchKernel (
        rdp_nvenc->cu_bgrx_to_yuv420, grid_dim_x, grid_dim_y, grid_dim_z,
        block_dim_x, block_dim_y, block_dim_z, 0, cu_stream, args, NULL) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to launch BGRX_TO_YUV420 kernel");
      rdp_nvenc->cuda_funcs->cuStreamSynchronize (cu_stream);
      rdp_nvenc->cuda_funcs->cuMemFree (nv12_buffer);
      rdp_nvenc->cuda_funcs->cuMemFree (bgrx_buffer);
      rdp_nvenc->cuda_funcs->cuStreamDestroy (cu_stream);
      return FALSE;
    }

  if (rdp_nvenc->cuda_funcs->cuStreamSynchronize (cu_stream) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to synchronize stream");
      rdp_nvenc->cuda_funcs->cuMemFree (nv12_buffer);
      rdp_nvenc->cuda_funcs->cuMemFree (bgrx_buffer);
      rdp_nvenc->cuda_funcs->cuStreamDestroy (cu_stream);
      return FALSE;
    }

  rdp_nvenc->cuda_funcs->cuStreamDestroy (cu_stream);
  rdp_nvenc->cuda_funcs->cuMemFree (bgrx_buffer);

  register_res.version = NV_ENC_REGISTER_RESOURCE_VER;
  register_res.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
  register_res.width = aligned_width;
  register_res.height = aligned_height;
  register_res.pitch = aligned_width;
  register_res.resourceToRegister = (void *) nv12_buffer;
  register_res.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
  register_res.bufferUsage = NV_ENC_INPUT_IMAGE;

  if (rdp_nvenc->nvenc_api.nvEncRegisterResource (
        encode_session->encoder, &register_res) != NV_ENC_SUCCESS)
    {
      g_warning ("[HWAccel.NVENC] Failed to register resource");
      rdp_nvenc->cuda_funcs->cuMemFree (nv12_buffer);
      return FALSE;
    }

  map_input_res.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
  map_input_res.registeredResource = register_res.registeredResource;

  if (rdp_nvenc->nvenc_api.nvEncMapInputResource (
        encode_session->encoder, &map_input_res) != NV_ENC_SUCCESS)
    {
      g_warning ("[HWAccel.NVENC] Failed to map input resource");
      rdp_nvenc->nvenc_api.nvEncUnregisterResource (encode_session->encoder,
                                                    register_res.registeredResource);
      rdp_nvenc->cuda_funcs->cuMemFree (nv12_buffer);
      return FALSE;
    }

  pic_params.version = NV_ENC_PIC_PARAMS_VER;
  pic_params.inputWidth = aligned_width;
  pic_params.inputHeight = aligned_height;
  pic_params.inputPitch = aligned_width;
  pic_params.inputBuffer = map_input_res.mappedResource;
  pic_params.outputBitstream = encode_session->buffer_out;
  pic_params.bufferFmt = map_input_res.mappedBufferFmt;
  pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

  if (rdp_nvenc->nvenc_api.nvEncEncodePicture (
        encode_session->encoder, &pic_params) != NV_ENC_SUCCESS)
    {
      g_warning ("[HWAccel.NVENC] Failed to encode frame");
      rdp_nvenc->nvenc_api.nvEncUnmapInputResource (encode_session->encoder,
                                                    map_input_res.mappedResource);
      rdp_nvenc->nvenc_api.nvEncUnregisterResource (encode_session->encoder,
                                                    register_res.registeredResource);
      rdp_nvenc->cuda_funcs->cuMemFree (nv12_buffer);
      return FALSE;
    }

  lock_bitstream.version = NV_ENC_LOCK_BITSTREAM_VER;
  lock_bitstream.outputBitstream = encode_session->buffer_out;

  if (rdp_nvenc->nvenc_api.nvEncLockBitstream (
        encode_session->encoder, &lock_bitstream) != NV_ENC_SUCCESS)
    {
      g_warning ("[HWAccel.NVENC] Failed to lock bitstream");
      rdp_nvenc->nvenc_api.nvEncUnmapInputResource (encode_session->encoder,
                                                    map_input_res.mappedResource);
      rdp_nvenc->nvenc_api.nvEncUnregisterResource (encode_session->encoder,
                                                    register_res.registeredResource);
      rdp_nvenc->cuda_funcs->cuMemFree (nv12_buffer);
      return FALSE;
    }

  *bitstream_size = lock_bitstream.bitstreamSizeInBytes;
  *bitstream = g_memdup2 (lock_bitstream.bitstreamBufferPtr, *bitstream_size);

  rdp_nvenc->nvenc_api.nvEncUnlockBitstream (encode_session->encoder,
                                             lock_bitstream.outputBitstream);

  rdp_nvenc->nvenc_api.nvEncUnmapInputResource (encode_session->encoder,
                                                map_input_res.mappedResource);
  rdp_nvenc->nvenc_api.nvEncUnregisterResource (encode_session->encoder,
                                                register_res.registeredResource);
  rdp_nvenc->cuda_funcs->cuMemFree (nv12_buffer);

  return TRUE;
}

GrdRdpNvenc *
grd_rdp_nvenc_new (void)
{
  GrdRdpNvenc *rdp_nvenc;
  gboolean nvenc_device_found = FALSE;
  CUdevice cu_device = 0;
  int cu_device_count = 0;
  g_autofree char *avc_ptx_path = NULL;
  g_autofree char *avc_ptx_instructions = NULL;
  g_autoptr (GError) error = NULL;
  int i;

  rdp_nvenc = g_object_new (GRD_TYPE_RDP_NVENC, NULL);
  cuda_load_functions (&rdp_nvenc->cuda_funcs, NULL);
  nvenc_load_functions (&rdp_nvenc->nvenc_funcs, NULL);

  if (!rdp_nvenc->cuda_funcs || !rdp_nvenc->nvenc_funcs)
    {
      g_debug ("[HWAccel.CUDA] Failed to load CUDA or NVENC library");
      g_clear_object (&rdp_nvenc);
      return NULL;
    }

  rdp_nvenc->cuda_funcs->cuInit (0);
  rdp_nvenc->cuda_funcs->cuDeviceGetCount (&cu_device_count);

  g_debug ("[HWAccel.CUDA] Found %i CUDA devices", cu_device_count);
  for (i = 0; i < cu_device_count; ++i)
    {
      int cc_major = 0, cc_minor = 0;

      rdp_nvenc->cuda_funcs->cuDeviceGet (&cu_device, i);
      rdp_nvenc->cuda_funcs->cuDeviceComputeCapability (&cc_major, &cc_minor,
                                                        cu_device);

      g_debug ("[HWAccel.CUDA] Device %i compute capability: [%i, %i]",
               i, cc_major, cc_minor);
      if (cc_major >= 3)
        {
          g_debug ("[HWAccel.NVENC] Choosing CUDA device with id %i", i);
          nvenc_device_found = TRUE;
          break;
        }
    }

  if (!cu_device_count || !nvenc_device_found)
    {
      g_debug ("[HWAccel.NVENC] No NVENC capable gpu found");
      g_clear_object (&rdp_nvenc);
      return NULL;
    }

  rdp_nvenc->cu_device = cu_device;
  if (rdp_nvenc->cuda_funcs->cuDevicePrimaryCtxRetain (
        &rdp_nvenc->cu_context, rdp_nvenc->cu_device) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to retain CUDA context");
      g_clear_object (&rdp_nvenc);
      return NULL;
    }

  rdp_nvenc->nvenc_api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  if (rdp_nvenc->nvenc_funcs->NvEncodeAPICreateInstance (&rdp_nvenc->nvenc_api) != NV_ENC_SUCCESS)
    {
      g_warning ("[HWAccel.NVENC] Could not create NVENC API instance");

      rdp_nvenc->cuda_funcs->cuDevicePrimaryCtxRelease (rdp_nvenc->cu_device);
      g_clear_object (&rdp_nvenc);

      return NULL;
    }

  if (rdp_nvenc->cuda_funcs->cuCtxPushCurrent (rdp_nvenc->cu_context) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to push CUDA context");
      rdp_nvenc->cuda_funcs->cuDevicePrimaryCtxRelease (rdp_nvenc->cu_device);
      g_clear_object (&rdp_nvenc);
      return NULL;
    }

  rdp_nvenc->initialized = TRUE;

  avc_ptx_path = g_strdup_printf ("%s/grd-cuda-avc-utils_30.ptx", GRD_DATA_DIR);
  if (!g_file_get_contents (avc_ptx_path, &avc_ptx_instructions, NULL, &error))
    g_error ("[HWAccel.CUDA] Failed to read PTX instructions: %s", error->message);

  if (rdp_nvenc->cuda_funcs->cuModuleLoadData (
        &rdp_nvenc->cu_module_avc_utils, avc_ptx_instructions) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to load CUDA module");
      g_clear_object (&rdp_nvenc);
      return NULL;
    }

  if (rdp_nvenc->cuda_funcs->cuModuleGetFunction (
        &rdp_nvenc->cu_bgrx_to_yuv420, rdp_nvenc->cu_module_avc_utils,
        "convert_2x2_bgrx_area_to_yuv420_nv12") != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to get AVC CUDA kernel");
      g_clear_object (&rdp_nvenc);
      return NULL;
    }

  return rdp_nvenc;
}

static void
grd_rdp_nvenc_dispose (GObject *object)
{
  GrdRdpNvenc *rdp_nvenc = GRD_RDP_NVENC (object);

  if (rdp_nvenc->initialized)
    {
      rdp_nvenc->cuda_funcs->cuCtxPopCurrent (&rdp_nvenc->cu_context);
      rdp_nvenc->cuda_funcs->cuDevicePrimaryCtxRelease (rdp_nvenc->cu_device);

      rdp_nvenc->initialized = FALSE;
    }

  g_clear_pointer (&rdp_nvenc->cu_module_avc_utils,
                   rdp_nvenc->cuda_funcs->cuModuleUnload);

  nvenc_free_functions (&rdp_nvenc->nvenc_funcs);
  cuda_free_functions (&rdp_nvenc->cuda_funcs);

  g_assert (g_hash_table_size (rdp_nvenc->encode_sessions) == 0);
  g_clear_pointer (&rdp_nvenc->encode_sessions, g_hash_table_destroy);

  G_OBJECT_CLASS (grd_rdp_nvenc_parent_class)->dispose (object);
}

static void
grd_rdp_nvenc_init (GrdRdpNvenc *rdp_nvenc)
{
  rdp_nvenc->encode_sessions = g_hash_table_new (NULL, NULL);
}

static void
grd_rdp_nvenc_class_init (GrdRdpNvencClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_nvenc_dispose;
}
