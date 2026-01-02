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

#include "grd-vk-memory.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "grd-vk-device.h"
#include "grd-vk-physical-device.h"
#include "grd-vk-utils.h"

struct _GrdVkMemory
{
  GObject parent;

  GrdVkDevice *device;

  VkDeviceMemory vk_memory;
  VkMemoryPropertyFlagBits memory_flags;

  void *mapped_pointer;
};

G_DEFINE_TYPE (GrdVkMemory, grd_vk_memory, G_TYPE_OBJECT)

VkDeviceMemory
grd_vk_memory_get_device_memory (GrdVkMemory *memory)
{
  return memory->vk_memory;
}

VkMemoryPropertyFlagBits
grd_vk_memory_get_memory_flags (GrdVkMemory *memory)
{
  return memory->memory_flags;
}

void *
grd_vk_memory_get_mapped_pointer (GrdVkMemory  *memory,
                                  GError      **error)
{
  VkDevice vk_device = grd_vk_device_get_device (memory->device);
  VkResult vk_result;

  g_assert (memory->memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  g_assert (!memory->mapped_pointer);

  vk_result = vkMapMemory (vk_device, memory->vk_memory, 0, VK_WHOLE_SIZE, 0,
                           &memory->mapped_pointer);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to map memory: %i", vk_result);
      return NULL;
    }
  g_assert (memory->mapped_pointer);

  return memory->mapped_pointer;
}

static gboolean
prepare_fd_import (GrdVkMemory                  *memory,
                   const GrdVkMemoryDescriptor  *memory_descriptor,
                   uint32_t                     *memory_type_bits,
                   VkImportMemoryFdInfoKHR      *import_memory_fd_info,
                   GError                      **error)
{
  GrdVkDeviceFuncs *device_funcs = grd_vk_device_get_device_funcs (memory->device);
  VkDevice vk_device = grd_vk_device_get_device (memory->device);
  VkExternalMemoryHandleTypeFlagBits handle_type =
    memory_descriptor->import_handle_type;
  VkMemoryFdPropertiesKHR fd_properties = {};
  VkResult vk_result;

  g_assert (vk_device != VK_NULL_HANDLE);
  g_assert (device_funcs->vkGetMemoryFdPropertiesKHR);

  g_assert (handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
            handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
  g_assert (memory_descriptor->fd != -1);

  fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;

  vk_result = device_funcs->vkGetMemoryFdPropertiesKHR (vk_device, handle_type,
                                                        memory_descriptor->fd,
                                                        &fd_properties);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to get memory fd properties: %s",
                   g_strerror (errno));
      return FALSE;
    }
  *memory_type_bits = fd_properties.memoryTypeBits;

  import_memory_fd_info->sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  import_memory_fd_info->handleType = handle_type;

  /*
   * 11.2.5. File Descriptor External Memory
   *
   * Importing memory from a file descriptor transfers ownership of the file
   * descriptor from the application to the Vulkan implementation.
   * The application must not perform any operations on the file descriptor
   * after a successful import.
   * The imported memory object holds a reference to its payload.
   */
  import_memory_fd_info->fd = dup (memory_descriptor->fd);

  if (import_memory_fd_info->fd == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to duplicate fd: %s", g_strerror (errno));
      return FALSE;
    }
  g_assert (import_memory_fd_info->fd != -1);

  return TRUE;
}

static gboolean
get_memory_type_index (GrdVkMemory               *memory,
                       uint32_t                   memory_type_bits,
                       VkMemoryPropertyFlagBits   memory_flags,
                       uint32_t                  *memory_type_idx,
                       GError                   **error)
{
  GrdVkPhysicalDevice *physical_device =
    grd_vk_device_get_physical_device (memory->device);
  VkPhysicalDevice vk_physical_device =
    grd_vk_physical_device_get_physical_device (physical_device);
  VkPhysicalDeviceMemoryProperties phys_dev_mem_props = {};
  uint32_t i = 0;

  vkGetPhysicalDeviceMemoryProperties (vk_physical_device, &phys_dev_mem_props);

  for (i = 0; i < phys_dev_mem_props.memoryTypeCount; ++i)
    {
      VkMemoryType memory_type = phys_dev_mem_props.memoryTypes[i];

      if (memory_type_bits & 1 << i &&
          (memory_type.propertyFlags & memory_flags) == memory_flags)
        {
          *memory_type_idx = i;
          memory->memory_flags = memory_type.propertyFlags;
          return TRUE;
        }
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Failed to find memory type index");

  return FALSE;
}

GrdVkMemory *
grd_vk_memory_new (GrdVkDevice                  *device,
                   const GrdVkMemoryDescriptor  *memory_descriptor,
                   GError                      **error)
{
  VkDevice vk_device = grd_vk_device_get_device (device);
  const VkMemoryRequirements *memory_requirements =
    &memory_descriptor->memory_requirements_2->memoryRequirements;
  g_autoptr (GrdVkMemory) memory = NULL;
  VkMemoryAllocateInfo memory_allocate_info = {};
  VkImportMemoryFdInfoKHR import_memory_fd_info = {};
  uint32_t memory_type_bits;
  VkResult vk_result;

  import_memory_fd_info.fd = -1;

  memory = g_object_new (GRD_TYPE_VK_MEMORY, NULL);
  memory->device = device;

  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.allocationSize = memory_requirements->size;

  memory_type_bits = memory_requirements->memoryTypeBits;
  if (memory_descriptor->import_handle_type)
    {
      if (!prepare_fd_import (memory, memory_descriptor, &memory_type_bits,
                              &import_memory_fd_info, error))
        return NULL;

      grd_vk_append_to_chain (&memory_allocate_info, &import_memory_fd_info);
    }

  if (!get_memory_type_index (memory, memory_type_bits,
                              memory_descriptor->memory_flags,
                              &memory_allocate_info.memoryTypeIndex, error))
    {
      g_clear_fd (&import_memory_fd_info.fd, NULL);
      return NULL;
    }

  vk_result = vkAllocateMemory (vk_device, &memory_allocate_info, NULL,
                                &memory->vk_memory);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to allocate device memory: %i", vk_result);
      g_clear_fd (&import_memory_fd_info.fd, NULL);
      return NULL;
    }
  g_assert (memory->vk_memory != VK_NULL_HANDLE);

  return g_steal_pointer (&memory);
}

static void
grd_vk_memory_dispose (GObject *object)
{
  GrdVkMemory *memory = GRD_VK_MEMORY (object);
  VkDevice vk_device = grd_vk_device_get_device (memory->device);

  g_assert (vk_device != VK_NULL_HANDLE);

  if (memory->vk_memory != VK_NULL_HANDLE)
    {
      vkFreeMemory (vk_device, memory->vk_memory, NULL);
      memory->vk_memory = VK_NULL_HANDLE;
    }

  G_OBJECT_CLASS (grd_vk_memory_parent_class)->dispose (object);
}

static void
grd_vk_memory_init (GrdVkMemory *memory)
{
}

static void
grd_vk_memory_class_init (GrdVkMemoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_vk_memory_dispose;
}
