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

#include "config.h"

#include "grd-rdp-view-creator-avc.h"

#include "grd-debug.h"
#include "grd-image-view-nv12.h"
#include "grd-rdp-buffer.h"
#include "grd-rdp-render-state.h"
#include "grd-utils.h"
#include "grd-vk-buffer.h"
#include "grd-vk-device.h"
#include "grd-vk-image.h"
#include "grd-vk-memory.h"
#include "grd-vk-physical-device.h"
#include "grd-vk-queue.h"
#include "grd-vk-utils.h"

#define N_SPECIALIZATION_CONSTANTS 6

typedef struct
{
  uint32_t source_width;
  uint32_t source_height;

  uint32_t target_width;
  uint32_t target_height;

  uint32_t perform_dmg_detection;
  uint32_t state_buffer_stride;
} ViewCreateInfo;

typedef struct
{
  VkCommandBuffer init_state_buffers;
  VkCommandBuffer synchronize_state_buffers;

  VkCommandBuffer init_layouts;
  VkCommandBuffer create_dual_view_simple;
  VkCommandBuffer create_dual_view_difference;
} CommandBuffers;

typedef struct
{
  GrdRdpViewCreatorAVC *view_creator_avc;

  VkPipelineLayout vk_pipeline_layout;
  VkPipeline vk_pipeline;
} Pipeline;

struct _GrdRdpViewCreatorAVC
{
  GrdRdpViewCreator parent;

  GrdVkDevice *device;
  gboolean debug_vk_times;
  gboolean supports_update_after_bind;

  ViewCreateInfo view_create_info;

  VkDeviceSize state_buffer_size;

  GrdVkBuffer *dmg_buffer_device;
  GrdVkBuffer *dmg_buffer_host;
  uint32_t *mapped_dmg_buffer;

  GrdVkBuffer *chroma_check_buffer_device;
  GrdVkBuffer *chroma_check_buffer_host;
  uint32_t *mapped_chroma_check_buffer;

  VkSampler vk_src_new_sampler;
  VkSampler vk_src_old_sampler;

  VkDescriptorSetLayout vk_target_descriptor_set_layout;
  VkDescriptorSetLayout vk_source_descriptor_set_layout;
  VkDescriptorSetLayout vk_state_descriptor_set_layout;

  VkDescriptorPool vk_image_descriptor_pool;
  VkDescriptorPool vk_buffer_descriptor_pool;
  VkDescriptorSet vk_descriptor_set_main_view;
  VkDescriptorSet vk_descriptor_set_aux_view;
  VkDescriptorSet vk_descriptor_set_state;
  VkDescriptorSet vk_descriptor_set_sources;

  gboolean pending_view_creation_recording;
  gboolean pending_layout_transition;

  GrdVkQueue *queue;
  VkQueryPool vk_timestamp_query_pool;
  VkCommandPool vk_command_pool;
  CommandBuffers command_buffers;
  VkFence vk_fence;

  Pipeline *dual_view_pipeline;
  Pipeline *dual_view_dmg_pipeline;
};

G_DEFINE_TYPE (GrdRdpViewCreatorAVC, grd_rdp_view_creator_avc,
               GRD_TYPE_RDP_VIEW_CREATOR)

static void
pipeline_free (Pipeline *pipeline);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Pipeline, pipeline_free)

static void
pipeline_free (Pipeline *pipeline)
{
  GrdRdpViewCreatorAVC *view_creator_avc = pipeline->view_creator_avc;
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);

  g_assert (vk_device != VK_NULL_HANDLE);

  if (pipeline->vk_pipeline != VK_NULL_HANDLE)
    vkDestroyPipeline (vk_device, pipeline->vk_pipeline, NULL);
  if (pipeline->vk_pipeline_layout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout (vk_device, pipeline->vk_pipeline_layout, NULL);

  g_free (pipeline);
}

