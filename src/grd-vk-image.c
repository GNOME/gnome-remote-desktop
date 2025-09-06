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

#include "grd-vk-image.h"

#include <drm_fourcc.h>
#include <gio/gio.h>

#include "grd-vk-device.h"
#include "grd-vk-memory.h"
#include "grd-vk-utils.h"

struct _GrdVkImage
{
  GObject parent;

  GrdVkDevice *device;

  VkImage vk_image;
  GrdVkMemory *memory;

  VkImageView vk_image_view;
  VkImageLayout vk_image_layout;
};

G_DEFINE_TYPE (GrdVkImage, grd_vk_image, G_TYPE_OBJECT)

VkImage
grd_vk_image_get_image (GrdVkImage *image)
{
  return image->vk_image;
}

VkImageView
grd_vk_image_get_image_view (GrdVkImage *image)
{
  return image->vk_image_view;
}

VkImageLayout
grd_vk_image_get_image_layout (GrdVkImage *image)
{
  return image->vk_image_layout;
}

void
grd_vk_image_set_image_layout (GrdVkImage    *image,
                               VkImageLayout  vk_image_layout)
{
  image->vk_image_layout = vk_image_layout;
}

static gboolean
is_image_format_supported_by_device (GrdVkImage                  *image,
                                     const GrdVkImageDescriptor  *image_descriptor,
                                     GError                     **error)
{
  VkPhysicalDevice vk_physical_device =
    grd_vk_device_get_physical_device (image->device);
  VkImageCreateInfo *image_create_info = image_descriptor->image_create_info;
  VkPhysicalDeviceImageFormatInfo2 image_format_info_2 = {};
  VkPhysicalDeviceExternalImageFormatInfo external_image_format_info = {};
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm_format_modifier_info = {};
  VkImageFormatProperties2 image_format_properties_2 = {};
  VkImageFormatProperties *image_format_properties;
  VkResult vk_result;

  g_assert (image_create_info);
  if (image_descriptor->has_drm_format_modifier)
    g_assert (image_descriptor->drm_format_modifier != DRM_FORMAT_MOD_INVALID);

  image_format_info_2.sType =
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
  image_format_info_2.format = image_create_info->format;
  image_format_info_2.type = image_create_info->imageType;
  image_format_info_2.tiling = image_create_info->tiling;
  image_format_info_2.usage = image_create_info->usage;
  image_format_info_2.flags = image_create_info->flags;

  external_image_format_info.sType =
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
  external_image_format_info.handleType = image_descriptor->import_handle_type;

  drm_format_modifier_info.sType =
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
  drm_format_modifier_info.drmFormatModifier =
    image_descriptor->drm_format_modifier;
  drm_format_modifier_info.sharingMode = image_create_info->sharingMode;
  drm_format_modifier_info.queueFamilyIndexCount =
    image_create_info->queueFamilyIndexCount;
  drm_format_modifier_info.pQueueFamilyIndices =
    image_create_info->pQueueFamilyIndices;

  if (image_descriptor->import_handle_type)
    grd_vk_append_to_chain (&image_format_info_2, &external_image_format_info);
  if (image_descriptor->has_drm_format_modifier)
    grd_vk_append_to_chain (&image_format_info_2, &drm_format_modifier_info);

  image_format_properties_2.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;

  vk_result =
    vkGetPhysicalDeviceImageFormatProperties2 (vk_physical_device,
                                               &image_format_info_2,
                                               &image_format_properties_2);
  if (vk_result == VK_ERROR_FORMAT_NOT_SUPPORTED)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Required image format not supported by device");
      return FALSE;
    }
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to check image format properties: %i", vk_result);
      return FALSE;
    }

  image_format_properties = &image_format_properties_2.imageFormatProperties;
  if (image_format_properties->maxExtent.width < image_create_info->extent.width ||
      image_format_properties->maxExtent.height < image_create_info->extent.height ||
      image_format_properties->maxExtent.depth < image_create_info->extent.depth ||
      image_format_properties->maxMipLevels < image_create_info->mipLevels ||
      image_format_properties->maxArrayLayers < image_create_info->arrayLayers ||
      !(image_format_properties->sampleCounts & image_create_info->samples))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Required image format properties not supported by device: "
                   "maxExtent: [%u, %u, %u], maxMipLevels: %u, "
                   "maxArrayLayers %u, sampleCounts: 0x%08X",
                   image_format_properties->maxExtent.width,
                   image_format_properties->maxExtent.height,
                   image_format_properties->maxExtent.depth,
                   image_format_properties->maxMipLevels,
                   image_format_properties->maxArrayLayers,
                   image_format_properties->sampleCounts);
      return FALSE;
    }

  return TRUE;
}

