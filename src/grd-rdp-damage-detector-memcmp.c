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

#include "grd-rdp-damage-detector-memcmp.h"

#include "grd-damage-utils.h"
#include "grd-rdp-buffer.h"

#define TILE_WIDTH 64
#define TILE_HEIGHT 64

typedef struct _GrdRdpDamageDetectorMemcmp
{
  GrdRdpDamageDetector parent;

  uint32_t surface_width;
  uint32_t surface_height;

  uint32_t cols;
  uint32_t rows;

  GrdRdpBuffer *last_framebuffer;

  gboolean region_is_damaged;
  uint8_t *damage_array;
} GrdRdpDamageDetectorMemcmp;

G_DEFINE_TYPE (GrdRdpDamageDetectorMemcmp,
               grd_rdp_damage_detector_memcmp,
               GRD_TYPE_RDP_DAMAGE_DETECTOR)

static gboolean
invalidate_surface (GrdRdpDamageDetector *detector)
{
  GrdRdpDamageDetectorMemcmp *detector_memcmp =
    GRD_RDP_DAMAGE_DETECTOR_MEMCMP (detector);
  uint32_t cols = detector_memcmp->cols;
  uint32_t rows = detector_memcmp->rows;

  g_clear_pointer (&detector_memcmp->last_framebuffer, grd_rdp_buffer_release);

  if (!detector_memcmp->damage_array)
    return TRUE;

  memset (detector_memcmp->damage_array, 1, cols * rows);
  detector_memcmp->region_is_damaged = TRUE;

  return TRUE;
}

static gboolean
resize_surface (GrdRdpDamageDetector *detector,
                uint32_t              width,
                uint32_t              height)
{
  GrdRdpDamageDetectorMemcmp *detector_memcmp =
    GRD_RDP_DAMAGE_DETECTOR_MEMCMP (detector);
  uint32_t cols;
  uint32_t rows;

  g_clear_pointer (&detector_memcmp->last_framebuffer, grd_rdp_buffer_release);
  g_clear_pointer (&detector_memcmp->damage_array, g_free);

  detector_memcmp->surface_width = width;
  detector_memcmp->surface_height = height;

  cols = width / TILE_WIDTH + (width % TILE_WIDTH ? 1 : 0);
  rows = height / TILE_HEIGHT + (height % TILE_HEIGHT ? 1 : 0);
  detector_memcmp->damage_array = g_malloc0 (cols * rows * sizeof (uint8_t));

  memset (detector_memcmp->damage_array, 1, cols * rows);
  detector_memcmp->region_is_damaged = TRUE;

  detector_memcmp->cols = cols;
  detector_memcmp->rows = rows;

  return TRUE;
}

static gboolean
submit_new_framebuffer (GrdRdpDamageDetector *detector,
                        GrdRdpBuffer         *buffer)
{
  GrdRdpDamageDetectorMemcmp *detector_memcmp =
    GRD_RDP_DAMAGE_DETECTOR_MEMCMP (detector);
  uint32_t surface_width = detector_memcmp->surface_width;
  uint32_t surface_height = detector_memcmp->surface_height;
  uint32_t cols = detector_memcmp->cols;
  uint32_t rows = detector_memcmp->rows;
  gboolean region_is_damaged = FALSE;
  uint32_t x, y;

  g_assert (detector_memcmp->damage_array);

  if (!detector_memcmp->last_framebuffer)
    {
      detector_memcmp->last_framebuffer = buffer;

      memset (detector_memcmp->damage_array, 1, cols * rows);
      detector_memcmp->region_is_damaged = TRUE;
      return TRUE;
    }

  for (y = 0; y < detector_memcmp->rows; ++y)
    {
      for (x = 0; x < detector_memcmp->cols; ++x)
        {
          GrdRdpBuffer *last_framebuffer = detector_memcmp->last_framebuffer;
          cairo_rectangle_int_t tile;
          uint8_t tile_damaged = 0;

          tile.x = x * TILE_WIDTH;
          tile.y = y * TILE_HEIGHT;
          tile.width = surface_width - tile.x < TILE_WIDTH ? surface_width - tile.x
                                                           : TILE_WIDTH;
          tile.height = surface_height - tile.y < TILE_HEIGHT ? surface_height - tile.y
                                                              : TILE_HEIGHT;

          if (grd_is_tile_dirty (&tile, grd_rdp_buffer_get_local_data (buffer),
                                 grd_rdp_buffer_get_local_data (last_framebuffer),
                                 surface_width * 4, 4))
            {
              tile_damaged = 1;
              region_is_damaged = TRUE;
            }

          detector_memcmp->damage_array[y * detector_memcmp->cols + x] = tile_damaged;
        }
    }

  g_clear_pointer (&detector_memcmp->last_framebuffer, grd_rdp_buffer_release);
  detector_memcmp->last_framebuffer = buffer;
  detector_memcmp->region_is_damaged = region_is_damaged;

  return TRUE;
}