static void
write_image_descriptor_sets (GrdRdpViewCreatorAVC *view_creator_avc,
                             GrdImageViewNV12     *main_image_view,
                             GrdImageViewNV12     *aux_image_view,
                             GrdVkImage           *src_image_new,
                             GrdVkImage           *src_image_old)
{
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  GrdVkImage *main_view_y = grd_image_view_nv12_get_y_layer (main_image_view);
  GrdVkImage *main_view_uv = grd_image_view_nv12_get_uv_layer (main_image_view);
  GrdVkImage *aux_view_y = grd_image_view_nv12_get_y_layer (aux_image_view);
  GrdVkImage *aux_view_uv = grd_image_view_nv12_get_uv_layer (aux_image_view);
  VkWriteDescriptorSet write_descriptor_sets[6] = {};
  VkDescriptorImageInfo image_infos[6] = {};

  image_infos[0].sampler = VK_NULL_HANDLE;
  image_infos[0].imageView = grd_vk_image_get_image_view (main_view_y);
  image_infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

  image_infos[1] = image_infos[0];
  image_infos[1].imageView = grd_vk_image_get_image_view (main_view_uv);

  image_infos[2] = image_infos[0];
  image_infos[2].imageView = grd_vk_image_get_image_view (aux_view_y);

  image_infos[3] = image_infos[0];
  image_infos[3].imageView = grd_vk_image_get_image_view (aux_view_uv);

  image_infos[4].sampler = view_creator_avc->vk_src_new_sampler;
  image_infos[4].imageView = grd_vk_image_get_image_view (src_image_new);
  image_infos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  image_infos[5] = image_infos[4];
  image_infos[5].sampler = view_creator_avc->vk_src_old_sampler;
  if (src_image_old)
    image_infos[5].imageView = grd_vk_image_get_image_view (src_image_old);

  write_descriptor_sets[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write_descriptor_sets[0].dstSet =
    view_creator_avc->vk_descriptor_set_main_view;
  write_descriptor_sets[0].dstBinding = 0;
  write_descriptor_sets[0].descriptorCount = 1;
  write_descriptor_sets[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  write_descriptor_sets[0].pImageInfo = &image_infos[0];

  write_descriptor_sets[1] = write_descriptor_sets[0];
  write_descriptor_sets[1].dstBinding = 1;
  write_descriptor_sets[1].pImageInfo = &image_infos[1];

  write_descriptor_sets[2] = write_descriptor_sets[0];
  write_descriptor_sets[2].dstSet =
    view_creator_avc->vk_descriptor_set_aux_view;
  write_descriptor_sets[2].dstBinding = 0;
  write_descriptor_sets[2].pImageInfo = &image_infos[2];

  write_descriptor_sets[3] = write_descriptor_sets[2];
  write_descriptor_sets[3].dstBinding = 1;
  write_descriptor_sets[3].pImageInfo = &image_infos[3];

  write_descriptor_sets[4] = write_descriptor_sets[0];
  write_descriptor_sets[4].dstSet =
    view_creator_avc->vk_descriptor_set_sources;
  write_descriptor_sets[4].dstBinding = 0;
  write_descriptor_sets[4].descriptorType =
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write_descriptor_sets[4].pImageInfo = &image_infos[4];

  write_descriptor_sets[5] = write_descriptor_sets[4];
  write_descriptor_sets[5].dstBinding = 1;
  write_descriptor_sets[5].pImageInfo = &image_infos[5];

  vkUpdateDescriptorSets (vk_device, 6, write_descriptor_sets, 0, NULL);
}

static void
maybe_init_image_layout (GrdRdpViewCreatorAVC *view_creator_avc,
                         VkCommandBuffer       command_buffer,
                         GrdVkImage           *image,
                         VkImageLayout         new_layout,
                         VkAccessFlags2        dst_access_mask)
{
  GrdVkDevice *device = view_creator_avc->device;
  GrdVkDeviceFuncs *device_funcs = grd_vk_device_get_device_funcs (device);
  VkDependencyInfo dependency_info = {};
  VkImageMemoryBarrier2 image_memory_barrier_2 = {};

  g_assert (device_funcs->vkCmdPipelineBarrier2KHR);

  if (grd_vk_image_get_image_layout (image) == new_layout)
    return;

  image_memory_barrier_2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  image_memory_barrier_2.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  image_memory_barrier_2.srcAccessMask = VK_ACCESS_2_NONE;
  image_memory_barrier_2.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  image_memory_barrier_2.dstAccessMask = dst_access_mask;
  image_memory_barrier_2.oldLayout = grd_vk_image_get_image_layout (image);
  image_memory_barrier_2.newLayout = new_layout;
  image_memory_barrier_2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  image_memory_barrier_2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  image_memory_barrier_2.image = grd_vk_image_get_image (image);
  image_memory_barrier_2.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  image_memory_barrier_2.subresourceRange.baseMipLevel = 0;
  image_memory_barrier_2.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
  image_memory_barrier_2.subresourceRange.baseArrayLayer = 0;
  image_memory_barrier_2.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

  dependency_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency_info.imageMemoryBarrierCount = 1;
  dependency_info.pImageMemoryBarriers = &image_memory_barrier_2;

  device_funcs->vkCmdPipelineBarrier2KHR (command_buffer, &dependency_info);
}

static gboolean
maybe_record_init_layouts (GrdRdpViewCreatorAVC  *view_creator_avc,
                           GrdImageViewNV12      *main_image_view,
                           GrdImageViewNV12      *aux_image_view,
                           GrdVkImage            *src_image_new,
                           GrdVkImage            *src_image_old,
                           GError               **error)
{
  GrdVkImage *main_view_y = grd_image_view_nv12_get_y_layer (main_image_view);
  GrdVkImage *main_view_uv = grd_image_view_nv12_get_uv_layer (main_image_view);
  GrdVkImage *aux_view_y = grd_image_view_nv12_get_y_layer (aux_image_view);
  GrdVkImage *aux_view_uv = grd_image_view_nv12_get_uv_layer (aux_image_view);
  VkCommandBuffer command_buffer =
    view_creator_avc->command_buffers.init_layouts;
  VkCommandBufferBeginInfo begin_info = {};
  VkResult vk_result;

  view_creator_avc->pending_layout_transition =
    grd_vk_image_get_image_layout (main_view_y) != VK_IMAGE_LAYOUT_GENERAL ||
    grd_vk_image_get_image_layout (main_view_uv) != VK_IMAGE_LAYOUT_GENERAL ||
    grd_vk_image_get_image_layout (aux_view_y) != VK_IMAGE_LAYOUT_GENERAL ||
    grd_vk_image_get_image_layout (aux_view_uv) != VK_IMAGE_LAYOUT_GENERAL ||
    grd_vk_image_get_image_layout (src_image_new) != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
    (src_image_old &&
     grd_vk_image_get_image_layout (src_image_old) != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  if (!view_creator_avc->pending_layout_transition)
    return TRUE;

  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

  vk_result = vkBeginCommandBuffer (command_buffer, &begin_info);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to begin command buffer: %i", vk_result);
      return FALSE;
    }

  maybe_init_image_layout (view_creator_avc, command_buffer, main_view_y,
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  maybe_init_image_layout (view_creator_avc, command_buffer, main_view_uv,
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  maybe_init_image_layout (view_creator_avc, command_buffer, aux_view_y,
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  maybe_init_image_layout (view_creator_avc, command_buffer, aux_view_uv,
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  maybe_init_image_layout (view_creator_avc, command_buffer, src_image_new,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  if (src_image_old)
    {
      maybe_init_image_layout (view_creator_avc, command_buffer, src_image_old,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

  vk_result = vkEndCommandBuffer (command_buffer);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to end command buffer: %i", vk_result);
      return FALSE;
    }

  return TRUE;
}

static gboolean
record_create_view (GrdRdpViewCreatorAVC  *view_creator_avc,
                    VkCommandBuffer        command_buffer,
                    Pipeline              *pipeline,
                    GError               **error)
{
  GrdVkDevice *device = view_creator_avc->device;
  GrdVkDeviceFuncs *device_funcs = grd_vk_device_get_device_funcs (device);
  ViewCreateInfo *view_create_info = &view_creator_avc->view_create_info;
  VkCommandBufferBeginInfo begin_info = {};
  VkDescriptorSet descriptor_sets[4] = {};
  uint32_t n_groups_x;
  uint32_t n_groups_y;
  VkResult vk_result;

  g_assert (device_funcs->vkCmdWriteTimestamp2KHR);

  g_assert (view_create_info->target_width % 16 == 0);
  g_assert (view_create_info->target_height % 16 == 0);
  g_assert (view_create_info->target_width > 0);
  g_assert (view_create_info->target_height > 0);

  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

  vk_result = vkBeginCommandBuffer (command_buffer, &begin_info);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to begin command buffer: %i", vk_result);
      return FALSE;
    }

  vkCmdResetQueryPool (command_buffer,
                       view_creator_avc->vk_timestamp_query_pool, 0, 2);
  device_funcs->vkCmdWriteTimestamp2KHR (command_buffer,
                                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                         view_creator_avc->vk_timestamp_query_pool,
                                         0);

  descriptor_sets[0] = view_creator_avc->vk_descriptor_set_main_view;
  descriptor_sets[1] = view_creator_avc->vk_descriptor_set_aux_view;
  descriptor_sets[2] = view_creator_avc->vk_descriptor_set_state;
  descriptor_sets[3] = view_creator_avc->vk_descriptor_set_sources;

  vkCmdBindPipeline (command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                     pipeline->vk_pipeline);
  vkCmdBindDescriptorSets (command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                           pipeline->vk_pipeline_layout,
                           0, 4, descriptor_sets, 0, NULL);

  n_groups_x = view_create_info->target_width / 2 / 16 +
               (view_create_info->target_width / 2 % 16 ? 1 : 0);
  n_groups_y = view_create_info->target_height / 2 / 16 +
               (view_create_info->target_height / 2 % 16 ? 1 : 0);
  vkCmdDispatch (command_buffer, n_groups_x, n_groups_y, 1);

  device_funcs->vkCmdWriteTimestamp2KHR (command_buffer,
                                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                         view_creator_avc->vk_timestamp_query_pool,
                                         1);

  vk_result = vkEndCommandBuffer (command_buffer);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to end command buffer: %i", vk_result);
      return FALSE;
    }

  return TRUE;
}

static gboolean
record_create_view_command_buffers (GrdRdpViewCreatorAVC  *view_creator_avc,
                                    gboolean               record_create_simple_view,
                                    gboolean               record_create_difference_view,
                                    GError               **error)
{
  CommandBuffers *command_buffers = &view_creator_avc->command_buffers;

  if (record_create_simple_view &&
      !record_create_view (view_creator_avc,
                           command_buffers->create_dual_view_simple,
                           view_creator_avc->dual_view_pipeline,
                           error))
    return FALSE;

  if (record_create_difference_view &&
      !record_create_view (view_creator_avc,
                           command_buffers->create_dual_view_difference,
                           view_creator_avc->dual_view_dmg_pipeline,
                           error))
    return FALSE;

  return TRUE;
}

static gboolean
record_command_buffers (GrdRdpViewCreatorAVC  *view_creator_avc,
                        GrdImageViewNV12      *main_image_view,
                        GrdImageViewNV12      *aux_image_view,
                        GrdVkImage            *src_image_new,
                        GrdVkImage            *src_image_old,
                        GError               **error)
{
  if (!maybe_record_init_layouts (view_creator_avc,
                                  main_image_view, aux_image_view,
                                  src_image_new, src_image_old, error))
    return FALSE;
  if (view_creator_avc->pending_view_creation_recording &&
      !record_create_view_command_buffers (view_creator_avc,
                                           !src_image_old, !!src_image_old,
                                           error))
    return FALSE;

  return TRUE;
}

static void
update_image_layout_states (GrdImageViewNV12 *main_image_view,
                            GrdImageViewNV12 *aux_image_view,
                            GrdVkImage       *src_image_new,
                            GrdVkImage       *src_image_old)
{
  GrdVkImage *main_view_y = grd_image_view_nv12_get_y_layer (main_image_view);
  GrdVkImage *main_view_uv = grd_image_view_nv12_get_uv_layer (main_image_view);
  GrdVkImage *aux_view_y = grd_image_view_nv12_get_y_layer (aux_image_view);
  GrdVkImage *aux_view_uv = grd_image_view_nv12_get_uv_layer (aux_image_view);

  grd_vk_image_set_image_layout (main_view_y, VK_IMAGE_LAYOUT_GENERAL);
  grd_vk_image_set_image_layout (main_view_uv, VK_IMAGE_LAYOUT_GENERAL);
  grd_vk_image_set_image_layout (aux_view_y, VK_IMAGE_LAYOUT_GENERAL);
  grd_vk_image_set_image_layout (aux_view_uv, VK_IMAGE_LAYOUT_GENERAL);

  grd_vk_image_set_image_layout (src_image_new,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  if (src_image_old)
    {
      grd_vk_image_set_image_layout (src_image_old,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

static gboolean
grd_rdp_view_creator_avc_create_view (GrdRdpViewCreator  *view_creator,
                                      GList              *image_views,
                                      GrdRdpBuffer       *src_buffer_new,
                                      GrdRdpBuffer       *src_buffer_old,
                                      GError            **error)
{
  GrdRdpViewCreatorAVC *view_creator_avc =
    GRD_RDP_VIEW_CREATOR_AVC (view_creator);
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  CommandBuffers *command_buffers = &view_creator_avc->command_buffers;
  GrdVkImage *src_image_new = NULL;
  GrdVkImage *src_image_old = NULL;
  VkCommandBufferSubmitInfo buffer_submit_infos[4] = {};
  VkCommandBuffer create_dual_view = VK_NULL_HANDLE;
  uint32_t n_buffer_submit_infos = 0;
  VkSubmitInfo2 submit_info_2 = {};
  GrdImageViewNV12 *main_image_view;
  GrdImageViewNV12 *aux_image_view;
  VkResult vk_result;

  g_assert (image_views);
  g_assert (image_views->next);

  main_image_view = GRD_IMAGE_VIEW_NV12 (image_views->data);
  aux_image_view = GRD_IMAGE_VIEW_NV12 (image_views->next->data);

  src_image_new = grd_rdp_buffer_get_dma_buf_image (src_buffer_new);
  if (src_buffer_old)
    src_image_old = grd_rdp_buffer_get_dma_buf_image (src_buffer_old);

  write_image_descriptor_sets (view_creator_avc,
                               main_image_view, aux_image_view,
                               src_image_new, src_image_old);

  if (!record_command_buffers (view_creator_avc,
                               main_image_view, aux_image_view,
                               src_image_new, src_image_old, error))
    return FALSE;

  vk_result = vkResetFences (vk_device, 1, &view_creator_avc->vk_fence);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to reset fence: %i", vk_result);
      return FALSE;
    }

  if (view_creator_avc->pending_layout_transition)
    {
      buffer_submit_infos[n_buffer_submit_infos].sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
      buffer_submit_infos[n_buffer_submit_infos].commandBuffer =
        command_buffers->init_layouts;
      ++n_buffer_submit_infos;
    }

  buffer_submit_infos[n_buffer_submit_infos].sType =
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  buffer_submit_infos[n_buffer_submit_infos].commandBuffer =
    command_buffers->init_state_buffers;
  ++n_buffer_submit_infos;

  if (src_image_old)
    create_dual_view = command_buffers->create_dual_view_difference;
  else
    create_dual_view = command_buffers->create_dual_view_simple;

  buffer_submit_infos[n_buffer_submit_infos].sType =
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  buffer_submit_infos[n_buffer_submit_infos].commandBuffer = create_dual_view;
  ++n_buffer_submit_infos;

  buffer_submit_infos[n_buffer_submit_infos].sType =
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  buffer_submit_infos[n_buffer_submit_infos].commandBuffer =
    command_buffers->synchronize_state_buffers;
  ++n_buffer_submit_infos;

  submit_info_2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit_info_2.commandBufferInfoCount = n_buffer_submit_infos;
  submit_info_2.pCommandBufferInfos = buffer_submit_infos;

  if (!grd_vk_queue_submit (view_creator_avc->queue, &submit_info_2, 1,
                            view_creator_avc->vk_fence, error))
    return FALSE;

  update_image_layout_states (main_image_view, aux_image_view,
                              src_image_new, src_image_old);

  return TRUE;
}

static gboolean
print_view_creation_time (GrdRdpViewCreatorAVC  *view_creator_avc,
                          GError               **error)
{
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  float timestamp_period =
    grd_vk_device_get_timestamp_period (view_creator_avc->device);
  uint64_t gpu_timestamps[2] = {};
  VkQueryResultFlags query_flags;
  float execution_time_us;
  VkResult vk_result;

  query_flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT;
  vk_result = vkGetQueryPoolResults (vk_device,
                                     view_creator_avc->vk_timestamp_query_pool,
                                     0, 2,
                                     sizeof (gpu_timestamps), gpu_timestamps,
                                     sizeof (uint64_t), query_flags);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to get query pool results: %i", vk_result);
      return FALSE;
    }

  execution_time_us = (gpu_timestamps[1] - gpu_timestamps[0]) *
                      timestamp_period / 1000;
  g_debug ("[HWAccel.Vulkan] CreateView[ExecutionTime]: %liµs",
           (int64_t) execution_time_us);

  return TRUE;
}

static GrdRdpRenderState *
grd_rdp_view_creator_avc_finish_view (GrdRdpViewCreator  *view_creator,
                                      GError            **error)
{
  GrdRdpViewCreatorAVC *view_creator_avc =
    GRD_RDP_VIEW_CREATOR_AVC (view_creator);
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  uint32_t state_buffer_length;
  VkResult vk_result;

  do
    {
      vk_result = vkWaitForFences (vk_device, 1, &view_creator_avc->vk_fence,
                                   VK_FALSE, 100 * 1000);
    }
  while (vk_result == VK_TIMEOUT);

  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to wait for fence: %i", vk_result);
      return NULL;
    }

  if (view_creator_avc->debug_vk_times &&
      !print_view_creation_time (view_creator_avc, error))
    return NULL;

  state_buffer_length = view_creator_avc->state_buffer_size /
                        sizeof (uint32_t);

  return grd_rdp_render_state_new (view_creator_avc->mapped_dmg_buffer,
                                   view_creator_avc->mapped_chroma_check_buffer,
                                   state_buffer_length);
}

static void
prepare_view_create_info (GrdRdpViewCreatorAVC *view_creator_avc,
                          uint32_t              target_width,
                          uint32_t              target_height,
                          uint32_t              source_width,
                          uint32_t              source_height)
{
  ViewCreateInfo *view_create_info = &view_creator_avc->view_create_info;

  g_assert (target_width % 16 == 0);
  g_assert (target_height % 16 == 0);
  g_assert (target_width > 0);
  g_assert (target_height > 0);

  view_create_info->source_width = source_width;
  view_create_info->source_height = source_height;

  view_create_info->target_width = target_width;
  view_create_info->target_height = target_height;

  view_create_info->state_buffer_stride =
    grd_get_aligned_size (source_width, 64) / 64;
}

static gboolean
create_state_buffer (GrdRdpViewCreatorAVC  *view_creator_avc,
                     GrdVkBuffer          **state_buffer_device,
                     GrdVkBuffer          **state_buffer_host,
                     uint32_t             **mapped_state_buffer,
                     GError               **error)
{
  GrdVkBufferDescriptor buffer_descriptor = {};
  VkMemoryPropertyFlagBits memory_flags;
  GrdVkMemory *memory;

  buffer_descriptor.usage_flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  buffer_descriptor.size = view_creator_avc->state_buffer_size;
  buffer_descriptor.memory_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

  *state_buffer_host = grd_vk_buffer_new (view_creator_avc->device,
                                          &buffer_descriptor,
                                          error);
  if (!(*state_buffer_host))
    return FALSE;

  memory = grd_vk_buffer_get_memory (*state_buffer_host);

  *mapped_state_buffer = grd_vk_memory_get_mapped_pointer (memory, error);
  if (!(*mapped_state_buffer))
    return FALSE;

  memory_flags = grd_vk_memory_get_memory_flags (memory);
  if (memory_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    return TRUE;

  buffer_descriptor.usage_flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buffer_descriptor.memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  *state_buffer_device = grd_vk_buffer_new (view_creator_avc->device,
                                            &buffer_descriptor,
                                            error);
  if (!(*state_buffer_device))
    return FALSE;

  return TRUE;
}

static gboolean
create_state_buffers (GrdRdpViewCreatorAVC  *view_creator_avc,
                      uint32_t               source_width,
                      uint32_t               source_height,
                      GError               **error)
{
  view_creator_avc->state_buffer_size =
    (grd_get_aligned_size (source_width, 64) / 64) *
    (grd_get_aligned_size (source_height, 64) / 64) *
    sizeof (uint32_t);

  if (!create_state_buffer (view_creator_avc,
                            &view_creator_avc->dmg_buffer_device,
                            &view_creator_avc->dmg_buffer_host,
                            &view_creator_avc->mapped_dmg_buffer,
                            error))
    return FALSE;
  if (!create_state_buffer (view_creator_avc,
                            &view_creator_avc->chroma_check_buffer_device,
                            &view_creator_avc->chroma_check_buffer_host,
                            &view_creator_avc->mapped_chroma_check_buffer,
                            error))
    return FALSE;

  return TRUE;
}

static gboolean
create_sampler (GrdRdpViewCreatorAVC  *view_creator_avc,
                VkSampler             *sampler,
                GError               **error)
{
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  VkSamplerCreateInfo sampler_create_info = {};
  VkResult vk_result;

  g_assert (sampler);

  sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_create_info.magFilter = VK_FILTER_NEAREST;
  sampler_create_info.minFilter = VK_FILTER_NEAREST;
  sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_create_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  sampler_create_info.unnormalizedCoordinates = VK_TRUE;

  vk_result = vkCreateSampler (vk_device, &sampler_create_info, NULL, sampler);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create sampler: %i", vk_result);
      return FALSE;
    }
  g_assert (*sampler != VK_NULL_HANDLE);

  return TRUE;
}

static gboolean
create_samplers (GrdRdpViewCreatorAVC  *view_creator_avc,
                 GError               **error)
{
  if (!create_sampler (view_creator_avc,
                       &view_creator_avc->vk_src_new_sampler,
                       error))
    return FALSE;
  if (!create_sampler (view_creator_avc,
                       &view_creator_avc->vk_src_old_sampler,
                       error))
    return FALSE;

  return TRUE;
}

static gboolean
create_descriptor_set_layout (GrdRdpViewCreatorAVC   *view_creator_avc,
                              VkDescriptorSetLayout  *descriptor_set_layout,
                              VkDescriptorType        descriptor_type,
                              GError                **error)
{
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  VkDescriptorSetLayoutCreateInfo layout_create_info = {};
  VkDescriptorSetLayoutBinding layout_bindings[2] = {};
  VkDescriptorSetLayoutBindingFlagsCreateInfo layout_binding_flags_info = {};
  VkDescriptorBindingFlags binding_flags[2] = {};
  VkResult vk_result;

  g_assert (descriptor_set_layout);

  layout_bindings[0].binding = 0;
  layout_bindings[0].descriptorType = descriptor_type;
  layout_bindings[0].descriptorCount = 1;
  layout_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  layout_bindings[1] = layout_bindings[0];
  layout_bindings[1].binding = 1;

  layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_create_info.bindingCount = 2;
  layout_create_info.pBindings = layout_bindings;

  binding_flags[0] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
  binding_flags[1] = binding_flags[0];

  layout_binding_flags_info.sType =
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  layout_binding_flags_info.bindingCount = 2;
  layout_binding_flags_info.pBindingFlags = binding_flags;

  if (view_creator_avc->supports_update_after_bind &&
      descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
    {
      layout_create_info.flags =
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;

      grd_vk_append_to_chain (&layout_create_info, &layout_binding_flags_info);
    }

  vk_result = vkCreateDescriptorSetLayout (vk_device, &layout_create_info, NULL,
                                           descriptor_set_layout);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create descriptor set layout: %i", vk_result);
      return FALSE;
    }
  g_assert (*descriptor_set_layout != VK_NULL_HANDLE);

  return TRUE;
}

static gboolean
create_descriptor_set_layouts (GrdRdpViewCreatorAVC  *view_creator_avc,
                               GError               **error)
{
  VkDescriptorSetLayout *target_descriptor_set_layout =
    &view_creator_avc->vk_target_descriptor_set_layout;
  VkDescriptorSetLayout *source_descriptor_set_layout =
    &view_creator_avc->vk_source_descriptor_set_layout;
  VkDescriptorSetLayout *state_descriptor_set_layout =
    &view_creator_avc->vk_state_descriptor_set_layout;

  if (!create_descriptor_set_layout (view_creator_avc,
                                     target_descriptor_set_layout,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     error))
    return FALSE;
  if (!create_descriptor_set_layout (view_creator_avc,
                                     source_descriptor_set_layout,
                                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     error))
    return FALSE;
  if (!create_descriptor_set_layout (view_creator_avc,
                                     state_descriptor_set_layout,
                                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     error))
    return FALSE;

  return TRUE;
}

static gboolean
create_image_descriptor_sets (GrdRdpViewCreatorAVC  *view_creator_avc,
                              GError               **error)
{
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  VkDescriptorPoolCreateInfo pool_create_info = {};
  VkDescriptorPoolSize pool_sizes[2] = {};
  VkDescriptorSetAllocateInfo allocate_info = {};
  VkDescriptorSetLayout descriptor_set_layouts[3] = {};
  VkDescriptorSet descriptor_sets[3] = {};
  VkResult vk_result;

  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  pool_sizes[0].descriptorCount = 4;

  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_sizes[1].descriptorCount = 2;

  pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_create_info.maxSets = 3;
  pool_create_info.poolSizeCount = 2;
  pool_create_info.pPoolSizes = pool_sizes;

  if (view_creator_avc->supports_update_after_bind)
    pool_create_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

  vk_result = vkCreateDescriptorPool (vk_device, &pool_create_info, NULL,
                                      &view_creator_avc->vk_image_descriptor_pool);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create descriptor pool: %i", vk_result);
      return FALSE;
    }
  g_assert (view_creator_avc->vk_image_descriptor_pool != VK_NULL_HANDLE);

  descriptor_set_layouts[0] = view_creator_avc->vk_target_descriptor_set_layout;
  descriptor_set_layouts[1] = view_creator_avc->vk_target_descriptor_set_layout;
  descriptor_set_layouts[2] = view_creator_avc->vk_source_descriptor_set_layout;

  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorPool = view_creator_avc->vk_image_descriptor_pool;
  allocate_info.descriptorSetCount = 3;
  allocate_info.pSetLayouts = descriptor_set_layouts;

  vk_result = vkAllocateDescriptorSets (vk_device, &allocate_info,
                                        descriptor_sets);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to allocate descriptor sets: %i", vk_result);
      return FALSE;
    }
  g_assert (descriptor_sets[0] != VK_NULL_HANDLE);
  g_assert (descriptor_sets[1] != VK_NULL_HANDLE);
  g_assert (descriptor_sets[2] != VK_NULL_HANDLE);

  view_creator_avc->vk_descriptor_set_main_view = descriptor_sets[0];
  view_creator_avc->vk_descriptor_set_aux_view = descriptor_sets[1];
  view_creator_avc->vk_descriptor_set_sources = descriptor_sets[2];

  return TRUE;
}

static gboolean
create_buffer_descriptor_set (GrdRdpViewCreatorAVC  *view_creator_avc,
                              GError               **error)
{
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  VkDescriptorPoolCreateInfo pool_create_info = {};
  VkDescriptorPoolSize pool_size = {};
  VkDescriptorSetAllocateInfo allocate_info = {};
  VkDescriptorSetLayout descriptor_set_layout;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkResult vk_result;

  pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_size.descriptorCount = 2;

  pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_create_info.maxSets = 1;
  pool_create_info.poolSizeCount = 1;
  pool_create_info.pPoolSizes = &pool_size;

  vk_result = vkCreateDescriptorPool (vk_device, &pool_create_info, NULL,
                                      &view_creator_avc->vk_buffer_descriptor_pool);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create descriptor pool: %i", vk_result);
      return FALSE;
    }
  g_assert (view_creator_avc->vk_buffer_descriptor_pool != VK_NULL_HANDLE);

  descriptor_set_layout = view_creator_avc->vk_state_descriptor_set_layout;

  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorPool = view_creator_avc->vk_buffer_descriptor_pool;
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &descriptor_set_layout;

  vk_result = vkAllocateDescriptorSets (vk_device, &allocate_info,
                                        &descriptor_set);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to allocate descriptor sets: %i", vk_result);
      return FALSE;
    }
  g_assert (descriptor_set != VK_NULL_HANDLE);

  view_creator_avc->vk_descriptor_set_state = descriptor_set;

  return TRUE;
}

static gboolean
create_descriptor_sets (GrdRdpViewCreatorAVC  *view_creator_avc,
                        GError               **error)
{
  if (!create_image_descriptor_sets (view_creator_avc, error))
    return FALSE;
  if (!create_buffer_descriptor_set (view_creator_avc, error))
    return FALSE;

  return TRUE;
}

static gboolean
create_pipeline_layout (GrdRdpViewCreatorAVC         *view_creator_avc,
                        const VkDescriptorSetLayout  *descriptor_set_layouts,
                        const uint32_t                n_descriptor_set_layouts,
                        VkPipelineLayout             *vk_pipeline_layout,
                        GError                      **error)
{
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  VkPipelineLayoutCreateInfo pipeline_layout_create_info = {};
  VkResult vk_result;

  g_assert (vk_pipeline_layout);

  pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.setLayoutCount = n_descriptor_set_layouts;
  pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;

  vk_result = vkCreatePipelineLayout (vk_device, &pipeline_layout_create_info,
                                      NULL, vk_pipeline_layout);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create pipeline layout: %i", vk_result);
      return FALSE;
    }
  g_assert (*vk_pipeline_layout != VK_NULL_HANDLE);

  return TRUE;
}

static gboolean
create_pipeline (GrdRdpViewCreatorAVC  *view_creator_avc,
                 VkShaderModule         vk_shader_module,
                 VkPipelineLayout       vk_pipeline_layout,
                 VkPipeline            *vk_pipeline,
                 GError               **error)
{
  GrdVkDevice *device = view_creator_avc->device;
  VkDevice vk_device = grd_vk_device_get_device (device);
  VkPipelineCache vk_pipeline_cache = grd_vk_device_get_pipeline_cache (device);
  ViewCreateInfo *view_create_info = &view_creator_avc->view_create_info;
  VkComputePipelineCreateInfo pipeline_create_info = {};
  VkSpecializationInfo specialization_info = {};
  VkSpecializationMapEntry map_entries[N_SPECIALIZATION_CONSTANTS] = {};
  VkResult vk_result;
  uint32_t i;

  g_assert (vk_shader_module != VK_NULL_HANDLE);
  g_assert (vk_pipeline);
  g_assert (sizeof (ViewCreateInfo) /
            sizeof (uint32_t) == N_SPECIALIZATION_CONSTANTS);

  for (i = 0; i < N_SPECIALIZATION_CONSTANTS; ++i)
    {
      map_entries[i].constantID = i;
      map_entries[i].offset = i * sizeof (uint32_t);
      map_entries[i].size = sizeof (uint32_t);
    }

  specialization_info.mapEntryCount = N_SPECIALIZATION_CONSTANTS;
  specialization_info.pMapEntries = map_entries;
  specialization_info.dataSize = sizeof (ViewCreateInfo);
  specialization_info.pData = view_create_info;

  pipeline_create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_create_info.stage.sType =
    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_create_info.stage.module = vk_shader_module;
  pipeline_create_info.stage.pName = "main";
  pipeline_create_info.stage.pSpecializationInfo = &specialization_info;
  pipeline_create_info.layout = vk_pipeline_layout;

  vk_result = vkCreateComputePipelines (vk_device, vk_pipeline_cache,
                                        1, &pipeline_create_info,
                                        NULL, vk_pipeline);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create compute pipeline: %i", vk_result);
      return FALSE;
    }
  g_assert (*vk_pipeline != VK_NULL_HANDLE);

  return TRUE;
}

static Pipeline *
dual_view_pipeline_new (GrdRdpViewCreatorAVC  *view_creator_avc,
                        gboolean               perform_dmg_detection,
                        GError               **error)
{
  ViewCreateInfo *view_create_info = &view_creator_avc->view_create_info;
  const GrdVkShaderModules *shader_modules =
    grd_vk_device_get_shader_modules (view_creator_avc->device);
  g_autoptr (Pipeline) dual_view_pipeline = NULL;
  VkDescriptorSetLayout descriptor_set_layouts[4] = {};

  dual_view_pipeline = g_new0 (Pipeline, 1);
  dual_view_pipeline->view_creator_avc = view_creator_avc;

  descriptor_set_layouts[0] = view_creator_avc->vk_target_descriptor_set_layout;
  descriptor_set_layouts[1] = view_creator_avc->vk_target_descriptor_set_layout;
  descriptor_set_layouts[2] = view_creator_avc->vk_state_descriptor_set_layout;
  descriptor_set_layouts[3] = view_creator_avc->vk_source_descriptor_set_layout;

  if (!create_pipeline_layout (view_creator_avc, descriptor_set_layouts, 4,
                               &dual_view_pipeline->vk_pipeline_layout, error))
    return NULL;

  view_create_info->perform_dmg_detection = perform_dmg_detection;
  if (!create_pipeline (view_creator_avc, shader_modules->create_avc_dual_view,
                        dual_view_pipeline->vk_pipeline_layout,
                        &dual_view_pipeline->vk_pipeline, error))
    return NULL;

  return g_steal_pointer (&dual_view_pipeline);
}

static gboolean
create_pipelines (GrdRdpViewCreatorAVC  *view_creator_avc,
                  GError               **error)
{
  view_creator_avc->dual_view_pipeline =
    dual_view_pipeline_new (view_creator_avc, FALSE, error);
  if (!view_creator_avc->dual_view_pipeline)
    return FALSE;

  view_creator_avc->dual_view_dmg_pipeline =
    dual_view_pipeline_new (view_creator_avc, TRUE, error);
  if (!view_creator_avc->dual_view_dmg_pipeline)
    return FALSE;

  return TRUE;
}

static gboolean
create_timestamp_query_pool (GrdRdpViewCreatorAVC  *view_creator_avc,
                             GError               **error)
{
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  VkQueryPoolCreateInfo pool_create_info = {};
  VkResult vk_result;

  pool_create_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  pool_create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  pool_create_info.queryCount = 2;

  vk_result = vkCreateQueryPool (vk_device, &pool_create_info, NULL,
                                 &view_creator_avc->vk_timestamp_query_pool);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create query pool: %i", vk_result);
      return FALSE;
    }
  g_assert (view_creator_avc->vk_timestamp_query_pool != VK_NULL_HANDLE);

  return TRUE;
}

