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

#include "grd-hwaccel-nvidia.h"

#include <ffnvcodec/dynlink_loader.h>

#include "grd-egl-thread.h"
#include "grd-utils.h"

#define MAX_CUDA_DEVICES_FOR_RETRIEVAL 32

typedef CUresult CUDAAPI tcuGraphicsGLRegisterBuffer (CUgraphicsResource *resource,
                                                      GLuint              buffer,
                                                      unsigned int        flags);
typedef CUresult CUDAAPI tcuGraphicsResourceGetMappedPointer_v2 (CUdeviceptr        *dev_ptr,
                                                                 size_t             *size,
                                                                 CUgraphicsResource  resource);

typedef struct _ExtraCudaFunctions
{
  tcuGraphicsGLRegisterBuffer *cuGraphicsGLRegisterBuffer;
  tcuGraphicsResourceGetMappedPointer_v2 *cuGraphicsResourceGetMappedPointer;
} ExtraCudaFunctions;

typedef struct _DevRetrievalData
{
  GrdHwAccelNvidia *hwaccel_nvidia;
  GrdSyncPoint sync_point;

  unsigned int n_devices;
  CUdevice *devices;
} DevRetrievalData;

typedef struct _NvEncEncodeSession
{
  void *encoder;

  uint16_t enc_width;
  uint16_t enc_height;

  NV_ENC_OUTPUT_PTR buffer_out;
} NvEncEncodeSession;

struct _GrdHwAccelNvidia
{
  GObject parent;

  GrdEglThread *egl_thread;

  CudaFunctions *cuda_funcs;
  NvencFunctions *nvenc_funcs;
  NV_ENCODE_API_FUNCTION_LIST nvenc_api;

  void *cuda_lib;
  ExtraCudaFunctions *extra_cuda_funcs;

  CUdevice cu_device;
  CUcontext cu_context;
  gboolean initialized;

  CUmodule cu_module_dmg_utils;
  CUfunction cu_chk_dmg_pxl;
  CUfunction cu_cmb_dmg_arr_cols;
  CUfunction cu_cmb_dmg_arr_rows;
  CUfunction cu_simplify_dmg_arr;

  CUmodule cu_module_avc_utils;
  CUfunction cu_bgrx_to_yuv420;

  GHashTable *encode_sessions;

  uint32_t next_encode_session_id;
};

G_DEFINE_TYPE (GrdHwAccelNvidia, grd_hwaccel_nvidia, G_TYPE_OBJECT)

void
grd_hwaccel_nvidia_push_cuda_context (GrdHwAccelNvidia *hwaccel_nvidia)
{
  CudaFunctions *cuda_funcs = hwaccel_nvidia->cuda_funcs;

  if (cuda_funcs->cuCtxPushCurrent (hwaccel_nvidia->cu_context) != CUDA_SUCCESS)
    g_error ("[HWAccel.CUDA] Failed to push CUDA context");
}

void
grd_hwaccel_nvidia_pop_cuda_context (GrdHwAccelNvidia *hwaccel_nvidia)
{
  CUcontext cu_context;

  hwaccel_nvidia->cuda_funcs->cuCtxPopCurrent (&cu_context);
}