static gboolean
is_region_damaged (GrdRdpDamageDetector *detector)
{
  GrdRdpDamageDetectorMemcmp *detector_memcmp =
    GRD_RDP_DAMAGE_DETECTOR_MEMCMP (detector);

  g_assert (detector_memcmp->damage_array);
  g_assert (detector_memcmp->last_framebuffer);

  return detector_memcmp->region_is_damaged;
}

static cairo_region_t *
get_damage_region (GrdRdpDamageDetector *detector)
{
  GrdRdpDamageDetectorMemcmp *detector_memcmp =
    GRD_RDP_DAMAGE_DETECTOR_MEMCMP (detector);
  uint32_t surface_width = detector_memcmp->surface_width;
  uint32_t surface_height = detector_memcmp->surface_height;
  cairo_region_t *damage_region;
  cairo_rectangle_int_t tile;
  uint32_t x, y;

  g_assert (detector_memcmp->damage_array);
  g_assert (detector_memcmp->last_framebuffer);

  damage_region = cairo_region_create ();
  for (y = 0; y < detector_memcmp->rows; ++y)
    {
      for (x = 0; x < detector_memcmp->cols; ++x)
        {
          if (detector_memcmp->damage_array[y * detector_memcmp->cols + x])
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

GrdRdpDamageDetectorMemcmp *
grd_rdp_damage_detector_memcmp_new (void)
{
  GrdRdpDamageDetectorMemcmp *detector_memcmp;

  detector_memcmp = g_object_new (GRD_TYPE_RDP_DAMAGE_DETECTOR_MEMCMP, NULL);

  return detector_memcmp;
}

static void
grd_rdp_damage_detector_memcmp_dispose (GObject *object)
{
  GrdRdpDamageDetectorMemcmp *detector_memcmp =
    GRD_RDP_DAMAGE_DETECTOR_MEMCMP (object);

  g_assert (!detector_memcmp->last_framebuffer);

  g_clear_pointer (&detector_memcmp->damage_array, g_free);

  G_OBJECT_CLASS (grd_rdp_damage_detector_memcmp_parent_class)->dispose (object);
}

static void
grd_rdp_damage_detector_memcmp_init (GrdRdpDamageDetectorMemcmp *detector_memcmp)
{
}

static void
grd_rdp_damage_detector_memcmp_class_init (GrdRdpDamageDetectorMemcmpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdRdpDamageDetectorClass *detector_class =
    GRD_RDP_DAMAGE_DETECTOR_CLASS (klass);

  object_class->dispose = grd_rdp_damage_detector_memcmp_dispose;

  detector_class->invalidate_surface = invalidate_surface;
  detector_class->resize_surface = resize_surface;
  detector_class->submit_new_framebuffer = submit_new_framebuffer;
  detector_class->is_region_damaged = is_region_damaged;
  detector_class->get_damage_region = get_damage_region;
}
