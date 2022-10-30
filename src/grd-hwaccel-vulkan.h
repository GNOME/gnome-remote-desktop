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

#ifndef GRD_HWACCEL_VULKAN_H
#define GRD_HWACCEL_VULKAN_H

#include <glib-object.h>

#include "grd-vk-device.h"
#include "grd-types.h"

#define GRD_TYPE_HWACCEL_VULKAN (grd_hwaccel_vulkan_get_type ())
G_DECLARE_FINAL_TYPE (GrdHwAccelVulkan, grd_hwaccel_vulkan,
                      GRD, HWACCEL_VULKAN, GObject)

GrdHwAccelVulkan *grd_hwaccel_vulkan_new (GrdEglThread  *egl_thread,
                                          GError       **error);

GrdVkDevice *grd_hwaccel_vulkan_acquire_device (GrdHwAccelVulkan  *hwaccel_vulkan,
                                                GError           **error);

#endif /* GRD_HWACCEL_VULKAN_H */
