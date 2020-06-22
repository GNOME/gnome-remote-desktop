/*
 * Copyright (C) 2020 Pascal Nowack
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

#ifndef GRD_DAMAGE_UTILS_H
#define GRD_DAMAGE_UTILS_H

#include <cairo/cairo.h>
#include <stdint.h>

cairo_region_t *grd_get_damage_region (uint8_t  *current_data,
                                       uint8_t  *prev_data,
                                       uint32_t  desktop_width,
                                       uint32_t  desktop_height,
                                       uint32_t  tile_width,
                                       uint32_t  tile_height,
                                       uint32_t  stride,
                                       uint32_t  bytes_per_pixel);

#endif /* GRD_DAMAGE_UTILS_H */
