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

#include <gio/gio.h>
#include <vulkan/vulkan.h>

#include "grd-types.h"

void grd_vk_append_to_chain (void *chain,
                             void *element);

void grd_vk_clear_command_pool (GrdVkDevice   *device,
                                VkCommandPool *vk_command_pool);

void grd_vk_clear_descriptor_pool (GrdVkDevice      *device,
                                   VkDescriptorPool *vk_descriptor_pool);

void grd_vk_clear_descriptor_set_layout (GrdVkDevice           *device,
                                         VkDescriptorSetLayout *vk_descriptor_set_layout);

void grd_vk_clear_fence (GrdVkDevice *device,
                         VkFence     *vk_fence);

void grd_vk_clear_query_pool (GrdVkDevice *device,
                              VkQueryPool *vk_query_pool);

void grd_vk_clear_sampler (GrdVkDevice *device,
                           VkSampler   *vk_sampler);

gboolean grd_vk_get_vk_format_from_drm_format (uint32_t   drm_format,
                                               VkFormat  *vk_format,
                                               GError   **error);

GrdVkImage *grd_vk_dma_buf_image_new (GrdVkDevice        *vk_device,
                                      VkFormat            vk_format,
                                      uint32_t            width,
                                      uint32_t            height,
                                      VkImageUsageFlags   usage_flags,
                                      int                 fd,
                                      VkDeviceSize        offset,
                                      VkDeviceSize        row_pitch,
                                      uint64_t            drm_format_modifier,
                                      GError            **error);
