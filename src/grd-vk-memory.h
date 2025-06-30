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

#define GRD_TYPE_VK_MEMORY (grd_vk_memory_get_type ())
G_DECLARE_FINAL_TYPE (GrdVkMemory, grd_vk_memory,
                      GRD, VK_MEMORY, GObject)

typedef struct
{
  VkMemoryRequirements2 *memory_requirements_2;
  VkMemoryPropertyFlagBits memory_flags;

  /* Only for import operations */
  VkExternalMemoryHandleTypeFlagBits import_handle_type;
  int fd;
} GrdVkMemoryDescriptor;

GrdVkMemory *grd_vk_memory_new (GrdVkDevice                  *device,
                                const GrdVkMemoryDescriptor  *memory_descriptor,
                                GError                      **error);

VkDeviceMemory grd_vk_memory_get_device_memory (GrdVkMemory *memory);

VkMemoryPropertyFlagBits grd_vk_memory_get_memory_flags (GrdVkMemory *memory);

void *grd_vk_memory_get_mapped_pointer (GrdVkMemory  *memory,
                                        GError      **error);