gboolean
grd_hwaccel_nvidia_register_read_only_gl_buffer (GrdHwAccelNvidia   *hwaccel_nvidia,
                                                 CUgraphicsResource *cuda_resource,
                                                 uint32_t            buffer)
{
  ExtraCudaFunctions *extra_cuda_funcs = hwaccel_nvidia->extra_cuda_funcs;

  if (extra_cuda_funcs->cuGraphicsGLRegisterBuffer (
        cuda_resource, buffer,
        CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to register GL buffer");
      return FALSE;
    }

  return TRUE;
}

void
grd_hwaccel_nvidia_unregister_cuda_resource (GrdHwAccelNvidia   *hwaccel_nvidia,
                                             CUgraphicsResource  cuda_resource,
                                             CUstream            cuda_stream)
{
  hwaccel_nvidia->cuda_funcs->cuStreamSynchronize (cuda_stream);
  hwaccel_nvidia->cuda_funcs->cuGraphicsUnregisterResource (cuda_resource);
}

gboolean
grd_hwaccel_nvidia_map_cuda_resource (GrdHwAccelNvidia   *hwaccel_nvidia,
                                      CUgraphicsResource  cuda_resource,
                                      CUdeviceptr        *dev_ptr,
                                      size_t             *size,
                                      CUstream            cuda_stream)
{
  CudaFunctions *cuda_funcs = hwaccel_nvidia->cuda_funcs;
  ExtraCudaFunctions *extra_cuda_funcs = hwaccel_nvidia->extra_cuda_funcs;

  if (cuda_funcs->cuGraphicsMapResources (1, &cuda_resource,
                                          cuda_stream) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to map resources");
      return FALSE;
    }
  if (extra_cuda_funcs->cuGraphicsResourceGetMappedPointer (dev_ptr, size,
                                                            cuda_resource) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to get mapped pointer");
      cuda_funcs->cuGraphicsUnmapResources (1, &cuda_resource, cuda_stream);
      return FALSE;
    }

  return TRUE;
}

void
grd_hwaccel_nvidia_unmap_cuda_resource (GrdHwAccelNvidia   *hwaccel_nvidia,
                                        CUgraphicsResource  cuda_resource,
                                        CUstream            cuda_stream)
{
  hwaccel_nvidia->cuda_funcs->cuGraphicsUnmapResources (1, &cuda_resource,
                                                        cuda_stream);
}

gboolean
grd_hwaccel_nvidia_create_cuda_stream (GrdHwAccelNvidia *hwaccel_nvidia,
                                       CUstream         *cuda_stream)
{
  CudaFunctions *cuda_funcs = hwaccel_nvidia->cuda_funcs;

  if (cuda_funcs->cuStreamCreate (cuda_stream,
                                  CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to create stream");
      return FALSE;
    }

  return TRUE;
}

void
grd_hwaccel_nvidia_destroy_cuda_stream (GrdHwAccelNvidia *hwaccel_nvidia,
                                        CUstream          cuda_stream)
{
  hwaccel_nvidia->cuda_funcs->cuStreamSynchronize (cuda_stream);
  hwaccel_nvidia->cuda_funcs->cuStreamDestroy (cuda_stream);
}

static uint32_t
get_next_free_encode_session_id (GrdHwAccelNvidia *hwaccel_nvidia)
{
  uint32_t encode_session_id = hwaccel_nvidia->next_encode_session_id;

  while (g_hash_table_contains (hwaccel_nvidia->encode_sessions,
                                GUINT_TO_POINTER (encode_session_id)))
    ++encode_session_id;

  hwaccel_nvidia->next_encode_session_id = encode_session_id + 1;

  return encode_session_id;
}

gboolean
grd_hwaccel_nvidia_create_nvenc_session (GrdHwAccelNvidia *hwaccel_nvidia,
                                         uint32_t         *encode_session_id,
                                         uint16_t          surface_width,
                                         uint16_t          surface_height,
                                         uint16_t          refresh_rate)
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

  *encode_session_id = get_next_free_encode_session_id (hwaccel_nvidia);
  encode_session = g_malloc0 (sizeof (NvEncEncodeSession));
  encode_session->enc_width = aligned_width;
  encode_session->enc_height = aligned_height;

  open_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  open_params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  open_params.device = hwaccel_nvidia->cu_context;
  open_params.apiVersion = NVENCAPI_VERSION;

  if (hwaccel_nvidia->nvenc_api.nvEncOpenEncodeSessionEx (
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
  if (hwaccel_nvidia->nvenc_api.nvEncInitializeEncoder (
        encode_session->encoder, &init_params) != NV_ENC_SUCCESS)
    {
      NV_ENC_PIC_PARAMS pic_params = {0};

      g_warning ("[HWAccel.NVENC] Failed to initialize encoder");
      pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
      hwaccel_nvidia->nvenc_api.nvEncEncodePicture (encode_session->encoder,
                                                    &pic_params);
      hwaccel_nvidia->nvenc_api.nvEncDestroyEncoder (encode_session->encoder);

      g_free (encode_session);
      return FALSE;
    }

  create_bitstream_buffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
  if (hwaccel_nvidia->nvenc_api.nvEncCreateBitstreamBuffer (
        encode_session->encoder, &create_bitstream_buffer) != NV_ENC_SUCCESS)
    {
      NV_ENC_PIC_PARAMS pic_params = {0};

      g_warning ("[HWAccel.NVENC] Failed to create bitstream buffer");
      pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
      hwaccel_nvidia->nvenc_api.nvEncEncodePicture (encode_session->encoder,
                                                    &pic_params);
      hwaccel_nvidia->nvenc_api.nvEncDestroyEncoder (encode_session->encoder);

      g_free (encode_session);
      return FALSE;
    }
  encode_session->buffer_out = create_bitstream_buffer.bitstreamBuffer;

  g_hash_table_insert (hwaccel_nvidia->encode_sessions,
                       GUINT_TO_POINTER (*encode_session_id),
                       encode_session);

  return TRUE;
}

void
grd_hwaccel_nvidia_free_nvenc_session (GrdHwAccelNvidia *hwaccel_nvidia,
                                       uint32_t          encode_session_id)
{
  NvEncEncodeSession *encode_session;
  NV_ENC_PIC_PARAMS pic_params = {0};

  if (!g_hash_table_steal_extended (hwaccel_nvidia->encode_sessions,
                                    GUINT_TO_POINTER (encode_session_id),
                                    NULL, (gpointer *) &encode_session))
    return;

  hwaccel_nvidia->nvenc_api.nvEncDestroyBitstreamBuffer (encode_session->encoder,
                                                         encode_session->buffer_out);

  pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
  hwaccel_nvidia->nvenc_api.nvEncEncodePicture (encode_session->encoder,
                                                &pic_params);
  hwaccel_nvidia->nvenc_api.nvEncDestroyEncoder (encode_session->encoder);

  g_free (encode_session);
}

gboolean
grd_hwaccel_nvidia_avc420_encode_bgrx_frame (GrdHwAccelNvidia  *hwaccel_nvidia,
                                             uint32_t           encode_session_id,
                                             uint8_t           *src_data,
                                             uint16_t           src_width,
                                             uint16_t           src_height,
                                             uint16_t           aligned_width,
                                             uint16_t           aligned_height,
                                             uint8_t          **bitstream,
                                             uint32_t          *bitstream_size)
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

  if (!g_hash_table_lookup_extended (hwaccel_nvidia->encode_sessions,
                                     GUINT_TO_POINTER (encode_session_id),
                                     NULL, (gpointer *) &encode_session))
    return FALSE;

  g_assert (encode_session->enc_width == aligned_width);
  g_assert (encode_session->enc_height == aligned_height);

  if (hwaccel_nvidia->cuda_funcs->cuStreamCreate (&cu_stream, 0) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to create stream");
      return FALSE;
    }

  if (hwaccel_nvidia->cuda_funcs->cuMemAllocPitch (
        &bgrx_buffer, &bgrx_pitch, src_width * 4, src_height, 4) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to allocate BGRX buffer");
      hwaccel_nvidia->cuda_funcs->cuStreamDestroy (cu_stream);
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

  if (hwaccel_nvidia->cuda_funcs->cuMemcpy2DAsync (
        &cu_memcpy_2d, cu_stream) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to initiate H2D copy");
      hwaccel_nvidia->cuda_funcs->cuMemFree (bgrx_buffer);
      hwaccel_nvidia->cuda_funcs->cuStreamDestroy (cu_stream);
      return FALSE;
    }

  if (hwaccel_nvidia->cuda_funcs->cuMemAllocPitch (
        &nv12_buffer, &nv12_pitch,
        aligned_width, aligned_height + aligned_height / 2, 4) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to allocate NV12 buffer");
      hwaccel_nvidia->cuda_funcs->cuStreamSynchronize (cu_stream);
      hwaccel_nvidia->cuda_funcs->cuMemFree (bgrx_buffer);
      hwaccel_nvidia->cuda_funcs->cuStreamDestroy (cu_stream);
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

  if (hwaccel_nvidia->cuda_funcs->cuLaunchKernel (
        hwaccel_nvidia->cu_bgrx_to_yuv420, grid_dim_x, grid_dim_y, grid_dim_z,
        block_dim_x, block_dim_y, block_dim_z, 0, cu_stream, args, NULL) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to launch BGRX_TO_YUV420 kernel");
      hwaccel_nvidia->cuda_funcs->cuStreamSynchronize (cu_stream);
      hwaccel_nvidia->cuda_funcs->cuMemFree (nv12_buffer);
      hwaccel_nvidia->cuda_funcs->cuMemFree (bgrx_buffer);
      hwaccel_nvidia->cuda_funcs->cuStreamDestroy (cu_stream);
      return FALSE;
    }

  if (hwaccel_nvidia->cuda_funcs->cuStreamSynchronize (cu_stream) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to synchronize stream");
      hwaccel_nvidia->cuda_funcs->cuMemFree (nv12_buffer);
      hwaccel_nvidia->cuda_funcs->cuMemFree (bgrx_buffer);
      hwaccel_nvidia->cuda_funcs->cuStreamDestroy (cu_stream);
      return FALSE;
    }

  hwaccel_nvidia->cuda_funcs->cuStreamDestroy (cu_stream);
  hwaccel_nvidia->cuda_funcs->cuMemFree (bgrx_buffer);

  register_res.version = NV_ENC_REGISTER_RESOURCE_VER;
  register_res.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
  register_res.width = aligned_width;
  register_res.height = aligned_height;
  register_res.pitch = aligned_width;
  register_res.resourceToRegister = (void *) nv12_buffer;
  register_res.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
  register_res.bufferUsage = NV_ENC_INPUT_IMAGE;

  if (hwaccel_nvidia->nvenc_api.nvEncRegisterResource (
        encode_session->encoder, &register_res) != NV_ENC_SUCCESS)
    {
      g_warning ("[HWAccel.NVENC] Failed to register resource");
      hwaccel_nvidia->cuda_funcs->cuMemFree (nv12_buffer);
      return FALSE;
    }

  map_input_res.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
  map_input_res.registeredResource = register_res.registeredResource;

  if (hwaccel_nvidia->nvenc_api.nvEncMapInputResource (
        encode_session->encoder, &map_input_res) != NV_ENC_SUCCESS)
    {
      g_warning ("[HWAccel.NVENC] Failed to map input resource");
      hwaccel_nvidia->nvenc_api.nvEncUnregisterResource (encode_session->encoder,
                                                         register_res.registeredResource);
      hwaccel_nvidia->cuda_funcs->cuMemFree (nv12_buffer);
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

  if (hwaccel_nvidia->nvenc_api.nvEncEncodePicture (
        encode_session->encoder, &pic_params) != NV_ENC_SUCCESS)
    {
      g_warning ("[HWAccel.NVENC] Failed to encode frame");
      hwaccel_nvidia->nvenc_api.nvEncUnmapInputResource (encode_session->encoder,
                                                         map_input_res.mappedResource);
      hwaccel_nvidia->nvenc_api.nvEncUnregisterResource (encode_session->encoder,
                                                         register_res.registeredResource);
      hwaccel_nvidia->cuda_funcs->cuMemFree (nv12_buffer);
      return FALSE;
    }

  lock_bitstream.version = NV_ENC_LOCK_BITSTREAM_VER;
  lock_bitstream.outputBitstream = encode_session->buffer_out;

  if (hwaccel_nvidia->nvenc_api.nvEncLockBitstream (
        encode_session->encoder, &lock_bitstream) != NV_ENC_SUCCESS)
    {
      g_warning ("[HWAccel.NVENC] Failed to lock bitstream");
      hwaccel_nvidia->nvenc_api.nvEncUnmapInputResource (encode_session->encoder,
                                                         map_input_res.mappedResource);
      hwaccel_nvidia->nvenc_api.nvEncUnregisterResource (encode_session->encoder,
                                                         register_res.registeredResource);
      hwaccel_nvidia->cuda_funcs->cuMemFree (nv12_buffer);
      return FALSE;
    }

  *bitstream_size = lock_bitstream.bitstreamSizeInBytes;
  *bitstream = g_memdup2 (lock_bitstream.bitstreamBufferPtr, *bitstream_size);

  hwaccel_nvidia->nvenc_api.nvEncUnlockBitstream (encode_session->encoder,
                                                  lock_bitstream.outputBitstream);

  hwaccel_nvidia->nvenc_api.nvEncUnmapInputResource (encode_session->encoder,
                                                     map_input_res.mappedResource);
  hwaccel_nvidia->nvenc_api.nvEncUnregisterResource (encode_session->encoder,
                                                     register_res.registeredResource);
  hwaccel_nvidia->cuda_funcs->cuMemFree (nv12_buffer);

  return TRUE;
}

static gboolean
load_extra_cuda_functions (GrdHwAccelNvidia *hwaccel_nvidia)
{
  ExtraCudaFunctions *extra_cuda_funcs;

  hwaccel_nvidia->cuda_lib = dlopen ("libcuda.so.1", RTLD_LAZY);
  if (!hwaccel_nvidia->cuda_lib)
    return FALSE;

  hwaccel_nvidia->extra_cuda_funcs = g_malloc0 (sizeof (ExtraCudaFunctions));

  extra_cuda_funcs = hwaccel_nvidia->extra_cuda_funcs;
  extra_cuda_funcs->cuGraphicsGLRegisterBuffer =
    dlsym (hwaccel_nvidia->cuda_lib, "cuGraphicsGLRegisterBuffer");
  if (!extra_cuda_funcs->cuGraphicsGLRegisterBuffer)
    return FALSE;

  extra_cuda_funcs->cuGraphicsResourceGetMappedPointer =
    dlsym (hwaccel_nvidia->cuda_lib, "cuGraphicsResourceGetMappedPointer_v2");
  if (!extra_cuda_funcs->cuGraphicsGLRegisterBuffer)
    return FALSE;

  return TRUE;
}

static gboolean
get_cuda_devices_in_impl (gpointer user_data)
{
  DevRetrievalData *data = user_data;
  GrdHwAccelNvidia *hwaccel_nvidia = data->hwaccel_nvidia;
  CudaFunctions *cuda_funcs = hwaccel_nvidia->cuda_funcs;

  return cuda_funcs->cuGLGetDevices (&data->n_devices, data->devices,
                                     MAX_CUDA_DEVICES_FOR_RETRIEVAL,
                                     CU_GL_DEVICE_LIST_ALL) == CUDA_SUCCESS;
}

static void
compute_devices_ready (gboolean success,
                       gpointer user_data)
{
  GrdSyncPoint *sync_point = user_data;

  grd_sync_point_complete (sync_point, success);
}

static gboolean
get_cuda_devices_from_gl_context (GrdHwAccelNvidia *hwaccel_nvidia,
                                  GrdEglThread     *egl_thread,
                                  unsigned int     *n_returned_devices,
                                  CUdevice         *device_array)
{
  DevRetrievalData data = {};
  gboolean success;

  grd_sync_point_init (&data.sync_point);
  data.hwaccel_nvidia = hwaccel_nvidia;
  data.devices = device_array;

  grd_egl_thread_run_custom_task (egl_thread,
                                  get_cuda_devices_in_impl,
                                  &data,
                                  compute_devices_ready,
                                  &data.sync_point,
                                  NULL);

  success = grd_sync_point_wait_for_completion (&data.sync_point);
  grd_sync_point_clear (&data.sync_point);

  *n_returned_devices = data.n_devices;

  return success;
}

static gboolean
push_cuda_context_in_egl_thread (gpointer user_data)
{
  GrdHwAccelNvidia *hwaccel_nvidia = user_data;

  grd_hwaccel_nvidia_push_cuda_context (hwaccel_nvidia);

  return TRUE;
}

static gboolean
pop_cuda_context_in_egl_thread (gpointer user_data)
{
  GrdHwAccelNvidia *hwaccel_nvidia = user_data;

  grd_hwaccel_nvidia_pop_cuda_context (hwaccel_nvidia);

  return TRUE;
}

static void
complete_sync (gboolean success,
               gpointer user_data)
{
  GrdSyncPoint *sync_point = user_data;

  grd_sync_point_complete (sync_point, success);
}

static void
run_function_in_egl_thread (GrdHwAccelNvidia       *hwaccel_nvidia,
                            GrdEglThreadCustomFunc  function)
{
  GrdSyncPoint sync_point = {};

  grd_sync_point_init (&sync_point);

  grd_egl_thread_run_custom_task (hwaccel_nvidia->egl_thread,
                                  function,
                                  hwaccel_nvidia,
                                  complete_sync,
                                  &sync_point,
                                  NULL);

  grd_sync_point_wait_for_completion (&sync_point);
  grd_sync_point_clear (&sync_point);
}

static gboolean
load_cuda_module (GrdHwAccelNvidia *hwaccel_nvidia,
                  CUmodule         *module,
                  const char       *name,
                  const char       *ptx_instructions)
{
  CudaFunctions *cuda_funcs = hwaccel_nvidia->cuda_funcs;

  if (cuda_funcs->cuModuleLoadData (module, ptx_instructions) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to load %s module", name);
      return FALSE;
    }

  return TRUE;
}

static gboolean
load_cuda_function (GrdHwAccelNvidia *hwaccel_nvidia,
                    CUfunction       *function,
                    CUmodule          module,
                    const char       *name)
{
  CudaFunctions *cuda_funcs = hwaccel_nvidia->cuda_funcs;

  if (cuda_funcs->cuModuleGetFunction (function, module, name) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to get kernel %s", name);
      return FALSE;
    }

  return TRUE;
}

GrdHwAccelNvidia *
grd_hwaccel_nvidia_new (GrdEglThread *egl_thread)
{
  g_autoptr (GrdHwAccelNvidia) hwaccel_nvidia = NULL;
  gboolean cuda_device_found = FALSE;
  CUdevice cu_devices[MAX_CUDA_DEVICES_FOR_RETRIEVAL] = {};
  CUdevice cu_device = 0;
  unsigned int cu_device_count = 0;
  CudaFunctions *cuda_funcs;
  NvencFunctions *nvenc_funcs;
  g_autofree char *dmg_ptx_path = NULL;
  g_autofree char *dmg_ptx_instructions = NULL;
  g_autofree char *avc_ptx_path = NULL;
  g_autofree char *avc_ptx_instructions = NULL;
  g_autoptr (GError) error = NULL;
  unsigned int i;

  hwaccel_nvidia = g_object_new (GRD_TYPE_HWACCEL_NVIDIA, NULL);
  hwaccel_nvidia->egl_thread = egl_thread;

  cuda_load_functions (&hwaccel_nvidia->cuda_funcs, NULL);
  nvenc_load_functions (&hwaccel_nvidia->nvenc_funcs, NULL);

  if (!hwaccel_nvidia->cuda_funcs || !hwaccel_nvidia->nvenc_funcs)
    {
      g_debug ("[HWAccel.CUDA] Failed to load CUDA or NVENC library");
      return NULL;
    }
  if (!load_extra_cuda_functions (hwaccel_nvidia))
    {
      g_warning ("[HWAccel.CUDA] Failed to load extra CUDA functions");
      return NULL;
    }

  cuda_funcs = hwaccel_nvidia->cuda_funcs;
  nvenc_funcs = hwaccel_nvidia->nvenc_funcs;

  if (cuda_funcs->cuInit (0) != CUDA_SUCCESS)
    {
      g_debug ("[HWAccel.CUDA] Failed to initialize CUDA");
      return NULL;
    }
  if (!get_cuda_devices_from_gl_context (hwaccel_nvidia, egl_thread,
                                         &cu_device_count, cu_devices))
    {
      g_warning ("[HWAccel.CUDA] Failed to retrieve CUDA devices");
      return NULL;
    }

  g_debug ("[HWAccel.CUDA] Retrieved %u CUDA device(s)", cu_device_count);
  for (i = 0; i < cu_device_count; ++i)
    {
      int cc_major = 0, cc_minor = 0;

      cu_device = cu_devices[i];
      cuda_funcs->cuDeviceComputeCapability (&cc_major, &cc_minor, cu_device);

      g_debug ("[HWAccel.CUDA] Device %u compute capability: [%i, %i]",
               i, cc_major, cc_minor);
      if (cc_major >= 3)
        {
          g_debug ("[HWAccel.CUDA] Choosing CUDA device with id %u", i);
          cuda_device_found = TRUE;
          break;
        }
    }

  if (!cu_device_count || !cuda_device_found)
    {
      g_debug ("[HWAccel.CUDA] No appropriate CUDA capable gpu found");
      return NULL;
    }

  hwaccel_nvidia->cu_device = cu_device;
  if (cuda_funcs->cuDevicePrimaryCtxRetain (&hwaccel_nvidia->cu_context,
                                            hwaccel_nvidia->cu_device) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to retain CUDA context");
      return NULL;
    }

  hwaccel_nvidia->nvenc_api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  if (nvenc_funcs->NvEncodeAPICreateInstance (&hwaccel_nvidia->nvenc_api) != NV_ENC_SUCCESS)
    {
      g_warning ("[HWAccel.NVENC] Could not create NVENC API instance");
      cuda_funcs->cuDevicePrimaryCtxRelease (hwaccel_nvidia->cu_device);
      return NULL;
    }

  if (cuda_funcs->cuCtxPushCurrent (hwaccel_nvidia->cu_context) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to push CUDA context");
      cuda_funcs->cuDevicePrimaryCtxRelease (hwaccel_nvidia->cu_device);
      return NULL;
    }

  run_function_in_egl_thread (hwaccel_nvidia, push_cuda_context_in_egl_thread);

  hwaccel_nvidia->initialized = TRUE;

  dmg_ptx_path = g_strdup_printf ("%s/grd-cuda-damage-utils_30.ptx", GRD_DATA_DIR);
  avc_ptx_path = g_strdup_printf ("%s/grd-cuda-avc-utils_30.ptx", GRD_DATA_DIR);

  if (!g_file_get_contents (dmg_ptx_path, &dmg_ptx_instructions, NULL, &error) ||
      !g_file_get_contents (avc_ptx_path, &avc_ptx_instructions, NULL, &error))
    g_error ("[HWAccel.CUDA] Failed to read PTX instructions: %s", error->message);

  if (!load_cuda_module (hwaccel_nvidia, &hwaccel_nvidia->cu_module_dmg_utils,
                         "damage utils", dmg_ptx_instructions))
    return NULL;

  if (!load_cuda_function (hwaccel_nvidia, &hwaccel_nvidia->cu_chk_dmg_pxl,
                           hwaccel_nvidia->cu_module_dmg_utils, "check_damaged_pixel") ||
      !load_cuda_function (hwaccel_nvidia, &hwaccel_nvidia->cu_cmb_dmg_arr_cols,
                           hwaccel_nvidia->cu_module_dmg_utils, "combine_damage_array_cols") ||
      !load_cuda_function (hwaccel_nvidia, &hwaccel_nvidia->cu_cmb_dmg_arr_rows,
                           hwaccel_nvidia->cu_module_dmg_utils, "combine_damage_array_rows") ||
      !load_cuda_function (hwaccel_nvidia, &hwaccel_nvidia->cu_simplify_dmg_arr,
                           hwaccel_nvidia->cu_module_dmg_utils, "simplify_damage_array"))
    return NULL;

  if (!load_cuda_module (hwaccel_nvidia, &hwaccel_nvidia->cu_module_avc_utils,
                         "AVC utils", avc_ptx_instructions))
    return NULL;

  if (!load_cuda_function (hwaccel_nvidia, &hwaccel_nvidia->cu_bgrx_to_yuv420,
                           hwaccel_nvidia->cu_module_avc_utils, "convert_2x2_bgrx_area_to_yuv420_nv12"))
    return NULL;

  return g_steal_pointer (&hwaccel_nvidia);
}

static void
grd_hwaccel_nvidia_dispose (GObject *object)
{
  GrdHwAccelNvidia *hwaccel_nvidia = GRD_HWACCEL_NVIDIA (object);

  if (hwaccel_nvidia->initialized)
    {
      run_function_in_egl_thread (hwaccel_nvidia, pop_cuda_context_in_egl_thread);

      hwaccel_nvidia->cuda_funcs->cuCtxPopCurrent (&hwaccel_nvidia->cu_context);
      hwaccel_nvidia->cuda_funcs->cuDevicePrimaryCtxRelease (hwaccel_nvidia->cu_device);

      hwaccel_nvidia->initialized = FALSE;
    }

  g_clear_pointer (&hwaccel_nvidia->cu_module_avc_utils,
                   hwaccel_nvidia->cuda_funcs->cuModuleUnload);
  g_clear_pointer (&hwaccel_nvidia->cu_module_dmg_utils,
                   hwaccel_nvidia->cuda_funcs->cuModuleUnload);

  g_clear_pointer (&hwaccel_nvidia->cuda_lib, dlclose);
  g_clear_pointer (&hwaccel_nvidia->extra_cuda_funcs, g_free);

  nvenc_free_functions (&hwaccel_nvidia->nvenc_funcs);
  cuda_free_functions (&hwaccel_nvidia->cuda_funcs);

  g_assert (g_hash_table_size (hwaccel_nvidia->encode_sessions) == 0);
  g_clear_pointer (&hwaccel_nvidia->encode_sessions, g_hash_table_destroy);

  G_OBJECT_CLASS (grd_hwaccel_nvidia_parent_class)->dispose (object);
}

static void
grd_hwaccel_nvidia_init (GrdHwAccelNvidia *hwaccel_nvidia)
{
  hwaccel_nvidia->encode_sessions = g_hash_table_new (NULL, NULL);
}

static void
grd_hwaccel_nvidia_class_init (GrdHwAccelNvidiaClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_hwaccel_nvidia_dispose;
}