static gboolean
create_command_buffers (GrdRdpViewCreatorAVC  *view_creator_avc,
                        GError               **error)
{
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  CommandBuffers *command_buffers = &view_creator_avc->command_buffers;
  GrdVkQueue *queue = view_creator_avc->queue;
  VkCommandPoolCreateInfo pool_create_info = {};
  VkCommandBufferAllocateInfo allocate_info = {};
  VkCommandBuffer vk_command_buffers[5] = {};
  VkResult vk_result;

  pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_create_info.queueFamilyIndex = grd_vk_queue_get_queue_family_idx (queue);

  vk_result = vkCreateCommandPool (vk_device, &pool_create_info, NULL,
                                   &view_creator_avc->vk_command_pool);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create command pool: %i", vk_result);
      return FALSE;
    }
  g_assert (view_creator_avc->vk_command_pool != VK_NULL_HANDLE);

  allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate_info.commandPool = view_creator_avc->vk_command_pool;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 5;

  vk_result = vkAllocateCommandBuffers (vk_device, &allocate_info,
                                        vk_command_buffers);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to allocate command buffers: %i", vk_result);
      return FALSE;
    }
  g_assert (vk_command_buffers[0] != VK_NULL_HANDLE);
  g_assert (vk_command_buffers[1] != VK_NULL_HANDLE);
  g_assert (vk_command_buffers[2] != VK_NULL_HANDLE);
  g_assert (vk_command_buffers[3] != VK_NULL_HANDLE);
  g_assert (vk_command_buffers[4] != VK_NULL_HANDLE);

  command_buffers->init_state_buffers = vk_command_buffers[0];
  command_buffers->synchronize_state_buffers = vk_command_buffers[1];
  command_buffers->init_layouts = vk_command_buffers[2];
  command_buffers->create_dual_view_simple = vk_command_buffers[3];
  command_buffers->create_dual_view_difference = vk_command_buffers[4];

  return TRUE;
}

