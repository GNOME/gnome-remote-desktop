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

#include "grd-hwaccel-vulkan.h"

#include <gio/gio.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

#include "grd-debug.h"
#include "grd-egl-thread.h"
#include "grd-vk-utils.h"

#define VULKAN_API_VERSION VK_API_VERSION_1_2

struct _GrdHwAccelVulkan
{
  GObject parent;

  VkInstance vk_instance;

  /* VK_EXT_debug_utils */
  PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT;
  PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;

  VkDebugUtilsMessengerEXT vk_debug_messenger;

  VkPhysicalDevice vk_physical_device;

  /* Physical device features */
  gboolean supports_sampled_image_update_after_bind;
  gboolean supports_storage_image_update_after_bind;

  GrdVkSPIRVSources spirv_sources;
};

G_DEFINE_TYPE (GrdHwAccelVulkan, grd_hwaccel_vulkan, G_TYPE_OBJECT)

GrdVkDevice *
grd_hwaccel_vulkan_acquire_device (GrdHwAccelVulkan  *hwaccel_vulkan,
                                   GError           **error)
{
  GrdVkDeviceFeatures device_features = 0;

  if (hwaccel_vulkan->supports_sampled_image_update_after_bind)
    device_features |= GRD_VK_DEVICE_FEATURE_UPDATE_AFTER_BIND_SAMPLED_IMAGE;
  if (hwaccel_vulkan->supports_storage_image_update_after_bind)
    device_features |= GRD_VK_DEVICE_FEATURE_UPDATE_AFTER_BIND_STORAGE_IMAGE;

  return grd_vk_device_new (hwaccel_vulkan->vk_physical_device, device_features,
                            &hwaccel_vulkan->spirv_sources, error);
}

static gboolean
has_vk_layer (VkLayerProperties *properties,
              uint32_t           n_properties,
              const char        *extension)
{
  uint32_t i;

  for (i = 0; i < n_properties; ++i)
    {
      if (strcmp (properties[i].layerName, extension) == 0)
        return TRUE;
    }

  return FALSE;
}

static gboolean
check_instance_layers (GrdHwAccelVulkan  *hwaccel_vulkan,
                       gboolean          *has_validation_layer,
                       GError           **error)
{
  g_autofree VkLayerProperties *properties = NULL;
  uint32_t n_properties = 0;
  VkResult vk_result;

  *has_validation_layer = FALSE;

  vk_result = vkEnumerateInstanceLayerProperties (&n_properties, NULL);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to enumerate instance layer properties: %i",
                   vk_result);
      return FALSE;
    }
  if (n_properties == 0)
    return TRUE;

  properties = g_new0 (VkLayerProperties, n_properties);
  vk_result = vkEnumerateInstanceLayerProperties (&n_properties, properties);
  if (vk_result == VK_INCOMPLETE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Instance layer properties changed during count fetch");
      return FALSE;
    }
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to enumerate instance layer properties: %i",
                   vk_result);
      return FALSE;
    }

  *has_validation_layer = has_vk_layer (properties, n_properties,
                                        "VK_LAYER_KHRONOS_validation");

  return TRUE;
}

static gboolean
has_vk_extension (VkExtensionProperties *properties,
                  uint32_t               n_properties,
                  const char            *extension)
{
  uint32_t i;

  for (i = 0; i < n_properties; ++i)
    {
      if (strcmp (properties[i].extensionName, extension) == 0)
        return TRUE;
    }

  return FALSE;
}

static gboolean
check_instance_extensions (GrdHwAccelVulkan  *hwaccel_vulkan,
                           gboolean          *supports_debug_utils,
                           GError           **error)
{
  g_autofree VkExtensionProperties *properties = NULL;
  uint32_t n_properties = 0;
  VkResult vk_result;

  *supports_debug_utils = FALSE;

  vk_result = vkEnumerateInstanceExtensionProperties (NULL,
                                                      &n_properties,
                                                      NULL);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to enumerate instance extension properties: %i",
                   vk_result);
      return FALSE;
    }
  if (n_properties == 0)
    return TRUE;

  properties = g_new0 (VkExtensionProperties, n_properties);
  vk_result = vkEnumerateInstanceExtensionProperties (NULL,
                                                      &n_properties,
                                                      properties);
  if (vk_result == VK_INCOMPLETE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Instance extension properties changed during count fetch");
      return FALSE;
    }
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to enumerate instance extension properties: %i",
                   vk_result);
      return FALSE;
    }

  *supports_debug_utils = has_vk_extension (properties, n_properties,
                                            VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  return TRUE;
}

