/*
 * Copyright (C) 2026 Pascal Nowack
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
#include <vulkan/vulkan.h>

#define GRD_TYPE_VK_PHYSICAL_DEVICE (grd_vk_physical_device_get_type ())
G_DECLARE_FINAL_TYPE (GrdVkPhysicalDevice, grd_vk_physical_device,
                      GRD, VK_PHYSICAL_DEVICE, GObject)

typedef enum
{
  GRD_VK_DEVICE_FEATURE_UPDATE_AFTER_BIND_SAMPLED_IMAGE = 1 << 0,
  GRD_VK_DEVICE_FEATURE_UPDATE_AFTER_BIND_STORAGE_IMAGE = 1 << 1,
} GrdVkDeviceFeatures;

GrdVkPhysicalDevice *grd_vk_physical_device_new (VkPhysicalDevice    vk_physical_device,
                                                 GrdVkDeviceFeatures device_features);

VkPhysicalDevice grd_vk_physical_device_get_physical_device (GrdVkPhysicalDevice *physical_device);

GrdVkDeviceFeatures grd_vk_physical_device_get_device_features (GrdVkPhysicalDevice *physical_device);
