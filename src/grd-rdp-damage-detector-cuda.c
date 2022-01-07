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

#include "grd-rdp-damage-detector-cuda.h"

#include "grd-hwaccel-nvidia.h"
#include "grd-rdp-buffer.h"

#define TILE_WIDTH 64
#define TILE_HEIGHT 64

typedef struct _GrdRdpDamageDetectorCuda
{
  GrdRdpDamageDetector parent;

  CudaFunctions *cuda_funcs;
  CUstream cuda_stream;

  CUfunction cu_chk_dmg_pxl;
  CUfunction cu_cmb_dmg_arr_cols;
  CUfunction cu_cmb_dmg_arr_rows;
  CUfunction cu_simplify_dmg_arr;

  uint32_t surface_width;
  uint32_t surface_height;

  uint32_t cols;
  uint32_t rows;

  GrdRdpBuffer *last_framebuffer;

  CUdeviceptr region_is_damaged;
  CUdeviceptr damage_array;
  CUdeviceptr simplified_damage_array;
} GrdRdpDamageDetectorCuda;

G_DEFINE_TYPE (GrdRdpDamageDetectorCuda,
               grd_rdp_damage_detector_cuda,
               GRD_TYPE_RDP_DAMAGE_DETECTOR)

static gboolean
invalidate_surface (GrdRdpDamageDetector *detector)
{
  GrdRdpDamageDetectorCuda *detector_cuda =
    GRD_RDP_DAMAGE_DETECTOR_CUDA (detector);
  CudaFunctions *cuda_funcs = detector_cuda->cuda_funcs;
  uint32_t surface_width = detector_cuda->surface_width;
  uint32_t surface_height = detector_cuda->surface_height;

  g_clear_pointer (&detector_cuda->last_framebuffer, grd_rdp_buffer_release);

  if (!detector_cuda->damage_array)
    return TRUE;

  if (cuda_funcs->cuMemsetD8Async (detector_cuda->damage_array,
                                   1, surface_width * surface_height,
                                   detector_cuda->cuda_stream) != CUDA_SUCCESS ||
      cuda_funcs->cuMemsetD8Async (detector_cuda->region_is_damaged, 1, 1,
                                   detector_cuda->cuda_stream) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to set memory");
      return FALSE;
    }

  return TRUE;
}

static void
clear_cuda_pointer (GrdRdpDamageDetectorCuda *detector_cuda,
                    CUdeviceptr              *device_ptr)
{
  if (!(*device_ptr))
    return;

  detector_cuda->cuda_funcs->cuMemFree (*device_ptr);
  *device_ptr = 0;
}

static gboolean
resize_surface (GrdRdpDamageDetector *detector,
                uint32_t              width,
                uint32_t              height)
{
  GrdRdpDamageDetectorCuda *detector_cuda =
    GRD_RDP_DAMAGE_DETECTOR_CUDA (detector);
  CudaFunctions *cuda_funcs = detector_cuda->cuda_funcs;
  uint32_t cols;
  uint32_t rows;

  g_clear_pointer (&detector_cuda->last_framebuffer, grd_rdp_buffer_release);

  clear_cuda_pointer (detector_cuda, &detector_cuda->simplified_damage_array);
  clear_cuda_pointer (detector_cuda, &detector_cuda->damage_array);

  detector_cuda->surface_width = width;
  detector_cuda->surface_height = height;

  cols = width / TILE_WIDTH + (width % TILE_WIDTH ? 1 : 0);
  rows = height / TILE_HEIGHT + (height % TILE_HEIGHT ? 1 : 0);
  detector_cuda->cols = cols;
  detector_cuda->rows = rows;

  if (cuda_funcs->cuMemAlloc (&detector_cuda->damage_array,
                              width * height) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to allocate damage array");
      return FALSE;
    }
  if (cuda_funcs->cuMemAlloc (&detector_cuda->simplified_damage_array,
                              cols * rows) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to allocate simplified damage array");
      return FALSE;
    }
  if (cuda_funcs->cuMemsetD8Async (detector_cuda->damage_array, 1, width * height,
                                   detector_cuda->cuda_stream) != CUDA_SUCCESS ||
      cuda_funcs->cuMemsetD8Async (detector_cuda->region_is_damaged, 1, 1,
                                   detector_cuda->cuda_stream) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to set memory");
      return FALSE;
    }

  return TRUE;
}