static gboolean
load_instance_debug_funcs (GrdHwAccelVulkan  *hwaccel_vulkan,
                           GError           **error)
{
  VkInstance vk_instance = hwaccel_vulkan->vk_instance;

  hwaccel_vulkan->vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)
    vkGetInstanceProcAddr (vk_instance, "vkCreateDebugUtilsMessengerEXT");
  if (!hwaccel_vulkan->vkCreateDebugUtilsMessengerEXT)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to get instance function address for function "
                   "\"vkCreateDebugUtilsMessengerEXT\"");
      return FALSE;
    }

  hwaccel_vulkan->vkDestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)
    vkGetInstanceProcAddr (vk_instance, "vkDestroyDebugUtilsMessengerEXT");
  if (!hwaccel_vulkan->vkDestroyDebugUtilsMessengerEXT)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to get instance function address for function "
                   "\"vkDestroyDebugUtilsMessengerEXT\"");
      return FALSE;
    }

  return TRUE;
}

static const char *
message_severity_to_string (VkDebugUtilsMessageSeverityFlagBitsEXT message_severity)
{
  switch (message_severity)
    {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
      return "Verbose";
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
      return "Info";
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
      return "Warning";
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
      return "Error";
    default:
      g_assert_not_reached ();
      return NULL;
    }
}

static VkBool32
debug_messenger (VkDebugUtilsMessageSeverityFlagBitsEXT      message_severity,
                 VkDebugUtilsMessageTypeFlagsEXT             message_type_flags,
                 const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
                 void*                                       user_data)
{
  g_debug ("[HWAccel.Vulkan] Debug[%s]: Types: 0x%08X: Id: [%i: %s]: %s",
           message_severity_to_string (message_severity), message_type_flags,
           callback_data->messageIdNumber, callback_data->pMessageIdName,
           callback_data->pMessage);

  return VK_FALSE;
}

static gboolean
create_debug_messenger (GrdHwAccelVulkan  *hwaccel_vulkan,
                        GError           **error)
{
  VkInstance vk_instance = hwaccel_vulkan->vk_instance;
  VkDebugUtilsMessengerCreateInfoEXT create_info = {};
  VkDebugUtilsMessengerEXT vk_debug_messenger = VK_NULL_HANDLE;
  VkResult vk_result;

  g_assert (vk_instance != VK_NULL_HANDLE);
  g_assert (hwaccel_vulkan->vkCreateDebugUtilsMessengerEXT);

  create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  create_info.pfnUserCallback = debug_messenger;
  create_info.pUserData = NULL;

  vk_result =
    hwaccel_vulkan->vkCreateDebugUtilsMessengerEXT (vk_instance,
                                                    &create_info, NULL,
                                                    &vk_debug_messenger);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create debug messenger: %i", vk_result);
      return FALSE;
    }
  g_assert (vk_debug_messenger != VK_NULL_HANDLE);

  hwaccel_vulkan->vk_debug_messenger = vk_debug_messenger;

  return TRUE;
}

static gboolean
create_vk_instance (GrdHwAccelVulkan  *hwaccel_vulkan,
                    GError           **error)
{
  VkInstanceCreateInfo instance_create_info = {};
  VkApplicationInfo app_info = {};
  gboolean has_validation_layer = FALSE;
  gboolean supports_debug_utils = FALSE;
  gboolean vulkan_debug = FALSE;
  const char *layers[1] = {};
  uint32_t n_layers = 0;
  const char *extensions[1] = {};
  uint32_t n_extensions = 0;
  VkResult vk_result;

  if (grd_get_debug_flags () & GRD_DEBUG_VK_VALIDATION)
    vulkan_debug = TRUE;

  if (!check_instance_layers (hwaccel_vulkan, &has_validation_layer, error))
    return FALSE;
  if (!check_instance_extensions (hwaccel_vulkan, &supports_debug_utils, error))
    return FALSE;

  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.apiVersion = VULKAN_API_VERSION;

  if (vulkan_debug)
    {
      if (has_validation_layer)
        layers[n_layers++] = "VK_LAYER_KHRONOS_validation";
      else
        g_warning ("[HWAccel.Vulkan] VK_LAYER_KHRONOS_validation is unavailable");

      if (supports_debug_utils)
        extensions[n_extensions++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
      else
        g_warning ("[HWAccel.Vulkan] VK_EXT_debug_utils is unavailable");
    }

  instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_create_info.pApplicationInfo = &app_info;
  instance_create_info.enabledLayerCount = n_layers;
  instance_create_info.ppEnabledLayerNames = layers;
  instance_create_info.enabledExtensionCount = n_extensions;
  instance_create_info.ppEnabledExtensionNames = extensions;

  vk_result = vkCreateInstance (&instance_create_info, NULL,
                                &hwaccel_vulkan->vk_instance);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create instance: %i", vk_result);
      return FALSE;
    }
  g_assert (hwaccel_vulkan->vk_instance != VK_NULL_HANDLE);

  if (!vulkan_debug || !supports_debug_utils)
    return TRUE;

  if (!load_instance_debug_funcs (hwaccel_vulkan, error))
    return FALSE;
  if (!create_debug_messenger (hwaccel_vulkan, error))
    return FALSE;

  return TRUE;
}

