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

#define GRD_TYPE_VK_IMAGE (grd_vk_image_get_type ())
G_DECLARE_FINAL_TYPE (GrdVkImage, grd_vk_image,
                      GRD, VK_IMAGE, GObject)

typedef struct
{
  VkImageCreateInfo *image_create_info;
  VkMemoryPropertyFlagBits memory_flags;

  /* Only for import operations */
  VkExternalMemoryHandleTypeFlagBits import_handle_type;
  int fd;

  uint64_t drm_format_modifier;
  gboolean has_drm_format_modifier;
} GrdVkImageDescriptor;

GrdVkImage *grd_vk_image_new (GrdVkDevice                 *device,
                              const GrdVkImageDescriptor  *image_descriptor,
                              GError                     **error);

VkImage grd_vk_image_get_image (GrdVkImage *image);

VkImageView grd_vk_image_get_image_view (GrdVkImage *image);

VkImageLayout grd_vk_image_get_image_layout (GrdVkImage *image);

void grd_vk_image_set_image_layout (GrdVkImage    *image,
                                    VkImageLayout  vk_image_layout);
