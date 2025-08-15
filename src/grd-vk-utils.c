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

#include "grd-vk-utils.h"

#include <drm_fourcc.h>

#include "grd-vk-device.h"
#include "grd-vk-image.h"

typedef struct
{
  VkStructureType type; /* sType */
  const void *next; /* pNext */
} GrdVkBaseStruct;

void
grd_vk_append_to_chain (void *chain,
                        void *element)
{
  GrdVkBaseStruct *tmp;

  tmp = chain;
  while (tmp->next)
    tmp = (GrdVkBaseStruct *) tmp->next;

  tmp->next = element;
}

void
grd_vk_clear_command_pool (GrdVkDevice   *device,
                           VkCommandPool *vk_command_pool)
{
  VkDevice vk_device = grd_vk_device_get_device (device);

  g_assert (vk_device != VK_NULL_HANDLE);

  if (*vk_command_pool == VK_NULL_HANDLE)
    return;

  vkDestroyCommandPool (vk_device, *vk_command_pool, NULL);
  *vk_command_pool = VK_NULL_HANDLE;
}

void
grd_vk_clear_descriptor_pool (GrdVkDevice      *device,
                              VkDescriptorPool *vk_descriptor_pool)
{
  VkDevice vk_device = grd_vk_device_get_device (device);

  g_assert (vk_device != VK_NULL_HANDLE);

  if (*vk_descriptor_pool == VK_NULL_HANDLE)
    return;

  vkDestroyDescriptorPool (vk_device, *vk_descriptor_pool, NULL);
  *vk_descriptor_pool = VK_NULL_HANDLE;
}

void
grd_vk_clear_descriptor_set_layout (GrdVkDevice           *device,
                                    VkDescriptorSetLayout *vk_descriptor_set_layout)
{
  VkDevice vk_device = grd_vk_device_get_device (device);

  g_assert (vk_device != VK_NULL_HANDLE);

  if (*vk_descriptor_set_layout == VK_NULL_HANDLE)
    return;

  vkDestroyDescriptorSetLayout (vk_device, *vk_descriptor_set_layout, NULL);
  *vk_descriptor_set_layout = VK_NULL_HANDLE;
}

void
grd_vk_clear_fence (GrdVkDevice *device,
                    VkFence     *vk_fence)
{
  VkDevice vk_device = grd_vk_device_get_device (device);

  g_assert (vk_device != VK_NULL_HANDLE);

  if (*vk_fence == VK_NULL_HANDLE)
    return;

  vkDestroyFence (vk_device, *vk_fence, NULL);
  *vk_fence = VK_NULL_HANDLE;
}

void
grd_vk_clear_query_pool (GrdVkDevice *device,
                         VkQueryPool *vk_query_pool)
{
  VkDevice vk_device = grd_vk_device_get_device (device);

  g_assert (vk_device != VK_NULL_HANDLE);

  if (*vk_query_pool == VK_NULL_HANDLE)
    return;

  vkDestroyQueryPool (vk_device, *vk_query_pool, NULL);
  *vk_query_pool = VK_NULL_HANDLE;
}

void
grd_vk_clear_sampler (GrdVkDevice *device,
                      VkSampler   *vk_sampler)
{
  VkDevice vk_device = grd_vk_device_get_device (device);

  g_assert (vk_device != VK_NULL_HANDLE);

  if (*vk_sampler == VK_NULL_HANDLE)
    return;

  vkDestroySampler (vk_device, *vk_sampler, NULL);
  *vk_sampler = VK_NULL_HANDLE;
}

gboolean
grd_vk_get_vk_format_from_drm_format (uint32_t   drm_format,
                                      VkFormat  *vk_format,
                                      GError   **error)
{
  *vk_format = VK_FORMAT_UNDEFINED;

  switch (drm_format)
    {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
      *vk_format = VK_FORMAT_B8G8R8A8_UNORM;
      return TRUE;
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "No VkFormat available for DRM format 0x%08X", drm_format);

  return FALSE;
}

GrdVkImage *
grd_vk_dma_buf_image_new (GrdVkDevice        *vk_device,
                          VkFormat            vk_format,
                          uint32_t            width,
                          uint32_t            height,
                          VkImageUsageFlags   usage_flags,
                          int                 fd,
                          VkDeviceSize        offset,
                          VkDeviceSize        row_pitch,
                          uint64_t            drm_format_modifier,
                          GError            **error)
{
  GrdVkImageDescriptor image_descriptor = {};
  VkImageCreateInfo image_create_info = {};
  VkExternalMemoryImageCreateInfo external_memory_image_create_info = {};
  VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_create_info = {};
  VkSubresourceLayout subresource_layout = {};
  VkExternalMemoryHandleTypeFlagBits handle_type;

  g_assert (fd != -1);
  g_assert (drm_format_modifier != DRM_FORMAT_MOD_INVALID);

  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.format = vk_format;
  image_create_info.extent.width = width;
  image_create_info.extent.height = height;
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  image_create_info.usage = usage_flags;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  external_memory_image_create_info.sType =
    VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  external_memory_image_create_info.handleTypes = handle_type;

  subresource_layout.offset = offset;
  subresource_layout.rowPitch = row_pitch;

  explicit_create_info.sType =
    VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
  explicit_create_info.drmFormatModifier = drm_format_modifier;
  explicit_create_info.drmFormatModifierPlaneCount = 1;
  explicit_create_info.pPlaneLayouts = &subresource_layout;

  grd_vk_append_to_chain (&image_create_info,
                          &external_memory_image_create_info);
  grd_vk_append_to_chain (&image_create_info, &explicit_create_info);

  image_descriptor.image_create_info = &image_create_info;
  image_descriptor.import_handle_type = handle_type;
  image_descriptor.fd = fd;
  image_descriptor.drm_format_modifier = drm_format_modifier;
  image_descriptor.has_drm_format_modifier = TRUE;

  return grd_vk_image_new (vk_device, &image_descriptor, error);
}
