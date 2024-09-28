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

#include "grd-vk-device.h"

#include <gio/gio.h>

#include "grd-hwaccel-vulkan.h"
#include "grd-vk-queue.h"
#include "grd-vk-utils.h"

#define MAX_DEVICE_QUEUES 6

typedef struct
{
  GrdVkQueue *queue;
  uint64_t ref_count;
} QueueInfo;

struct _GrdVkDevice
{
  GObject parent;

  VkPhysicalDevice vk_physical_device;
  GrdVkDeviceFeatures device_features;

  VkDevice vk_device;
  VkPipelineCache vk_pipeline_cache;

  uint32_t queue_family_idx;
  uint32_t max_queues;

  GMutex queue_pool_mutex;
  GHashTable *queue_table;

  float timestamp_period;
  VkDeviceSize non_coherent_atom_size;
  int64_t drm_render_node;

  GrdVkDeviceFuncs device_funcs;
  GrdVkShaderModules shader_modules;
};

G_DEFINE_TYPE (GrdVkDevice, grd_vk_device, G_TYPE_OBJECT)

VkPhysicalDevice
grd_vk_device_get_physical_device (GrdVkDevice *device)
{
  return device->vk_physical_device;
}

GrdVkDeviceFeatures
grd_vk_device_get_device_features (GrdVkDevice *device)
{
  return device->device_features;
}

VkDevice
grd_vk_device_get_device (GrdVkDevice *device)
{
  return device->vk_device;
}

VkPipelineCache
grd_vk_device_get_pipeline_cache (GrdVkDevice *device)
{
  return device->vk_pipeline_cache;
}

float
grd_vk_device_get_timestamp_period (GrdVkDevice *device)
{
  return device->timestamp_period;
}

int64_t
grd_vk_device_get_drm_render_node (GrdVkDevice *device)
{
  return device->drm_render_node;
}

GrdVkDeviceFuncs *
grd_vk_device_get_device_funcs (GrdVkDevice *device)
{
  return &device->device_funcs;
}

const GrdVkShaderModules *
grd_vk_device_get_shader_modules (GrdVkDevice *device)
{
  return &device->shader_modules;
}

VkDeviceSize
grd_vk_device_get_aligned_size (GrdVkDevice  *device,
                                VkDeviceSize  size)
{
  VkDeviceSize alignment = device->non_coherent_atom_size;

  return size + (size % alignment ? alignment - size % alignment : 0);
}

GrdVkQueue *
grd_vk_device_acquire_queue (GrdVkDevice *device)
{
  /* Least-used-queue queue info */
  QueueInfo *luq_queue_info = NULL;
  QueueInfo *queue_info;
  GHashTableIter iter;

  g_assert (device->max_queues > 0);

  g_mutex_lock (&device->queue_pool_mutex);
  g_hash_table_iter_init (&iter, device->queue_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &queue_info))
    {
      if (!luq_queue_info || queue_info->ref_count < luq_queue_info->ref_count)
        luq_queue_info = queue_info;
    }
  g_assert (luq_queue_info);

  g_assert (luq_queue_info->ref_count < UINT64_MAX);
  ++luq_queue_info->ref_count;
  g_mutex_unlock (&device->queue_pool_mutex);

  return luq_queue_info->queue;
}

void
grd_vk_device_release_queue (GrdVkDevice *device,
                             GrdVkQueue  *queue)
{
  uint32_t queue_idx = grd_vk_queue_get_queue_idx (queue);
  QueueInfo *queue_info = NULL;

  g_mutex_lock (&device->queue_pool_mutex);
  if (!g_hash_table_lookup_extended (device->queue_table,
                                     GUINT_TO_POINTER (queue_idx),
                                     NULL, (gpointer *) &queue_info))
    g_assert_not_reached ();

  g_assert (queue_info->ref_count > 0);
  --queue_info->ref_count;
  g_mutex_unlock (&device->queue_pool_mutex);
}

