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

#define GRD_TYPE_VK_QUEUE (grd_vk_queue_get_type ())
G_DECLARE_FINAL_TYPE (GrdVkQueue, grd_vk_queue,
                      GRD, VK_QUEUE, GObject)

GrdVkQueue *grd_vk_queue_new (GrdVkDevice *device,
                              uint32_t     queue_family_idx,
                              uint32_t     queue_idx);

uint32_t grd_vk_queue_get_queue_family_idx (GrdVkQueue *queue);

uint32_t grd_vk_queue_get_queue_idx (GrdVkQueue *queue);

gboolean grd_vk_queue_submit (GrdVkQueue           *queue,
                              const VkSubmitInfo2  *submit_infos_2,
                              uint32_t              n_submit_infos_2,
                              VkFence               vk_fence,
                              GError              **error);