static gboolean
bind_image_memory (GrdVkImage                  *image,
                   const GrdVkImageDescriptor  *image_descriptor,
                   GError                     **error)
{
  VkDevice vk_device = grd_vk_device_get_device (image->device);
  VkImageMemoryRequirementsInfo2 memory_requirements_info_2 = {};
  VkMemoryRequirements2 memory_requirements_2 = {};
  GrdVkMemoryDescriptor memory_descriptor = {};
  VkBindImageMemoryInfo bind_image_memory_info = {};
  VkResult vk_result;

  memory_requirements_info_2.sType =
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
  memory_requirements_info_2.image = image->vk_image;
  memory_requirements_2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;

  vkGetImageMemoryRequirements2 (vk_device, &memory_requirements_info_2,
                                 &memory_requirements_2);

  memory_descriptor.memory_requirements_2 = &memory_requirements_2;
  memory_descriptor.memory_flags = image_descriptor->memory_flags;
  memory_descriptor.import_handle_type = image_descriptor->import_handle_type;
  memory_descriptor.fd = image_descriptor->fd;

  image->memory = grd_vk_memory_new (image->device, &memory_descriptor, error);
  if (!image->memory)
    return FALSE;

  bind_image_memory_info.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
  bind_image_memory_info.image = image->vk_image;
  bind_image_memory_info.memory =
    grd_vk_memory_get_device_memory (image->memory);
  bind_image_memory_info.memoryOffset = 0;

  vk_result = vkBindImageMemory2 (vk_device, 1, &bind_image_memory_info);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to bind image memory: %i", vk_result);
      return FALSE;
    }

  return TRUE;
}

static gboolean
create_image_view_for_plane (GrdVkImage                  *image,
                             const GrdVkImageDescriptor  *image_descriptor,
                             GError                     **error)
{
  VkDevice vk_device = grd_vk_device_get_device (image->device);
  VkImageCreateInfo *image_create_info = image_descriptor->image_create_info;
  VkImageViewCreateInfo image_view_create_info = {};
  VkImageSubresourceRange *subresource_range;
  VkResult vk_result;

  image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_create_info.image = image->vk_image;
  image_view_create_info.format = image_create_info->format;
  image_view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

  switch (image_descriptor->image_create_info->imageType)
    {
    case VK_IMAGE_TYPE_2D:
      image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      break;
    default:
      g_assert_not_reached ();
    }

  subresource_range = &image_view_create_info.subresourceRange;
  subresource_range->aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  subresource_range->baseMipLevel = 0;
  subresource_range->levelCount = VK_REMAINING_MIP_LEVELS;
  subresource_range->baseArrayLayer = 0;
  subresource_range->layerCount = VK_REMAINING_ARRAY_LAYERS;

  vk_result = vkCreateImageView (vk_device, &image_view_create_info, NULL,
                                 &image->vk_image_view);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create image view: %i", vk_result);
      return FALSE;
    }
  g_assert (image->vk_image_view != VK_NULL_HANDLE);

  return TRUE;
}

GrdVkImage *
grd_vk_image_new (GrdVkDevice                 *device,
                  const GrdVkImageDescriptor  *image_descriptor,
                  GError                     **error)
{
  VkDevice vk_device = grd_vk_device_get_device (device);
  g_autoptr (GrdVkImage) image = NULL;
  VkResult vk_result;

  g_assert (vk_device != VK_NULL_HANDLE);
  g_assert (image_descriptor->image_create_info);

  image = g_object_new (GRD_TYPE_VK_IMAGE, NULL);
  image->device = device;

  if (!is_image_format_supported_by_device (image, image_descriptor, error))
    return NULL;

  vk_result = vkCreateImage (vk_device, image_descriptor->image_create_info,
                             NULL, &image->vk_image);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create image: %i", vk_result);
      return NULL;
    }
  g_assert (image->vk_image != VK_NULL_HANDLE);

  if (!bind_image_memory (image, image_descriptor, error))
    return NULL;

  if (!create_image_view_for_plane (image, image_descriptor, error))
    return NULL;

  return g_steal_pointer (&image);
}

static void
grd_vk_image_dispose (GObject *object)
{
  GrdVkImage *image = GRD_VK_IMAGE (object);
  VkDevice vk_device = grd_vk_device_get_device (image->device);

  g_assert (vk_device != VK_NULL_HANDLE);

  if (image->vk_image_view != VK_NULL_HANDLE)
    {
      vkDestroyImageView (vk_device, image->vk_image_view, NULL);
      image->vk_image_view = VK_NULL_HANDLE;
    }
  if (image->vk_image != VK_NULL_HANDLE)
    {
      vkDestroyImage (vk_device, image->vk_image, NULL);
      image->vk_image = VK_NULL_HANDLE;
    }

  g_clear_object (&image->memory);

  G_OBJECT_CLASS (grd_vk_image_parent_class)->dispose (object);
}

static void
grd_vk_image_init (GrdVkImage *image)
{
  image->vk_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

static void
grd_vk_image_class_init (GrdVkImageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_vk_image_dispose;
}