static void
get_queue_family_properties (VkPhysicalDevice           vk_physical_device,
                             VkQueueFamilyProperties2 **properties_2,
                             uint32_t                  *n_properties_2)
{
  uint32_t i;

  *properties_2 = NULL;
  *n_properties_2 = 0;

  vkGetPhysicalDeviceQueueFamilyProperties2 (vk_physical_device,
                                             n_properties_2, NULL);
  g_assert (*n_properties_2 > 0);

  *properties_2 = g_new0 (VkQueueFamilyProperties2, *n_properties_2);
  for (i = 0; i < *n_properties_2; ++i)
    (*properties_2)[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;

  vkGetPhysicalDeviceQueueFamilyProperties2 (vk_physical_device,
                                             n_properties_2, *properties_2);
}

static void
find_queue_family_with_most_queues (GrdVkDevice *device)
{
  VkPhysicalDevice vk_physical_device = device->vk_physical_device;
  g_autofree VkQueueFamilyProperties2 *properties_2 = NULL;
  uint32_t n_properties_2 = 0;
  VkQueueFlags bitmask;
  uint32_t i;

  g_assert (device->max_queues == 0);

  get_queue_family_properties (vk_physical_device,
                               &properties_2, &n_properties_2);

  bitmask = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;

  for (i = 0; i < n_properties_2; ++i)
    {
      VkQueueFamilyProperties *properties =
        &properties_2[i].queueFamilyProperties;

      if (properties->queueFlags & bitmask &&
          properties->queueCount > device->max_queues)
        {
          device->queue_family_idx = i;
          device->max_queues = properties->queueCount;
        }
    }
  g_assert (device->max_queues > 0);
}

static gboolean
create_pipeline_cache (GrdVkDevice  *device,
                       GError      **error)
{
  VkDevice vk_device = device->vk_device;
  VkPipelineCacheCreateInfo cache_create_info = {};
  VkResult vk_result;

  cache_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

  vk_result = vkCreatePipelineCache (vk_device, &cache_create_info, NULL,
                                     &device->vk_pipeline_cache);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create pipeline cache: %i", vk_result);
      return FALSE;
    }
  g_assert (device->vk_pipeline_cache != VK_NULL_HANDLE);

  return TRUE;
}

static gboolean
load_device_funcs (GrdVkDevice  *device,
                   GError      **error)
{
  GrdVkDeviceFuncs *device_funcs = &device->device_funcs;
  VkDevice vk_device = device->vk_device;

  /* VK_KHR_external_memory_fd */
  device_funcs->vkGetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)
    vkGetDeviceProcAddr (vk_device, "vkGetMemoryFdPropertiesKHR");
  if (!device_funcs->vkGetMemoryFdPropertiesKHR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to get device function address for function "
                   "\"vkGetMemoryFdPropertiesKHR\"");
      return FALSE;
    }

  /* VK_KHR_synchronization2 */
  device_funcs->vkCmdPipelineBarrier2KHR = (PFN_vkCmdPipelineBarrier2KHR)
    vkGetDeviceProcAddr (vk_device, "vkCmdPipelineBarrier2KHR");
  if (!device_funcs->vkCmdPipelineBarrier2KHR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to get device function address for function "
                   "\"vkCmdPipelineBarrier2KHR\"");
      return FALSE;
    }

  device_funcs->vkCmdWriteTimestamp2KHR = (PFN_vkCmdWriteTimestamp2KHR)
    vkGetDeviceProcAddr (vk_device, "vkCmdWriteTimestamp2KHR");
  if (!device_funcs->vkCmdWriteTimestamp2KHR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to get device function address for function "
                   "\"vkCmdWriteTimestamp2KHR\"");
      return FALSE;
    }

  device_funcs->vkQueueSubmit2KHR = (PFN_vkQueueSubmit2KHR)
    vkGetDeviceProcAddr (vk_device, "vkQueueSubmit2KHR");
  if (!device_funcs->vkQueueSubmit2KHR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to get device function address for function "
                   "\"vkQueueSubmit2KHR\"");
      return FALSE;
    }

  return TRUE;
}

static gboolean
create_shader_module (GrdVkDevice             *device,
                      const GrdVkSPIRVSource  *spirv_source,
                      VkShaderModule          *shader_module,
                      GError                 **error)
{
  VkDevice vk_device = device->vk_device;
  VkShaderModuleCreateInfo shader_module_create_info = {};
  VkResult vk_result;

  g_assert (shader_module);
  g_assert (spirv_source->size % 4 == 0);

  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.codeSize = spirv_source->size;
  shader_module_create_info.pCode = (const uint32_t *) spirv_source->data;

  vk_result = vkCreateShaderModule (vk_device, &shader_module_create_info, NULL,
                                    shader_module);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create shader module: %i", vk_result);
      return FALSE;
    }
  g_assert (*shader_module != VK_NULL_HANDLE);

  return TRUE;
}

