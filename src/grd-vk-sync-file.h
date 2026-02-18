/*
 * Copyright (C) 2026 Pascal Nowack
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

#include "grd-types.h"

GrdVkSyncFile *grd_vk_sync_file_new (GrdVkDevice  *device,
                                     GError      **error);

void grd_vk_sync_file_free (GrdVkSyncFile *sync_file);

VkSemaphore grd_vk_sync_file_get_semaphore (GrdVkSyncFile *sync_file);

gboolean grd_vk_sync_file_import_syncobj_timeline_point (GrdVkSyncFile  *sync_file,
                                                         int             syncobj_fd,
                                                         uint64_t        timeline_point,
                                                         GError        **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdVkSyncFile, grd_vk_sync_file_free)
