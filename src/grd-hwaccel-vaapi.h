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

#pragma once

#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_HWACCEL_VAAPI (grd_hwaccel_vaapi_get_type ())
G_DECLARE_FINAL_TYPE (GrdHwAccelVaapi, grd_hwaccel_vaapi,
                      GRD, HWACCEL_VAAPI, GObject)

GrdHwAccelVaapi *grd_hwaccel_vaapi_new (GrdVkDevice  *vk_device,
                                        GError      **error);

GrdEncodeSession *grd_hwaccel_vaapi_create_encode_session (GrdHwAccelVaapi  *hwaccel_vaapi,
                                                           uint32_t          source_width,
                                                           uint32_t          source_height,
                                                           uint32_t          refresh_rate,
                                                           GError          **error);