static gboolean
create_shader_modules (GrdVkDevice              *device,
                       const GrdVkSPIRVSources  *spirv_sources,
                       GError                  **error)
{
  GrdVkShaderModules *shader_modules = &device->shader_modules;

  if (!create_shader_module (device, spirv_sources->avc_main_view,
                             &shader_modules->create_avc_main_view, error))
    return FALSE;
  if (!create_shader_module (device, spirv_sources->avc_stereo_view,
                             &shader_modules->create_avc_stereo_view, error))
    return FALSE;

  return TRUE;
}

static void
prepare_queue_pool (GrdVkDevice *device)
{
  uint32_t i;

  g_assert (device->max_queues > 0);

  for (i = 0; i < device->max_queues; ++i)
    {
      QueueInfo *queue_info;

      queue_info = g_new0 (QueueInfo, 1);
      queue_info->queue =
        grd_vk_queue_new (device, device->queue_family_idx, i);

      g_hash_table_insert (device->queue_table,
                           GUINT_TO_POINTER (i), queue_info);
    }
  g_assert (g_hash_table_size (device->queue_table) == device->max_queues);
}

static void
fetch_device_properties (GrdVkDevice *device)
{
  VkPhysicalDeviceProperties properties = {};
  VkPhysicalDeviceProperties2 properties_2 = {};
  VkPhysicalDeviceDrmPropertiesEXT drm_properties = {};

  vkGetPhysicalDeviceProperties (device->vk_physical_device, &properties);
  device->timestamp_period = properties.limits.timestampPeriod;
  device->non_coherent_atom_size = properties.limits.nonCoherentAtomSize;

  g_assert (device->timestamp_period > 0);

  properties_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  drm_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;

  grd_vk_append_to_chain (&properties_2, &drm_properties);

  vkGetPhysicalDeviceProperties2 (device->vk_physical_device, &properties_2);
  device->drm_render_node = drm_properties.renderMinor;
}

