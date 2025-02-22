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

#ifndef GRD_DAMAGE_DETECTOR_SW_H
#define GRD_DAMAGE_DETECTOR_SW_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_DAMAGE_DETECTOR_SW (grd_damage_detector_sw_get_type ())
G_DECLARE_FINAL_TYPE (GrdDamageDetectorSw, grd_damage_detector_sw,
                      GRD, DAMAGE_DETECTOR_SW, GObject)

GrdDamageDetectorSw *grd_damage_detector_sw_new (uint32_t surface_width,
                                                 uint32_t surface_height);

uint32_t *grd_damage_detector_sw_get_damage_buffer (GrdDamageDetectorSw *damage_detector);

uint32_t grd_damage_detector_sw_get_damage_buffer_length (GrdDamageDetectorSw *damage_detector);

void grd_damage_detector_sw_compute_damage (GrdDamageDetectorSw *damage_detector,
                                            GrdLocalBuffer      *local_buffer_new,
                                            GrdLocalBuffer      *local_buffer_old);

#endif /* GRD_DAMAGE_DETECTOR_SW_H */