static gboolean
check_device_extension (VkExtensionProperties *properties,
                        uint32_t               n_properties,
                        const char            *extension)
{
  if (!has_vk_extension (properties, n_properties, extension))
    {
      g_debug ("[HWAccel.Vulkan] Skipping device. Missing extension '%s'",
               extension);
      return FALSE;
    }

  return TRUE;
}

static gboolean
check_device_extensions (VkPhysicalDevice vk_physical_device)
{
  g_autofree VkExtensionProperties *properties = NULL;
  uint32_t n_properties = 0;
  VkResult vk_result;

  vk_result = vkEnumerateDeviceExtensionProperties (vk_physical_device, NULL,
                                                    &n_properties, NULL);
  if (vk_result != VK_SUCCESS)
    {
      g_warning ("[HWAccel.Vulkan] Failed to enumerate device extension "
                 "properties: %i", vk_result);
      return FALSE;
    }
  if (n_properties == 0)
    return TRUE;

  properties = g_new0 (VkExtensionProperties, n_properties);
  vk_result = vkEnumerateDeviceExtensionProperties (vk_physical_device, NULL,
                                                    &n_properties, properties);
  if (vk_result != VK_SUCCESS)
    {
      g_warning ("[HWAccel.Vulkan] Failed to enumerate device extension "
                 "properties: %i", vk_result);
      return FALSE;
    }

  if (!check_device_extension (properties, n_properties,
                               VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME))
    return FALSE;
  if (!check_device_extension (properties, n_properties,
                               VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))
    return FALSE;
  if (!check_device_extension (properties, n_properties,
                               VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME))
    return FALSE;
  if (!check_device_extension (properties, n_properties,
                               VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME))
    return FALSE;
  if (!check_device_extension (properties, n_properties,
                               VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME))
    return FALSE;
  if (!check_device_extension (properties, n_properties,
                               VK_KHR_ZERO_INITIALIZE_WORKGROUP_MEMORY_EXTENSION_NAME))
    return FALSE;
  if (!check_device_extension (properties, n_properties,
                               VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME))
    return FALSE;

  return TRUE;
}

