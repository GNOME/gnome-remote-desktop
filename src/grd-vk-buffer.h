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

#ifndef GRD_VK_BUFFER_H
#define GRD_VK_BUFFER_H

#include <glib-object.h>
#include <vulkan/vulkan.h>

#include "grd-types.h"

#define GRD_TYPE_VK_BUFFER (grd_vk_buffer_get_type ())
G_DECLARE_FINAL_TYPE (GrdVkBuffer, grd_vk_buffer,
                      GRD, VK_BUFFER, GObject)

typedef struct
{
  VkBufferUsageFlags usage_flags;

  VkDeviceSize size;
  VkMemoryPropertyFlagBits memory_flags;
} GrdVkBufferDescriptor;

GrdVkBuffer *grd_vk_buffer_new (GrdVkDevice                  *device,
                                const GrdVkBufferDescriptor  *buffer_descriptor,
                                GError                      **error);

VkBuffer grd_vk_buffer_get_buffer (GrdVkBuffer *buffer);

GrdVkMemory *grd_vk_buffer_get_memory (GrdVkBuffer *buffer);

#endif /* GRD_VK_BUFFER_H */