static gboolean
create_fence (GrdRdpViewCreatorAVC  *view_creator_avc,
              GError               **error)
{
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  VkFenceCreateInfo fence_create_info = {};
  VkResult vk_result;

  fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

  vk_result = vkCreateFence (vk_device, &fence_create_info, NULL,
                             &view_creator_avc->vk_fence);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create fence: %i", vk_result);
      return FALSE;
    }
  g_assert (view_creator_avc->vk_fence != VK_NULL_HANDLE);

  return TRUE;
}

static void
write_state_descriptor_set (GrdRdpViewCreatorAVC *view_creator_avc)
{
  VkDevice vk_device = grd_vk_device_get_device (view_creator_avc->device);
  VkWriteDescriptorSet write_descriptor_sets[2] = {};
  VkDescriptorBufferInfo buffer_infos[2] = {};
  GrdVkBuffer *dmg_buffer;
  GrdVkBuffer *chroma_check_buffer;

  dmg_buffer = view_creator_avc->dmg_buffer_device;
  if (!dmg_buffer)
    dmg_buffer = view_creator_avc->dmg_buffer_host;

  chroma_check_buffer = view_creator_avc->chroma_check_buffer_device;
  if (!chroma_check_buffer)
    chroma_check_buffer = view_creator_avc->chroma_check_buffer_host;

  buffer_infos[0].buffer = grd_vk_buffer_get_buffer (dmg_buffer);
  buffer_infos[0].offset = 0;
  buffer_infos[0].range = VK_WHOLE_SIZE;

  buffer_infos[1] = buffer_infos[0];
  buffer_infos[1].buffer = grd_vk_buffer_get_buffer (chroma_check_buffer);

  write_descriptor_sets[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write_descriptor_sets[0].dstSet = view_creator_avc->vk_descriptor_set_state;
  write_descriptor_sets[0].dstBinding = 0;
  write_descriptor_sets[0].descriptorCount = 1;
  write_descriptor_sets[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write_descriptor_sets[0].pBufferInfo = &buffer_infos[0];

  write_descriptor_sets[1] = write_descriptor_sets[0];
  write_descriptor_sets[1].dstBinding = 1;
  write_descriptor_sets[1].pBufferInfo = &buffer_infos[1];

  vkUpdateDescriptorSets (vk_device, 2, write_descriptor_sets, 0, NULL);
}

static gboolean
record_init_state_buffers (GrdRdpViewCreatorAVC  *view_creator_avc,
                           GError               **error)
{
  CommandBuffers *command_buffers = &view_creator_avc->command_buffers;
  VkCommandBuffer command_buffer = command_buffers->init_state_buffers;
  GrdVkDevice *device = view_creator_avc->device;
  GrdVkDeviceFuncs *device_funcs = grd_vk_device_get_device_funcs (device);
  VkCommandBufferBeginInfo begin_info = {};
  VkDependencyInfo dependency_info = {};
  VkMemoryBarrier2 memory_barrier_2 = {};
  GrdVkBuffer *dmg_buffer;
  GrdVkBuffer *chroma_check_buffer;
  VkResult vk_result;

  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

  vk_result = vkBeginCommandBuffer (command_buffer, &begin_info);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to begin command buffer: %i", vk_result);
      return FALSE;
    }

  dmg_buffer = view_creator_avc->dmg_buffer_device;
  if (!dmg_buffer)
    dmg_buffer = view_creator_avc->dmg_buffer_host;

  chroma_check_buffer = view_creator_avc->chroma_check_buffer_device;
  if (!chroma_check_buffer)
    chroma_check_buffer = view_creator_avc->chroma_check_buffer_host;

  vkCmdFillBuffer (command_buffer,
                   grd_vk_buffer_get_buffer (dmg_buffer),
                   0, VK_WHOLE_SIZE,
                   0);
  vkCmdFillBuffer (command_buffer,
                   grd_vk_buffer_get_buffer (chroma_check_buffer),
                   0, VK_WHOLE_SIZE,
                   0);

  memory_barrier_2.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  memory_barrier_2.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
  memory_barrier_2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  memory_barrier_2.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  memory_barrier_2.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

  dependency_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency_info.memoryBarrierCount = 1;
  dependency_info.pMemoryBarriers = &memory_barrier_2;

  device_funcs->vkCmdPipelineBarrier2KHR (command_buffer, &dependency_info);

  vk_result = vkEndCommandBuffer (command_buffer);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to end command buffer: %i", vk_result);
      return FALSE;
    }

  return TRUE;
}