static gboolean
get_queue_family_properties (VkPhysicalDevice           vk_physical_device,
                             VkQueueFamilyProperties2 **properties_2,
                             uint32_t                  *n_properties_2)
{
  uint32_t i;

  *properties_2 = NULL;
  *n_properties_2 = 0;

  vkGetPhysicalDeviceQueueFamilyProperties2 (vk_physical_device,
                                             n_properties_2, NULL);
  if (*n_properties_2 == 0)
    return FALSE;

  *properties_2 = g_new0 (VkQueueFamilyProperties2, *n_properties_2);
  for (i = 0; i < *n_properties_2; ++i)
    (*properties_2)[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;

  vkGetPhysicalDeviceQueueFamilyProperties2 (vk_physical_device,
                                             n_properties_2, *properties_2);

  return TRUE;
}

static gboolean
has_queue_family_with_bitmask (VkPhysicalDevice vk_physical_device,
                               VkQueueFlags     bitmask)
{
  g_autofree VkQueueFamilyProperties2 *properties_2 = NULL;
  uint32_t n_properties_2 = 0;
  uint32_t i;

  if (!get_queue_family_properties (vk_physical_device, &properties_2,
                                    &n_properties_2))
    return FALSE;

  for (i = 0; i < n_properties_2; ++i)
    {
      if (properties_2[i].queueFamilyProperties.queueFlags & bitmask)
        return TRUE;
    }

  return FALSE;
}

static gboolean
check_physical_device (GrdHwAccelVulkan *hwaccel_vulkan,
                       VkPhysicalDevice  vk_physical_device,
                       int64_t           render_major_egl,
                       int64_t           render_minor_egl)
{
  VkPhysicalDeviceProperties properties = {};
  VkPhysicalDeviceProperties2 properties_2 = {};
  VkPhysicalDeviceDrmPropertiesEXT drm_properties = {};
  VkPhysicalDeviceFeatures2 features_2 = {};
  VkPhysicalDeviceVulkan12Features vulkan12_features = {};
  VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures zero_init_features = {};
  g_autoptr (GError) error = NULL;

  vkGetPhysicalDeviceProperties (vk_physical_device, &properties);
  if (properties.apiVersion < VULKAN_API_VERSION)
    {
      g_debug ("[HWAccel.Vulkan] Skipping device. API version too old "
               "(have %u, need %u)", properties.apiVersion, VULKAN_API_VERSION);
      return FALSE;
    }
  if (!properties.limits.timestampComputeAndGraphics)
    {
      g_debug ("[HWAccel.Vulkan] Skipping device. Support for "
               "'timestampComputeAndGraphics' is missing");
      return FALSE;
    }

  if (!check_device_extensions (vk_physical_device))
    return FALSE;

  properties_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  drm_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;

  grd_vk_append_to_chain (&properties_2, &drm_properties);

  vkGetPhysicalDeviceProperties2 (vk_physical_device, &properties_2);

  if (drm_properties.hasRender != VK_TRUE)
    {
      g_debug ("[HWAccel.Vulkan] Skipping device. Device has no "
               "DRM render node");
      return FALSE;
    }
  if (drm_properties.renderMajor != render_major_egl ||
      drm_properties.renderMinor != render_minor_egl)
    {
      g_debug ("[HWAccel.Vulkan] Skipping device. Vulkan and EGL device DRM "
               "render nodes don't match");
      return FALSE;
    }

  features_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  vulkan12_features.sType =
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  zero_init_features.sType =
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES;

  grd_vk_append_to_chain (&features_2, &vulkan12_features);
  grd_vk_append_to_chain (&features_2, &zero_init_features);

  vkGetPhysicalDeviceFeatures2 (vk_physical_device, &features_2);

  if (zero_init_features.shaderZeroInitializeWorkgroupMemory != VK_TRUE)
    {
      g_debug ("[HWAccel.Vulkan] Skipping device. Support for "
               "'shaderZeroInitializeWorkgroupMemory' is missing");
      return FALSE;
    }

  hwaccel_vulkan->supports_sampled_image_update_after_bind =
    !!vulkan12_features.descriptorBindingSampledImageUpdateAfterBind;
  hwaccel_vulkan->supports_storage_image_update_after_bind =
    !!vulkan12_features.descriptorBindingStorageImageUpdateAfterBind;

  if (!has_queue_family_with_bitmask (vk_physical_device,
                                      VK_QUEUE_COMPUTE_BIT |
                                      VK_QUEUE_TRANSFER_BIT))
    {
      g_debug ("[HWAccel.Vulkan] Skipping device. Missing device queue family "
               "with (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT) bitmask");
      return FALSE;
    }

  hwaccel_vulkan->vk_physical_device = vk_physical_device;

  return TRUE;
}

static gboolean
find_and_check_physical_device (GrdHwAccelVulkan        *hwaccel_vulkan,
                                const VkPhysicalDevice  *physical_devices,
                                uint32_t                 n_physical_devices,
                                int64_t                  render_major,
                                int64_t                  render_minor,
                                GError                 **error)
{
  uint32_t i;

  for (i = 0; i < n_physical_devices; ++i)
    {
      g_debug ("[HWAccel.Vulkan] Checking physical device %u/%u",
               i + 1, n_physical_devices);
      if (check_physical_device (hwaccel_vulkan, physical_devices[i],
                                 render_major, render_minor))
        return TRUE;
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Could not find proper device");

  return FALSE;
}

static gboolean
find_vulkan_device_from_egl_thread (GrdHwAccelVulkan  *hwaccel_vulkan,
                                    GrdEglThread      *egl_thread,
                                    GError           **error)
{
  g_autofree VkPhysicalDevice *physical_devices = NULL;
  uint32_t n_physical_devices = 0;
  const char *drm_render_node;
  struct stat stat_buf = {};
  VkResult vk_result;
  int ret;

  g_assert (egl_thread);

  vk_result = vkEnumeratePhysicalDevices (hwaccel_vulkan->vk_instance,
                                          &n_physical_devices, NULL);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to enumerate physical devices: %i", vk_result);
      return FALSE;
    }
  if (n_physical_devices == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No physical devices found: %i", vk_result);
      return FALSE;
    }

  physical_devices = g_new0 (VkPhysicalDevice, n_physical_devices);
  vk_result = vkEnumeratePhysicalDevices (hwaccel_vulkan->vk_instance,
                                          &n_physical_devices,
                                          physical_devices);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to enumerate physical devices: %i", vk_result);
      return FALSE;
    }

  drm_render_node = grd_egl_thread_get_drm_render_node (egl_thread);

  ret = stat (drm_render_node, &stat_buf);
  if (ret < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                   "Failed to check status of DRM render node: %s",
                   strerror (-ret));
      return FALSE;
    }
  if (!S_ISCHR (stat_buf.st_mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid file mode for DRM render node");
      return FALSE;
    }

  if (!find_and_check_physical_device (hwaccel_vulkan,
                                       physical_devices, n_physical_devices,
                                       major (stat_buf.st_rdev),
                                       minor (stat_buf.st_rdev),
                                       error))
    return FALSE;

  return TRUE;
}