static gboolean
submit_new_framebuffer (GrdRdpDamageDetector *detector,
                        GrdRdpBuffer         *buffer)
{
  GrdRdpDamageDetectorCuda *detector_cuda =
    GRD_RDP_DAMAGE_DETECTOR_CUDA (detector);
  CudaFunctions *cuda_funcs = detector_cuda->cuda_funcs;
  uint32_t surface_width = detector_cuda->surface_width;
  uint32_t surface_height = detector_cuda->surface_height;
  unsigned int grid_dim_x, grid_dim_y, grid_dim_z;
  unsigned int block_dim_x, block_dim_y, block_dim_z;
  void *args[8];

  g_assert (detector_cuda->damage_array);

  if (!detector_cuda->last_framebuffer)
    {
      if (cuda_funcs->cuMemsetD8Async (detector_cuda->damage_array,
                                       1, surface_width * surface_height,
                                       detector_cuda->cuda_stream) != CUDA_SUCCESS ||
          cuda_funcs->cuMemsetD8Async (detector_cuda->region_is_damaged, 1, 1,
                                       detector_cuda->cuda_stream) != CUDA_SUCCESS)
        {
          g_warning ("[HWAccel.CUDA] Failed to set memory");
          return FALSE;
        }

      detector_cuda->last_framebuffer = buffer;

      return TRUE;
    }

  if (cuda_funcs->cuMemsetD8Async (detector_cuda->region_is_damaged, 0, 1,
                                   detector_cuda->cuda_stream) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to set memory");
      return FALSE;
    }

  /* Threads per blocks */
  block_dim_x = 32;
  block_dim_y = 16;
  block_dim_z = 1;
  /* Amount of blocks per grid */
  grid_dim_x = surface_width / block_dim_x +
               (surface_width % block_dim_x ? 1 : 0);
  grid_dim_y = surface_height / block_dim_y +
               (surface_height % block_dim_y ? 1 : 0);
  grid_dim_z = 1;

  args[0] = &detector_cuda->damage_array;
  args[1] = &detector_cuda->region_is_damaged;
  args[2] = &buffer->mapped_cuda_pointer;
  args[3] = &detector_cuda->last_framebuffer->mapped_cuda_pointer;
  args[4] = &surface_width;
  args[5] = &surface_width;
  args[6] = &surface_height;
  args[7] = &surface_width;
  if (cuda_funcs->cuLaunchKernel (detector_cuda->cu_chk_dmg_pxl,
                                  grid_dim_x, grid_dim_y, grid_dim_z,
                                  block_dim_x, block_dim_y, block_dim_z,
                                  0, detector_cuda->cuda_stream,
                                  args, NULL) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to launch CHK_DMG_PXL kernel");
      return FALSE;
    }

  g_clear_pointer (&detector_cuda->last_framebuffer, grd_rdp_buffer_release);
  detector_cuda->last_framebuffer = buffer;

  return TRUE;
}

static gboolean
is_region_damaged (GrdRdpDamageDetector *detector)
{
  GrdRdpDamageDetectorCuda *detector_cuda =
    GRD_RDP_DAMAGE_DETECTOR_CUDA (detector);
  CudaFunctions *cuda_funcs = detector_cuda->cuda_funcs;
  uint8_t is_damaged;

  g_assert (detector_cuda->damage_array);
  g_assert (detector_cuda->last_framebuffer);

  cuda_funcs->cuMemcpyDtoHAsync (&is_damaged, detector_cuda->region_is_damaged,
                                 1, detector_cuda->cuda_stream);
  cuda_funcs->cuStreamSynchronize (detector_cuda->cuda_stream);

  return !!is_damaged;
}