static gboolean
have_device_state_buffers (GrdRdpViewCreatorAVC *view_creator_avc)
{
  gboolean have_device_state_buffers;

  have_device_state_buffers = view_creator_avc->dmg_buffer_device ||
                              view_creator_avc->chroma_check_buffer_device;
  if (have_device_state_buffers)
    {
      g_assert (view_creator_avc->dmg_buffer_device &&
                view_creator_avc->chroma_check_buffer_device);
    }

  return have_device_state_buffers;
}

static void
record_synchronize_device_state_buffers (GrdRdpViewCreatorAVC *view_creator_avc,
                                         VkCommandBuffer       command_buffer)
{
  GrdVkDevice *device = view_creator_avc->device;
  GrdVkDeviceFuncs *device_funcs = grd_vk_device_get_device_funcs (device);
  GrdVkBuffer *dmg_buffer_device = view_creator_avc->dmg_buffer_device;
  GrdVkBuffer *dmg_buffer_host = view_creator_avc->dmg_buffer_host;
  GrdVkBuffer *chroma_check_buffer_device =
    view_creator_avc->chroma_check_buffer_device;
  GrdVkBuffer *chroma_check_buffer_host =
    view_creator_avc->chroma_check_buffer_host;
  VkDependencyInfo dependency_info = {};
  VkMemoryBarrier2 memory_barrier_2 = {};
  VkBufferCopy buffer_copy = {};

  memory_barrier_2.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  memory_barrier_2.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  memory_barrier_2.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  memory_barrier_2.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  memory_barrier_2.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;

  dependency_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency_info.memoryBarrierCount = 1;
  dependency_info.pMemoryBarriers = &memory_barrier_2;

  device_funcs->vkCmdPipelineBarrier2KHR (command_buffer, &dependency_info);

  buffer_copy.srcOffset = 0;
  buffer_copy.dstOffset = 0;
  buffer_copy.size = view_creator_avc->state_buffer_size;

  vkCmdCopyBuffer (command_buffer,
                   grd_vk_buffer_get_buffer (dmg_buffer_device),
                   grd_vk_buffer_get_buffer (dmg_buffer_host),
                   1, &buffer_copy);
  vkCmdCopyBuffer (command_buffer,
                   grd_vk_buffer_get_buffer (chroma_check_buffer_device),
                   grd_vk_buffer_get_buffer (chroma_check_buffer_host),
                   1, &buffer_copy);

  memory_barrier_2.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  memory_barrier_2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  memory_barrier_2.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  memory_barrier_2.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;

  device_funcs->vkCmdPipelineBarrier2KHR (command_buffer, &dependency_info);
}

