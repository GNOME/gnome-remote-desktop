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

#include "grd-vk-buffer.h"

#include <gio/gio.h>

#include "grd-vk-device.h"
#include "grd-vk-memory.h"

struct _GrdVkBuffer
{
  GObject parent;

  GrdVkDevice *device;

  VkBuffer vk_buffer;
  GrdVkMemory *memory;
};

G_DEFINE_TYPE (GrdVkBuffer, grd_vk_buffer, G_TYPE_OBJECT)

VkBuffer
grd_vk_buffer_get_buffer (GrdVkBuffer *buffer)
{
  return buffer->vk_buffer;
}

GrdVkMemory *
grd_vk_buffer_get_memory (GrdVkBuffer *buffer)
{
  return buffer->memory;
}

GrdVkBuffer *
grd_vk_buffer_new (GrdVkDevice                  *device,
                   const GrdVkBufferDescriptor  *buffer_descriptor,
                   GError                      **error)
{
  VkDevice vk_device = grd_vk_device_get_device (device);
  g_autoptr (GrdVkBuffer) buffer = NULL;
  VkBufferCreateInfo buffer_create_info = {};
  VkBufferMemoryRequirementsInfo2 memory_requirements_info_2 = {};
  VkMemoryRequirements2 memory_requirements_2 = {};
  GrdVkMemoryDescriptor memory_descriptor = {};
  VkBindBufferMemoryInfo bind_buffer_memory_info = {};
  VkDeviceMemory vk_memory;
  VkResult vk_result;

  g_assert (vk_device != VK_NULL_HANDLE);
  g_assert (buffer_descriptor->size > 0);

  buffer = g_object_new (GRD_TYPE_VK_BUFFER, NULL);
  buffer->device = device;

  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.size =
    grd_vk_device_get_aligned_size (device, buffer_descriptor->size);
  buffer_create_info.usage = buffer_descriptor->usage_flags;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  vk_result = vkCreateBuffer (vk_device, &buffer_create_info, NULL,
                              &buffer->vk_buffer);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create buffer: %i", vk_result);
      return NULL;
    }
  g_assert (buffer->vk_buffer != VK_NULL_HANDLE);

  memory_requirements_info_2.sType =
    VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
  memory_requirements_info_2.buffer = buffer->vk_buffer;
  memory_requirements_2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;

  vkGetBufferMemoryRequirements2 (vk_device, &memory_requirements_info_2,
                                  &memory_requirements_2);

  memory_descriptor.memory_requirements_2 = &memory_requirements_2;
  memory_descriptor.memory_flags = buffer_descriptor->memory_flags;

  buffer->memory = grd_vk_memory_new (device, &memory_descriptor, error);
  if (!buffer->memory)
    return NULL;

  vk_memory = grd_vk_memory_get_device_memory (buffer->memory);

  bind_buffer_memory_info.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO;
  bind_buffer_memory_info.buffer = buffer->vk_buffer;
  bind_buffer_memory_info.memory = vk_memory;
  bind_buffer_memory_info.memoryOffset = 0;

  vk_result = vkBindBufferMemory2 (vk_device, 1, &bind_buffer_memory_info);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to bind buffer memory: %i", vk_result);
      return NULL;
    }

  return g_steal_pointer (&buffer);
}

static void
grd_vk_buffer_dispose (GObject *object)
{
  GrdVkBuffer *buffer = GRD_VK_BUFFER (object);
  VkDevice vk_device = grd_vk_device_get_device (buffer->device);

  g_assert (vk_device != VK_NULL_HANDLE);

  if (buffer->vk_buffer != VK_NULL_HANDLE)
    {
      vkDestroyBuffer (vk_device, buffer->vk_buffer, NULL);
      buffer->vk_buffer = VK_NULL_HANDLE;
    }

  g_clear_object (&buffer->memory);

  G_OBJECT_CLASS (grd_vk_buffer_parent_class)->dispose (object);
}

static void
grd_vk_buffer_init (GrdVkBuffer *buffer)
{
}

static void
grd_vk_buffer_class_init (GrdVkBufferClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_vk_buffer_dispose;
}