static cairo_region_t *
get_cairo_region (GrdRdpDamageDetectorCuda *detector_cuda,
                  uint8_t                  *simplified_damage_array)
{
  uint32_t surface_width = detector_cuda->surface_width;
  uint32_t surface_height = detector_cuda->surface_height;
  cairo_region_t *damage_region;
  cairo_rectangle_int_t tile;
  uint32_t x, y;

  damage_region = cairo_region_create ();
  for (y = 0; y < detector_cuda->rows; ++y)
    {
      for (x = 0; x < detector_cuda->cols; ++x)
        {
          if (simplified_damage_array[y * detector_cuda->cols + x])
            {
              tile.x = x * TILE_WIDTH;
              tile.y = y * TILE_HEIGHT;
              tile.width = surface_width - tile.x < TILE_WIDTH ? surface_width - tile.x
                                                               : TILE_WIDTH;
              tile.height = surface_height - tile.y < TILE_HEIGHT ? surface_height - tile.y
                                                                  : TILE_HEIGHT;

              cairo_region_union_rectangle (damage_region, &tile);
            }
        }
    }

  return damage_region;
}

static cairo_region_t *
get_damage_region (GrdRdpDamageDetector *detector)
{
  GrdRdpDamageDetectorCuda *detector_cuda =
    GRD_RDP_DAMAGE_DETECTOR_CUDA (detector);
  CudaFunctions *cuda_funcs = detector_cuda->cuda_funcs;
  g_autofree uint8_t *simplified_damage_array_host = NULL;
  unsigned int grid_dim_x, grid_dim_y, grid_dim_z;
  unsigned int block_dim_x, block_dim_y, block_dim_z;
  uint32_t combine_shift[6];
  void *args_cols[5];
  void *args_rows[5];
  void *args_simplify[6];
  uint32_t i;

  g_assert (detector_cuda->damage_array);
  g_assert (detector_cuda->last_framebuffer);

  /* Threads per blocks */
  block_dim_x = 32;
  block_dim_y = 16;
  block_dim_z = 1;

  args_cols[0] = args_rows[0] = &detector_cuda->damage_array;
  args_cols[1] = args_rows[1] = &detector_cuda->surface_width;
  args_cols[2] = args_rows[2] = &detector_cuda->surface_height;
  args_cols[3] = args_rows[3] = &detector_cuda->surface_width;

  for (i = 0; i < 6; ++i)
    {
      uint32_t full_blocks;

      combine_shift[i] = i;
      args_cols[4] = &combine_shift[i];

      full_blocks = detector_cuda->surface_width >> (i + 1);

      /* Amount of blocks per grid */
      grid_dim_x = full_blocks / block_dim_x + 1;
      grid_dim_y = detector_cuda->surface_height / block_dim_y +
                   (detector_cuda->surface_height % block_dim_y ? 1 : 0);
      grid_dim_z = 1;

      if (cuda_funcs->cuLaunchKernel (detector_cuda->cu_cmb_dmg_arr_cols,
                                      grid_dim_x, grid_dim_y, grid_dim_z,
                                      block_dim_x, block_dim_y, block_dim_z,
                                      0, detector_cuda->cuda_stream,
                                      args_cols, NULL) != CUDA_SUCCESS)
        {
          g_warning ("[HWAccel.CUDA] Failed to launch CMB_DMG_ARR_COLS kernel");
          return NULL;
        }
    }

  for (i = 0; i < 6; ++i)
    {
      uint32_t full_blocks;

      args_rows[4] = &combine_shift[i];

      full_blocks = detector_cuda->surface_height >> (i + 1);

      /* Amount of blocks per grid */
      grid_dim_x = detector_cuda->surface_width / block_dim_x +
                   (detector_cuda->surface_width % block_dim_x ? 1 : 0);
      grid_dim_y = full_blocks / block_dim_y + 1;
      grid_dim_z = 1;

      if (cuda_funcs->cuLaunchKernel (detector_cuda->cu_cmb_dmg_arr_rows,
                                      grid_dim_x, grid_dim_y, grid_dim_z,
                                      block_dim_x, block_dim_y, block_dim_z,
                                      0, detector_cuda->cuda_stream,
                                      args_rows, NULL) != CUDA_SUCCESS)
        {
          g_warning ("[HWAccel.CUDA] Failed to launch CMB_DMG_ARR_ROWS kernel");
          return NULL;
        }
    }

  /* Amount of blocks per grid */
  grid_dim_x = detector_cuda->surface_width / block_dim_x +
               (detector_cuda->surface_width % block_dim_x ? 1 : 0);
  grid_dim_y = detector_cuda->surface_height / block_dim_y +
               (detector_cuda->surface_height % block_dim_y ? 1 : 0);
  grid_dim_z = 1;

  args_simplify[0] = &detector_cuda->simplified_damage_array;
  args_simplify[1] = &detector_cuda->damage_array;
  args_simplify[2] = &detector_cuda->cols;
  args_simplify[3] = &detector_cuda->surface_width;
  args_simplify[4] = &detector_cuda->surface_height;
  args_simplify[5] = &detector_cuda->surface_width;

  if (cuda_funcs->cuLaunchKernel (detector_cuda->cu_simplify_dmg_arr,
                                  grid_dim_x, grid_dim_y, grid_dim_z,
                                  block_dim_x, block_dim_y, block_dim_z,
                                  0, detector_cuda->cuda_stream,
                                  args_simplify, NULL) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to launch SIMPLIFY_DMG_ARR kernel");
      return NULL;
    }

  simplified_damage_array_host = g_malloc0 (detector_cuda->cols *
                                            detector_cuda->rows *
                                            sizeof (uint8_t));

  if (cuda_funcs->cuMemcpyDtoHAsync (simplified_damage_array_host,
                                     detector_cuda->simplified_damage_array,
                                     detector_cuda->cols * detector_cuda->rows,
                                     detector_cuda->cuda_stream) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to transfer simplified damage array");
      return NULL;
    }
  if (cuda_funcs->cuStreamSynchronize (detector_cuda->cuda_stream) != CUDA_SUCCESS)
    {
      g_warning ("[HWAccel.CUDA] Failed to synchronize stream");
      return NULL;
    }

  return get_cairo_region (detector_cuda, simplified_damage_array_host);
}