static void
record_synchronize_host_state_buffers (GrdRdpViewCreatorAVC *view_creator_avc,
                                       VkCommandBuffer       command_buffer)
{
  GrdVkDevice *device = view_creator_avc->device;
  GrdVkDeviceFuncs *device_funcs = grd_vk_device_get_device_funcs (device);
  VkDependencyInfo dependency_info = {};
  VkMemoryBarrier2 memory_barrier_2 = {};

  memory_barrier_2.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  memory_barrier_2.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  memory_barrier_2.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  memory_barrier_2.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  memory_barrier_2.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;

  dependency_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency_info.memoryBarrierCount = 1;
  dependency_info.pMemoryBarriers = &memory_barrier_2;

  device_funcs->vkCmdPipelineBarrier2KHR (command_buffer, &dependency_info);
}

static gboolean
record_synchronize_state_buffers (GrdRdpViewCreatorAVC  *view_creator_avc,
                                  GError               **error)
{
  CommandBuffers *command_buffers = &view_creator_avc->command_buffers;
  VkCommandBuffer command_buffer = command_buffers->synchronize_state_buffers;
  VkCommandBufferBeginInfo begin_info = {};
  VkResult vk_result;

  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

  vk_result = vkBeginCommandBuffer (command_buffer, &begin_info);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to begin command buffer: %i", vk_result);
      return FALSE;
    }

  if (have_device_state_buffers (view_creator_avc))
    record_synchronize_device_state_buffers (view_creator_avc, command_buffer);
  else
    record_synchronize_host_state_buffers (view_creator_avc, command_buffer);

  vk_result = vkEndCommandBuffer (command_buffer);
  if (vk_result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to end command buffer: %i", vk_result);
      return FALSE;
    }

  return TRUE;
}

