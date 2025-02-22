/*
 * Copyright (C) 2025 Pascal Nowack
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

#include "grd-damage-detector-sw.h"

#include "grd-damage-utils.h"
#include "grd-local-buffer.h"
#include "grd-utils.h"

#define TILE_WIDTH 64
#define TILE_HEIGHT 64

struct _GrdDamageDetectorSw
{
  GObject parent;

  uint32_t surface_width;
  uint32_t surface_height;

  uint32_t damage_buffer_width;
  uint32_t damage_buffer_height;

  uint32_t *damage_buffer;
  uint32_t damage_buffer_length;
};

G_DEFINE_TYPE (GrdDamageDetectorSw, grd_damage_detector_sw,
               G_TYPE_OBJECT)

uint32_t *
grd_damage_detector_sw_get_damage_buffer (GrdDamageDetectorSw *damage_detector)
{
  return damage_detector->damage_buffer;
}

uint32_t
grd_damage_detector_sw_get_damage_buffer_length (GrdDamageDetectorSw *damage_detector)
{
  return damage_detector->damage_buffer_length;
}

static void
invalidate_surface (GrdDamageDetectorSw *damage_detector)
{
  uint32_t i;

  for (i = 0; i < damage_detector->damage_buffer_length; ++i)
    damage_detector->damage_buffer[i] = 1;
}

static void
compute_frame_damage (GrdDamageDetectorSw *damage_detector,
                      GrdLocalBuffer      *local_buffer_new,
                      GrdLocalBuffer      *local_buffer_old)
{
  uint32_t surface_width = damage_detector->surface_width;
  uint32_t surface_height = damage_detector->surface_height;
  uint32_t local_buffer_stride;
  uint32_t damage_buffer_stride;
  uint32_t x, y;

  g_assert (local_buffer_new);
  g_assert (local_buffer_old);

  g_assert (grd_local_buffer_get_buffer_stride (local_buffer_new) ==
            grd_local_buffer_get_buffer_stride (local_buffer_old));
  local_buffer_stride = grd_local_buffer_get_buffer_stride (local_buffer_new);

  damage_buffer_stride = damage_detector->damage_buffer_width;

  for (y = 0; y < damage_detector->damage_buffer_height; ++y)
    {
      for (x = 0; x < damage_detector->damage_buffer_width; ++x)
        {
          cairo_rectangle_int_t tile;
          uint32_t tile_damaged = 0;
          uint32_t buffer_pos;

          tile.x = x * TILE_WIDTH;
          tile.y = y * TILE_HEIGHT;
          tile.width = surface_width - tile.x < TILE_WIDTH ?
                         surface_width - tile.x : TILE_WIDTH;
          tile.height = surface_height - tile.y < TILE_HEIGHT ?
                          surface_height - tile.y : TILE_HEIGHT;

          if (grd_is_tile_dirty (&tile,
                                 grd_local_buffer_get_buffer (local_buffer_new),
                                 grd_local_buffer_get_buffer (local_buffer_old),
                                 local_buffer_stride, 4))
            tile_damaged = 1;

          buffer_pos = y * damage_buffer_stride + x;
          damage_detector->damage_buffer[buffer_pos] = tile_damaged;
        }
    }
}

void
grd_damage_detector_sw_compute_damage (GrdDamageDetectorSw *damage_detector,
                                       GrdLocalBuffer      *local_buffer_new,
                                       GrdLocalBuffer      *local_buffer_old)
{
  if (!local_buffer_old)
    {
      invalidate_surface (damage_detector);
      return;
    }

  compute_frame_damage (damage_detector, local_buffer_new, local_buffer_old);
}

static void
create_damage_buffer (GrdDamageDetectorSw *damage_detector)
{
  uint32_t surface_width = damage_detector->surface_width;
  uint32_t surface_height = damage_detector->surface_height;
  uint32_t damage_buffer_width;
  uint32_t damage_buffer_height;
  uint32_t damage_buffer_length;

  damage_buffer_width = grd_get_aligned_size (surface_width, TILE_WIDTH) /
                        TILE_WIDTH;
  damage_buffer_height = grd_get_aligned_size (surface_height, TILE_HEIGHT) /
                         TILE_HEIGHT;
  damage_buffer_length = damage_buffer_width * damage_buffer_height;

  damage_detector->damage_buffer_width = damage_buffer_width;
  damage_detector->damage_buffer_height = damage_buffer_height;

  damage_detector->damage_buffer = g_new0 (uint32_t, damage_buffer_length);
  damage_detector->damage_buffer_length = damage_buffer_length;
}

GrdDamageDetectorSw *
grd_damage_detector_sw_new (uint32_t surface_width,
                            uint32_t surface_height)
{
  GrdDamageDetectorSw *damage_detector;

  damage_detector = g_object_new (GRD_TYPE_DAMAGE_DETECTOR_SW, NULL);
  damage_detector->surface_width = surface_width;
  damage_detector->surface_height = surface_height;

  create_damage_buffer (damage_detector);

  return damage_detector;
}

static void
grd_damage_detector_sw_dispose (GObject *object)
{
  GrdDamageDetectorSw *damage_detector = GRD_DAMAGE_DETECTOR_SW (object);

  g_clear_pointer (&damage_detector->damage_buffer, g_free);

  G_OBJECT_CLASS (grd_damage_detector_sw_parent_class)->dispose (object);
}

static void
grd_damage_detector_sw_init (GrdDamageDetectorSw *damage_detector)
{
}

static void
grd_damage_detector_sw_class_init (GrdDamageDetectorSwClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_damage_detector_sw_dispose;
}