GrdRdpDamageDetectorCuda *
grd_rdp_damage_detector_cuda_new (GrdHwAccelNvidia *hwaccel_nvidia,
                                  CUstream          cuda_stream)
{
  g_autoptr (GrdRdpDamageDetectorCuda) detector_cuda = NULL;
  CudaFunctions *cuda_funcs;

  detector_cuda = g_object_new (GRD_TYPE_RDP_DAMAGE_DETECTOR_CUDA, NULL);
  detector_cuda->cuda_stream = cuda_stream;

  grd_hwaccel_nvidia_get_cuda_functions (hwaccel_nvidia,
                                         (gpointer *) &detector_cuda->cuda_funcs);
  grd_hwaccel_nvidia_get_cuda_damage_kernels (hwaccel_nvidia,
                                              &detector_cuda->cu_chk_dmg_pxl,
                                              &detector_cuda->cu_cmb_dmg_arr_cols,
                                              &detector_cuda->cu_cmb_dmg_arr_rows,
                                              &detector_cuda->cu_simplify_dmg_arr);

  cuda_funcs = detector_cuda->cuda_funcs;
  if (cuda_funcs->cuMemAlloc (&detector_cuda->region_is_damaged, 1) != CUDA_SUCCESS)
    return NULL;

  return g_steal_pointer (&detector_cuda);
}

static void
grd_rdp_damage_detector_cuda_dispose (GObject *object)
{
  GrdRdpDamageDetectorCuda *detector_cuda =
    GRD_RDP_DAMAGE_DETECTOR_CUDA (object);

  g_assert (!detector_cuda->last_framebuffer);

  clear_cuda_pointer (detector_cuda, &detector_cuda->simplified_damage_array);
  clear_cuda_pointer (detector_cuda, &detector_cuda->damage_array);
  clear_cuda_pointer (detector_cuda, &detector_cuda->region_is_damaged);

  G_OBJECT_CLASS (grd_rdp_damage_detector_cuda_parent_class)->dispose (object);
}

static void
grd_rdp_damage_detector_cuda_init (GrdRdpDamageDetectorCuda *detector_cuda)
{
}

static void
grd_rdp_damage_detector_cuda_class_init (GrdRdpDamageDetectorCudaClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdRdpDamageDetectorClass *detector_class =
    GRD_RDP_DAMAGE_DETECTOR_CLASS (klass);

  object_class->dispose = grd_rdp_damage_detector_cuda_dispose;

  detector_class->invalidate_surface = invalidate_surface;
  detector_class->resize_surface = resize_surface;
  detector_class->submit_new_framebuffer = submit_new_framebuffer;
  detector_class->is_region_damaged = is_region_damaged;
  detector_class->get_damage_region = get_damage_region;
}