static gboolean
record_state_handling_command_buffers (GrdRdpViewCreatorAVC  *view_creator_avc,
                                       GError               **error)
{
  if (!record_init_state_buffers (view_creator_avc, error))
    return FALSE;
  if (!record_synchronize_state_buffers (view_creator_avc, error))
    return FALSE;

  return TRUE;
}

GrdRdpViewCreatorAVC *
grd_rdp_view_creator_avc_new (GrdVkDevice  *device,
                              uint32_t      target_width,
                              uint32_t      target_height,
                              uint32_t      source_width,
                              uint32_t      source_height,
                              GError      **error)
{
  g_autoptr (GrdRdpViewCreatorAVC) view_creator_avc = NULL;
  GrdVkPhysicalDevice *physical_device =
    grd_vk_device_get_physical_device (device);
  GrdVkDeviceFeatures device_features =
    grd_vk_physical_device_get_device_features (physical_device);

  view_creator_avc = g_object_new (GRD_TYPE_RDP_VIEW_CREATOR_AVC, NULL);
  view_creator_avc->device = device;

  if (device_features & GRD_VK_DEVICE_FEATURE_UPDATE_AFTER_BIND_SAMPLED_IMAGE &&
      device_features & GRD_VK_DEVICE_FEATURE_UPDATE_AFTER_BIND_STORAGE_IMAGE)
    view_creator_avc->supports_update_after_bind = TRUE;

  prepare_view_create_info (view_creator_avc,
                            target_width, target_height,
                            source_width, source_height);

  if (!create_state_buffers (view_creator_avc,
                             source_width, source_height,
                             error))
    return NULL;

  if (!create_samplers (view_creator_avc, error))
    return NULL;
  if (!create_descriptor_set_layouts (view_creator_avc, error))
    return NULL;
  if (!create_descriptor_sets (view_creator_avc, error))
    return NULL;
  if (!create_pipelines (view_creator_avc, error))
    return NULL;

  view_creator_avc->queue = grd_vk_device_acquire_queue (device);
  if (!create_timestamp_query_pool (view_creator_avc, error))
    return NULL;

  if (!create_command_buffers (view_creator_avc, error))
    return NULL;
  if (!create_fence (view_creator_avc, error))
    return NULL;

  write_state_descriptor_set (view_creator_avc);
  if (!record_state_handling_command_buffers (view_creator_avc, error))
    return NULL;

  if (view_creator_avc->supports_update_after_bind &&
      !record_create_view_command_buffers (view_creator_avc, TRUE, TRUE, error))
    return NULL;

  if (view_creator_avc->supports_update_after_bind)
    view_creator_avc->pending_view_creation_recording = FALSE;

  return g_steal_pointer (&view_creator_avc);
}

