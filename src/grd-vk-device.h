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
#include <vulkan/vulkan.h>

#include "grd-types.h"

#define GRD_TYPE_VK_DEVICE (grd_vk_device_get_type ())
G_DECLARE_FINAL_TYPE (GrdVkDevice, grd_vk_device,
                      GRD, VK_DEVICE, GObject)

typedef struct
{
  /* VK_KHR_external_memory_fd */
  PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR;

  /* VK_KHR_synchronization2 */
  PFN_vkCmdPipelineBarrier2KHR vkCmdPipelineBarrier2KHR;
  PFN_vkCmdWriteTimestamp2KHR vkCmdWriteTimestamp2KHR;
  PFN_vkQueueSubmit2KHR vkQueueSubmit2KHR;

  /* VK_KHR_external_semaphore_fd */
  PFN_vkImportSemaphoreFdKHR vkImportSemaphoreFdKHR;
} GrdVkDeviceFuncs;

typedef struct
{
  VkShaderModule create_avc_dual_view;
} GrdVkShaderModules;

GrdVkDevice *grd_vk_device_new (GrdVkPhysicalDevice      *physical_device,
                                const GrdVkSPIRVSources  *spirv_sources,
                                GError                  **error);

GrdVkPhysicalDevice *grd_vk_device_get_physical_device (GrdVkDevice *device);

VkDevice grd_vk_device_get_device (GrdVkDevice *device);

VkPipelineCache grd_vk_device_get_pipeline_cache (GrdVkDevice *device);

int grd_vk_device_get_drm_render_node_fd (GrdVkDevice *device);

float grd_vk_device_get_timestamp_period (GrdVkDevice *device);

int64_t grd_vk_device_get_drm_render_node (GrdVkDevice *device);

VkDriverId grd_vk_device_get_driver_id (GrdVkDevice *device);

GrdVkDeviceFuncs *grd_vk_device_get_device_funcs (GrdVkDevice *device);

const GrdVkShaderModules *grd_vk_device_get_shader_modules (GrdVkDevice *device);

VkDeviceSize grd_vk_device_get_aligned_size (GrdVkDevice  *device,
                                             VkDeviceSize  size);

GrdVkQueue *grd_vk_device_acquire_queue (GrdVkDevice *device);

void grd_vk_device_release_queue (GrdVkDevice *device,
                                  GrdVkQueue  *queue);
