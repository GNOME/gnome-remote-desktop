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

#include <stdint.h>

extern "C"
{
  __global__ void
  check_damaged_pixel (uint8_t  *damage_array,
                       uint8_t  *region_is_damaged,
                       uint32_t *current_data,
                       uint32_t *previous_data,
                       uint32_t  damage_array_stride,
                       uint32_t  data_width,
                       uint32_t  data_height,
                       uint32_t  data_stride)
  {
    uint32_t data_pos;
    uint8_t damaged = 0;
    uint32_t x, y;

    x = blockIdx.x * blockDim.x + threadIdx.x;
    y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= data_width || y >= data_height)
      return;

    data_pos = y * data_stride + x;
    if (previous_data[data_pos] != current_data[data_pos])
      {
        damaged = 1;
        *region_is_damaged = 1;
      }

    damage_array[y * damage_array_stride + x] = damaged;
  }

  __global__ void
  combine_damage_array_cols (uint8_t  *damage_array,
                             uint32_t  damage_array_width,
                             uint32_t  damage_array_height,
                             uint32_t  damage_array_stride,
                             uint32_t  combine_shift)
  {
    uint32_t data_pos;
    uint32_t neighbour_offset;
    uint32_t x, y;
    uint32_t sx;

    sx = blockIdx.x * blockDim.x + threadIdx.x;
    y = blockIdx.y * blockDim.y + threadIdx.y;

    x = sx << combine_shift + 1;

    if (x >= damage_array_width || y >= damage_array_height)
      return;

    neighbour_offset = 1 << combine_shift;
    if (x + neighbour_offset >= damage_array_width)
      return;

    data_pos = y * damage_array_stride + x;
    if (damage_array[data_pos + neighbour_offset])
      damage_array[data_pos] = 1;
  }

  __global__ void
  combine_damage_array_rows (uint8_t  *damage_array,
                             uint32_t  damage_array_width,
                             uint32_t  damage_array_height,
                             uint32_t  damage_array_stride,
                             uint32_t  combine_shift)
  {
    uint32_t data_pos;
    uint32_t neighbour_offset;
    uint32_t x, y;
    uint32_t sy;

    x = blockIdx.x * blockDim.x + threadIdx.x;
    sy = blockIdx.y * blockDim.y + threadIdx.y;

    y = sy << combine_shift + 1;

    if (x >= damage_array_width || y >= damage_array_height)
      return;

    neighbour_offset = 1 << combine_shift;
    if (y + neighbour_offset >= damage_array_height)
      return;

    data_pos = y * damage_array_stride + x;
    if (damage_array[data_pos + neighbour_offset * damage_array_stride])
      damage_array[data_pos] = 1;
  }

  __global__ void
  simplify_damage_array (uint8_t  *dst_damage_array,
                         uint8_t  *src_damage_array,
                         uint32_t  dst_damage_array_stride,
                         uint32_t  src_damage_array_width,
                         uint32_t  src_damage_array_height,
                         uint32_t  src_damage_array_stride)
  {
    uint32_t src_data_pos, dst_data_pos;
    uint32_t sx, sy;
    uint32_t x, y;

    sx = blockIdx.x * blockDim.x + threadIdx.x;
    sy = blockIdx.y * blockDim.y + threadIdx.y;

    x = sx << 6;
    y = sy << 6;

    if (x >= src_damage_array_width || y >= src_damage_array_height)
      return;

    src_data_pos = y * src_damage_array_stride + x;
    dst_data_pos = sy * dst_damage_array_stride + sx;

    dst_damage_array[dst_data_pos] = src_damage_array[src_data_pos];
  }
}
