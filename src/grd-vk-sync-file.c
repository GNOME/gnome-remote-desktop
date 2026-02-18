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

#include "config.h"

#include "grd-vk-sync-file.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <xf86drm.h>

#include "grd-drm-utils.h"
#include "grd-vk-device.h"

struct _GrdVkSyncFile
{
  GrdVkDevice *device;

  VkSemaphore vk_semaphore;
};

VkSemaphore
grd_vk_sync_file_get_semaphore (GrdVkSyncFile *sync_file)
{
  return sync_file->vk_semaphore;
}

static int
acquire_syncfile_fd (GrdVkSyncFile  *sync_file,
                     int             syncobj_fd,
                     uint64_t        timeline_point,
                     GError        **error)
{
  int device_fd = grd_vk_device_get_drm_render_node_fd (sync_file->device);
  uint32_t syncobj_handle = 0;
  int syncfile_fd;

  if (!grd_create_drm_syncobj_handle (device_fd, syncobj_fd, &syncobj_handle,
                                      error))
    return -1;

  if (!grd_wait_for_drm_timeline_point (device_fd, syncobj_handle,
                                        timeline_point,
                                        error))
    {
      drmSyncobjDestroy (device_fd, syncobj_handle);
      return -1;
    }

  syncfile_fd = grd_export_drm_timeline_syncfile (device_fd, syncobj_handle,
                                                  timeline_point,
                                                  error);
  drmSyncobjDestroy (device_fd, syncobj_handle);

  return syncfile_fd;
}

gboolean
grd_vk_sync_file_import_syncobj_timeline_point (GrdVkSyncFile  *sync_file,
                                                int             syncobj_fd,
                                                uint64_t        timeline_point,
                                                GError        **error)
{
  VkDevice vk_device = grd_vk_device_get_device (sync_file->device);
  GrdVkDeviceFuncs *device_funcs =
    grd_vk_device_get_device_funcs (sync_file->device);
  VkImportSemaphoreFdInfoKHR import_semaphore_fd_info = {};
  g_autofd int syncfile_fd = -1;
  VkResult vk_result;

  syncfile_fd = acquire_syncfile_fd (sync_file,
                                     syncobj_fd, timeline_point,
                                     error);
  if (syncfile_fd == -1)
    return FALSE;

  import_semaphore_fd_info.sType =
    VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
  import_semaphore_fd_info.semaphore = sync_file->vk_semaphore;
  import_semaphore_fd_info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
  import_semaphore_fd_info.handleType =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  import_semaphore_fd_info.fd = syncfile_fd;

  vk_result = device_funcs->vkImportSemaphoreFdKHR (vk_device,
                                                    &import_semaphore_fd_info);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to import semaphore fd: %i", vk_result);
      return FALSE;
    }
  g_steal_fd (&syncfile_fd);

  return TRUE;
}

static gboolean
create_semaphore (GrdVkSyncFile  *sync_file,
                  GError        **error)
{
  VkDevice vk_device = grd_vk_device_get_device (sync_file->device);
  VkSemaphoreCreateInfo semaphore_create_info = {};
  VkResult vk_result;

  semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  vk_result = vkCreateSemaphore (vk_device, &semaphore_create_info, NULL,
                                 &sync_file->vk_semaphore);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create semaphore: %i", vk_result);
      return FALSE;
    }

  return TRUE;
}

GrdVkSyncFile *
grd_vk_sync_file_new (GrdVkDevice  *device,
                      GError      **error)
{
  g_autoptr (GrdVkSyncFile) sync_file = NULL;

  sync_file = g_new0 (GrdVkSyncFile, 1);
  sync_file->device = device;

  if (!create_semaphore (sync_file, error))
    return NULL;

  return g_steal_pointer (&sync_file);
}

void
grd_vk_sync_file_free (GrdVkSyncFile *sync_file)
{
  VkDevice vk_device = grd_vk_device_get_device (sync_file->device);

  if (sync_file->vk_semaphore != VK_NULL_HANDLE)
    {
      vkDestroySemaphore (vk_device, sync_file->vk_semaphore, NULL);
      sync_file->vk_semaphore = VK_NULL_HANDLE;
    }

  g_free (sync_file);
}
