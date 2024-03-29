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

#ifndef GRD_VK_UTILS_H
#define GRD_VK_UTILS_H

#include <gio/gio.h>
#include <vulkan/vulkan.h>

#include "grd-types.h"

void grd_vk_append_to_chain (void *chain,
                             void *element);

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

#endif /* GRD_VK_UTILS_H */