GrdHwAccelVulkan *
grd_hwaccel_vulkan_new (GrdEglThread  *egl_thread,
                        GError       **error)
{
  g_autoptr (GrdHwAccelVulkan) hwaccel_vulkan = NULL;

  hwaccel_vulkan = g_object_new (GRD_TYPE_HWACCEL_VULKAN, NULL);

  if (!create_vk_instance (hwaccel_vulkan, error))
    return NULL;

  if (!find_vulkan_device_from_egl_thread (hwaccel_vulkan, egl_thread, error))
    return NULL;

  return g_steal_pointer (&hwaccel_vulkan);
}

static void
spirv_source_free (GrdVkSPIRVSource *spirv_source)
{
  g_free (spirv_source->data);
  g_free (spirv_source);
}

static void
free_spirv_sources (GrdHwAccelVulkan *hwaccel_vulkan)
{
  GrdVkSPIRVSources *spirv_sources = &hwaccel_vulkan->spirv_sources;

  g_clear_pointer (&spirv_sources->avc_dual_view, spirv_source_free);
}

static void
grd_hwaccel_vulkan_dispose (GObject *object)
{
  GrdHwAccelVulkan *hwaccel_vulkan = GRD_HWACCEL_VULKAN (object);

  free_spirv_sources (hwaccel_vulkan);

  if (hwaccel_vulkan->vk_debug_messenger != VK_NULL_HANDLE)
    {
      VkInstance vk_instance = hwaccel_vulkan->vk_instance;
      VkDebugUtilsMessengerEXT vk_debug_messenger =
        hwaccel_vulkan->vk_debug_messenger;

      g_assert (vk_instance != VK_NULL_HANDLE);
      g_assert (hwaccel_vulkan->vkDestroyDebugUtilsMessengerEXT);

      hwaccel_vulkan->vkDestroyDebugUtilsMessengerEXT (vk_instance,
                                                       vk_debug_messenger,
                                                       NULL);
      hwaccel_vulkan->vk_debug_messenger = VK_NULL_HANDLE;
    }

  if (hwaccel_vulkan->vk_instance != VK_NULL_HANDLE)
    {
      vkDestroyInstance (hwaccel_vulkan->vk_instance, NULL);
      hwaccel_vulkan->vk_instance = VK_NULL_HANDLE;
    }

  G_OBJECT_CLASS (grd_hwaccel_vulkan_parent_class)->dispose (object);
}

static gboolean
load_spirv_source (const char        *path,
                   GrdVkSPIRVSource **spirv_source,
                   GError           **error)
{
  char *data = NULL;
  size_t size = 0;

  if (!g_file_get_contents (path, &data, &size, error))
    return FALSE;

  /* SPIR-V sources are always aligned to 32 bits */
  g_assert (size % 4 == 0);

  *spirv_source = g_new0 (GrdVkSPIRVSource, 1);
  (*spirv_source)->data = data;
  (*spirv_source)->size = size;

  return TRUE;
}

static void
load_spirv_sources (GrdHwAccelVulkan *hwaccel_vulkan)
{
  GrdVkSPIRVSources *spirv_sources = &hwaccel_vulkan->spirv_sources;
  g_autofree char *avc_dual_view_path = NULL;
  g_autoptr (GError) error = NULL;

  avc_dual_view_path = g_strdup_printf ("%s/grd-avc-dual-view_opt.spv",
                                        GRD_SHADER_DIR);
  if (!load_spirv_source (avc_dual_view_path, &spirv_sources->avc_dual_view,
                          &error))
    g_error ("[HWAccel.Vulkan] Failed to load shader: %s", error->message);
}

static void
grd_hwaccel_vulkan_init (GrdHwAccelVulkan *hwaccel_vulkan)
{
  load_spirv_sources (hwaccel_vulkan);
}

static void
grd_hwaccel_vulkan_class_init (GrdHwAccelVulkanClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_hwaccel_vulkan_dispose;
}
