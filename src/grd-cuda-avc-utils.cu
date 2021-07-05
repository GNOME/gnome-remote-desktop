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

/*
 * Generate the PTX instructions with:
 * nvcc -arch=compute_30 -ptx grd-cuda-avc-utils.cu -o grd-cuda-avc-utils_30.ptx
 *
 * Note: This requires CUDA < 11, since the generation of Kepler capable
 * PTX code was removed from CUDA 11.
 */

#include <stdint.h>

extern "C"
{
  __device__ uint16_t
  nv12_get_interlaced_y_1x1 (uint16_t y_1x1,
                             uint16_t aligned_height)
  {
    if (y_1x1 < aligned_height >> 1)
      return y_1x1 << 1;
    return (y_1x1 << 1) - aligned_height + 1;
  }

  __device__ uint16_t
  nv12_get_interlaced_y_2x2 (uint16_t y_2x2,
                             uint16_t aligned_height)
  {
    if (y_2x2 < aligned_height >> 2)
      return y_2x2 << 1;
    return (y_2x2 << 1) - (aligned_height >> 1) + 1;
  }

  __device__ uint8_t
  rgb_to_y (uint8_t r,
            uint8_t g,
            uint8_t b)
  {
    return (54 * r + 183 * g + 18 * b) >> 8;
  }

  __device__ uint8_t
  rgb_to_u (uint8_t r,
            uint8_t g,
            uint8_t b)
  {
    return ((-29 * r - 99 * g + 128 * b) >> 8) + 128;
  }

  __device__ uint8_t
  rgb_to_v (uint8_t r,
            uint8_t g,
            uint8_t b)
  {
    return ((128 * r - 116 * g - 12 * b) >> 8) + 128;
  }

  __global__ void
  convert_2x2_bgrx_area_to_yuv420_nv12 (uint8_t  *dst_data,
                                        uint8_t  *src_data,
                                        uint16_t  src_width,
                                        uint16_t  src_height,
                                        uint16_t  src_stride,
                                        uint16_t  aligned_width,
                                        uint16_t  aligned_height,
                                        uint16_t  aligned_stride)
  {
    uint8_t *src, *dst_y0, *dst_y1, *dst_y2, *dst_y3, *dst_u, *dst_v;
    uint16_t s0, s1, s2, s3;
    int32_t r_a, g_a, b_a;
    uint8_t r, g, b;
    uint16_t x_1x1, y_1x1;
    uint16_t x_2x2, y_2x2;

    x_2x2 = blockIdx.x * blockDim.x + threadIdx.x;
    y_2x2 = blockIdx.y * blockDim.y + threadIdx.y;

    if (x_2x2 >= aligned_width >> 1 || y_2x2 >= aligned_height >> 1)
      return;

    /*
     *  -------------
     *  | d_0 | d_1 |
     *  -------------
     *  | d_2 | d_3 |
     *  -------------
     */
    s0 = 0;
    s1 = 4;
    s2 = src_stride;
    s3 = src_stride + 4;
    /*
     * Technically, the correct positions for the Y data in the resulting NV12
     * image would be the following:
     *
     * d0 = 0;
     * d1 = 1;
     * d2 = aligned_stride;
     * d3 = aligned_stride + 1;
     *
     * However, since MBAFF is used as frame field mode, NVENC requires the input
     * frame to be interlaced.
     * If the frame is not interlaced, then even lines end up in the position
     * y / 2, instead of y and odd lines end up in the position y / 2 +
     * aligned_height / 2, instead of y.
     * So, calculate the interlaced y position via a dedicated function, which
     * ensures that the lines in the input frame end up in the resulting frame to
     * be at the correct position.
     * Doing this now in the kernel here, instead of after the BGRX -> YUV420
     * conversion, saves a huge amount of time, since each thread only has a
     * super tiny overhead to perform this action, while a normal
     * device-to-device copy operation can take at least several milliseconds.
     */

    x_1x1 = x_2x2 << 1;
    y_1x1 = y_2x2 << 1;
    src = src_data + y_1x1 * src_stride + (x_1x1 << 2);

    dst_y0 = dst_data +
             nv12_get_interlaced_y_1x1 (y_1x1, aligned_height) * aligned_stride +
             x_1x1;
    dst_y1 = dst_y0 + 1;
    dst_y2 = dst_data +
             nv12_get_interlaced_y_1x1 (y_1x1 + 1, aligned_height) * aligned_stride +
             x_1x1;
    dst_y3 = dst_y2 + 1;
    dst_u = dst_data + aligned_height * aligned_stride +
            nv12_get_interlaced_y_2x2 (y_2x2, aligned_height) * aligned_stride +
            x_1x1;
    dst_v = dst_u + 1;

    /* d_0 */
    if (x_1x1 < src_width && y_1x1 < src_height)
      {
        b_a = b = src[s0 + 0];
        g_a = g = src[s0 + 1];
        r_a = r = src[s0 + 2];
        *dst_y0 = rgb_to_y (r, g, b);
      }
    else
      {
        b_a = b = 0;
        g_a = g = 0;
        r_a = r = 0;
        *dst_y0 = 0;
      }

    if (x_1x1 + 1 < src_width && y_1x1 < src_height)
      {
        /* d_1 */
        b_a += b = src[s1 + 0];
        g_a += g = src[s1 + 1];
        r_a += r = src[s1 + 2];
        *dst_y1 = rgb_to_y (r, g, b);
      }
    else
      {
        *dst_y1 = 0;
      }

    if (y_1x1 + 1 < src_height)
      {
        /* d_2 */
        b_a += b = src[s2 + 0];
        g_a += g = src[s2 + 1];
        r_a += r = src[s2 + 2];
        *dst_y2 = rgb_to_y (r, g, b);

        if (x_1x1 + 1 < src_width)
          {
            /* d_3 */
            b_a += b = src[s3 + 0];
            g_a += g = src[s3 + 1];
            r_a += r = src[s3 + 2];
            *dst_y3 = rgb_to_y (r, g, b);
          }
        else
          {
            *dst_y3 = 0;
          }
      }
    else
      {
        *dst_y2 = 0;
        *dst_y3 = 0;
      }

    b_a >>= 2;
    g_a >>= 2;
    r_a >>= 2;
    *dst_u = rgb_to_u (r_a, g_a, b_a);
    *dst_v = rgb_to_v (r_a, g_a, b_a);
  }
}