GrdVkDevice *
grd_vk_device_new (VkPhysicalDevice          vk_physical_device,
                   GrdVkDeviceFeatures       device_features,
                   const GrdVkSPIRVSources  *spirv_sources,
                   GError                  **error)
{
  g_autoptr (GrdVkDevice) device = NULL;
  VkDeviceCreateInfo device_create_info = {};
  VkPhysicalDeviceVulkan12Features vulkan12_features = {};
  VkPhysicalDeviceSynchronization2Features synchronization2_features = {};
  VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures zero_init_features = {};
  VkDeviceQueueCreateInfo device_queue_create_info = {};
  float queue_priorities[MAX_DEVICE_QUEUES] = {};
  const char *extensions[6] = {};
  uint32_t n_extensions = 0;
  VkResult vk_result;
  uint32_t i;

  device = g_object_new (GRD_TYPE_VK_DEVICE, NULL);
  device->vk_physical_device = vk_physical_device;
  device->device_features = device_features;

  find_queue_family_with_most_queues (device);
  device->max_queues = MIN (device->max_queues, MAX_DEVICE_QUEUES);

  for (i = 0; i < device->max_queues; ++i)
    queue_priorities[i] = 1.0f;

  device_queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  device_queue_create_info.queueFamilyIndex = device->queue_family_idx;
  device_queue_create_info.queueCount = device->max_queues;
  device_queue_create_info.pQueuePriorities = queue_priorities;

  extensions[n_extensions++] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
  extensions[n_extensions++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
  extensions[n_extensions++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
  extensions[n_extensions++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
  extensions[n_extensions++] = VK_KHR_ZERO_INITIALIZE_WORKGROUP_MEMORY_EXTENSION_NAME;
  extensions[n_extensions++] = VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME;

  device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_create_info.queueCreateInfoCount = 1;
  device_create_info.pQueueCreateInfos = &device_queue_create_info;
  device_create_info.enabledExtensionCount = n_extensions;
  device_create_info.ppEnabledExtensionNames = extensions;

  vulkan12_features.sType =
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

  if (device_features & GRD_VK_DEVICE_FEATURE_UPDATE_AFTER_BIND_SAMPLED_IMAGE)
    vulkan12_features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
  if (device_features & GRD_VK_DEVICE_FEATURE_UPDATE_AFTER_BIND_STORAGE_IMAGE)
    vulkan12_features.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;

  synchronization2_features.sType =
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
  synchronization2_features.synchronization2 = VK_TRUE;

  zero_init_features.sType =
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES;
  zero_init_features.shaderZeroInitializeWorkgroupMemory = VK_TRUE;

  grd_vk_append_to_chain (&device_create_info, &vulkan12_features);
  grd_vk_append_to_chain (&device_create_info, &synchronization2_features);
  grd_vk_append_to_chain (&device_create_info, &zero_init_features);

  vk_result = vkCreateDevice (vk_physical_device, &device_create_info, NULL,
                              &device->vk_device);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create device: %i", vk_result);
      return NULL;
    }
  g_assert (device->vk_device != VK_NULL_HANDLE);

  if (!create_pipeline_cache (device, error))
    return NULL;
  if (!load_device_funcs (device, error))
    return NULL;
  if (!create_shader_modules (device, spirv_sources, error))
    return NULL;

  prepare_queue_pool (device);
  fetch_device_properties (device);

  g_debug ("[HWAccel.Vulkan] Using device features: 0x%08X",
           device->device_features);

  return g_steal_pointer (&device);
}

static void
clear_shader_module (GrdVkDevice    *device,
                     VkShaderModule *vk_shader_module)
{
  VkDevice vk_device = device->vk_device;

  g_assert (vk_device != VK_NULL_HANDLE);

  if (*vk_shader_module == VK_NULL_HANDLE)
    return;

  vkDestroyShaderModule (vk_device, *vk_shader_module, NULL);
  vk_shader_module = VK_NULL_HANDLE;
}

static void
destroy_shader_modules (GrdVkDevice *device)
{
  GrdVkShaderModules *shader_modules = &device->shader_modules;

  clear_shader_module (device, &shader_modules->create_avc_stereo_view);
  clear_shader_module (device, &shader_modules->create_avc_main_view);
}

static void
grd_vk_device_dispose (GObject *object)
{
  GrdVkDevice *device = GRD_VK_DEVICE (object);

  g_hash_table_remove_all (device->queue_table);

  if (device->vk_device != VK_NULL_HANDLE)
    destroy_shader_modules (device);

  if (device->vk_pipeline_cache != VK_NULL_HANDLE)
    {
      vkDestroyPipelineCache (device->vk_device, device->vk_pipeline_cache,
                              NULL);
      device->vk_pipeline_cache = VK_NULL_HANDLE;
    }
  if (device->vk_device != VK_NULL_HANDLE)
    {
      vkDestroyDevice (device->vk_device, NULL);
      device->vk_device = VK_NULL_HANDLE;
    }

  G_OBJECT_CLASS (grd_vk_device_parent_class)->dispose (object);
}

static void
grd_vk_device_finalize (GObject *object)
{
  GrdVkDevice *device = GRD_VK_DEVICE (object);

  g_mutex_clear (&device->queue_pool_mutex);

  g_assert (g_hash_table_size (device->queue_table) == 0);
  g_clear_pointer (&device->queue_table, g_hash_table_unref);

  G_OBJECT_CLASS (grd_vk_device_parent_class)->finalize (object);
}

static void
queue_info_free (gpointer data)
{
  QueueInfo *queue_info = data;

  g_assert (queue_info->ref_count == 0);
  g_clear_object (&queue_info->queue);

  g_free (queue_info);
}

static void
grd_vk_device_init (GrdVkDevice *device)
{
  g_assert (MAX_DEVICE_QUEUES > 0);

  device->queue_table = g_hash_table_new_full (NULL, NULL,
                                               NULL, queue_info_free);

  g_mutex_init (&device->queue_pool_mutex);
}

static void
grd_vk_device_class_init (GrdVkDeviceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_vk_device_dispose;
  object_class->finalize = grd_vk_device_finalize;
}