static void
grd_rdp_view_creator_avc_dispose (GObject *object)
{
  GrdRdpViewCreatorAVC *view_creator_avc = GRD_RDP_VIEW_CREATOR_AVC (object);
  GrdVkDevice *device = view_creator_avc->device;

  grd_vk_clear_fence (device, &view_creator_avc->vk_fence);
  grd_vk_clear_command_pool (device, &view_creator_avc->vk_command_pool);
  grd_vk_clear_query_pool (device, &view_creator_avc->vk_timestamp_query_pool);

  if (view_creator_avc->queue)
    {
      grd_vk_device_release_queue (device, view_creator_avc->queue);
      view_creator_avc->queue = NULL;
    }

  g_clear_pointer (&view_creator_avc->dual_view_dmg_pipeline, pipeline_free);
  g_clear_pointer (&view_creator_avc->dual_view_pipeline, pipeline_free);

  grd_vk_clear_descriptor_pool (device, &view_creator_avc->vk_buffer_descriptor_pool);
  grd_vk_clear_descriptor_pool (device, &view_creator_avc->vk_image_descriptor_pool);
  grd_vk_clear_descriptor_set_layout (device,
                                      &view_creator_avc->vk_state_descriptor_set_layout);
  grd_vk_clear_descriptor_set_layout (device,
                                      &view_creator_avc->vk_source_descriptor_set_layout);
  grd_vk_clear_descriptor_set_layout (device,
                                      &view_creator_avc->vk_target_descriptor_set_layout);
  grd_vk_clear_sampler (device, &view_creator_avc->vk_src_old_sampler);
  grd_vk_clear_sampler (device, &view_creator_avc->vk_src_new_sampler);

  g_clear_object (&view_creator_avc->chroma_check_buffer_device);
  g_clear_object (&view_creator_avc->chroma_check_buffer_host);
  g_clear_object (&view_creator_avc->dmg_buffer_device);
  g_clear_object (&view_creator_avc->dmg_buffer_host);

  G_OBJECT_CLASS (grd_rdp_view_creator_avc_parent_class)->dispose (object);
}

static void
grd_rdp_view_creator_avc_init (GrdRdpViewCreatorAVC *view_creator_avc)
{
  if (grd_get_debug_flags () & GRD_DEBUG_VK_TIMES)
    view_creator_avc->debug_vk_times = TRUE;

  view_creator_avc->pending_view_creation_recording = TRUE;
}

static void
grd_rdp_view_creator_avc_class_init (GrdRdpViewCreatorAVCClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdRdpViewCreatorClass *view_creator_class =
    GRD_RDP_VIEW_CREATOR_CLASS (klass);

  object_class->dispose = grd_rdp_view_creator_avc_dispose;

  view_creator_class->create_view = grd_rdp_view_creator_avc_create_view;
  view_creator_class->finish_view = grd_rdp_view_creator_avc_finish_view;
}
