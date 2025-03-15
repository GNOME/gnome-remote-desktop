/*
 * Copyright (C) 2024 Pascal Nowack
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

#include <vulkan/vulkan.h>

#include "grd-image-view.h"
#include "grd-types.h"

#define GRD_TYPE_IMAGE_VIEW_NV12 (grd_image_view_nv12_get_type ())
G_DECLARE_FINAL_TYPE (GrdImageViewNV12, grd_image_view_nv12,
                      GRD, IMAGE_VIEW_NV12, GrdImageView)

GrdImageViewNV12 *grd_image_view_nv12_new (GrdVkImage *vk_y_layer,
                                           GrdVkImage *vk_uv_layer);

VkImageView grd_image_view_nv12_get_y_layer (GrdImageViewNV12 *image_view_nv12);

VkImageView grd_image_view_nv12_get_uv_layer (GrdImageViewNV12 *image_view_nv12);

GList *grd_image_view_nv12_get_images (GrdImageViewNV12 *image_view_nv12);

VkImageLayout grd_image_view_nv12_get_image_layout (GrdImageViewNV12 *image_view_nv12);

void grd_image_view_nv12_set_image_layout (GrdImageViewNV12 *image_view_nv12,
                                           VkImageLayout     vk_image_layout);
