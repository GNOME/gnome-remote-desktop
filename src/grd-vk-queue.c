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

#include "grd-vk-queue.h"

#include <gio/gio.h>

#include "grd-vk-device.h"

struct _GrdVkQueue
{
  GObject parent;

  GrdVkDevice *device;

  uint32_t queue_family_idx;
  uint32_t queue_idx;

  GMutex queue_mutex;
  VkQueue vk_queue;
};

G_DEFINE_TYPE (GrdVkQueue, grd_vk_queue, G_TYPE_OBJECT)

uint32_t
grd_vk_queue_get_queue_family_idx (GrdVkQueue *queue)
{
  return queue->queue_family_idx;
}

uint32_t
grd_vk_queue_get_queue_idx (GrdVkQueue *queue)
{
  return queue->queue_idx;
}

gboolean
grd_vk_queue_submit (GrdVkQueue           *queue,
                     const VkSubmitInfo2  *submit_infos_2,
                     uint32_t              n_submit_infos_2,
                     VkFence               vk_fence,
                     GError              **error)
{
  GrdVkDevice *device = queue->device;
  GrdVkDeviceFuncs *device_funcs = grd_vk_device_get_device_funcs (device);
  g_autoptr (GMutexLocker) locker = NULL;
  VkResult vk_result;

  g_assert (device_funcs->vkQueueSubmit2KHR);

  locker = g_mutex_locker_new (&queue->queue_mutex);
  vk_result = device_funcs->vkQueueSubmit2KHR (queue->vk_queue,
                                               n_submit_infos_2, submit_infos_2,
                                               vk_fence);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to submit command buffers to queue: %i", vk_result);
      return FALSE;
    }

  return TRUE;
}

GrdVkQueue *
grd_vk_queue_new (GrdVkDevice *device,
                  uint32_t     queue_family_idx,
                  uint32_t     queue_idx)
{
  VkDevice vk_device = grd_vk_device_get_device (device);
  GrdVkQueue *queue;
  VkDeviceQueueInfo2 device_queue_info_2 = {};

  g_assert (vk_device != VK_NULL_HANDLE);

  queue = g_object_new (GRD_TYPE_VK_QUEUE, NULL);
  queue->device = device;
  queue->queue_family_idx = queue_family_idx;
  queue->queue_idx = queue_idx;

  device_queue_info_2.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
  device_queue_info_2.queueFamilyIndex = queue_family_idx;
  device_queue_info_2.queueIndex = queue_idx;

  vkGetDeviceQueue2 (vk_device, &device_queue_info_2, &queue->vk_queue);
  g_assert (queue->vk_queue != VK_NULL_HANDLE);

  return queue;
}

static void
grd_vk_queue_finalize (GObject *object)
{
  GrdVkQueue *queue = GRD_VK_QUEUE (object);

  g_mutex_clear (&queue->queue_mutex);

  G_OBJECT_CLASS (grd_vk_queue_parent_class)->finalize (object);
}

static void
grd_vk_queue_init (GrdVkQueue *queue)
{
  g_mutex_init (&queue->queue_mutex);
}

static void
grd_vk_queue_class_init (GrdVkQueueClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_vk_queue_finalize;
}
