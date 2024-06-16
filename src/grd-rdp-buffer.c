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

#include "grd-rdp-buffer.h"

#include <drm_fourcc.h>

#include "grd-rdp-buffer-info.h"
#include "grd-rdp-pw-buffer.h"
#include "grd-rdp-surface.h"
#include "grd-vk-utils.h"

struct _GrdRdpBuffer
{
  GObject parent;

  GrdVkImage *dma_buf_image;
};

G_DEFINE_TYPE (GrdRdpBuffer, grd_rdp_buffer, G_TYPE_OBJECT)

static gboolean
get_vk_format_from_drm_format (uint32_t   drm_format,
                               VkFormat  *vk_format,
                               GError   **error)
{
  *vk_format = VK_FORMAT_UNDEFINED;

  switch (drm_format)
    {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
      *vk_format = VK_FORMAT_B8G8R8A8_UNORM;
      return TRUE;
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "No VkFormat available for DRM format 0x%08X", drm_format);

  return FALSE;
}

static gboolean
import_dma_buf_image (GrdRdpBuffer      *rdp_buffer,
                      GrdRdpPwBuffer    *rdp_pw_buffer,
                      GrdRdpBufferInfo  *rdp_buffer_info,
                      GrdRdpSurface     *rdp_surface,
                      GrdVkDevice       *vk_device,
                      GError           **error)
{
  uint32_t surface_width = grd_rdp_surface_get_width (rdp_surface);
  uint32_t surface_height = grd_rdp_surface_get_height (rdp_surface);
  const GrdRdpPwBufferDmaBufInfo *dma_buf_info =
    grd_rdp_pw_buffer_get_dma_buf_info (rdp_pw_buffer);
  VkFormat vk_format = VK_FORMAT_UNDEFINED;

  if (!get_vk_format_from_drm_format (rdp_buffer_info->drm_format, &vk_format,
                                      error))
    return FALSE;

  rdp_buffer->dma_buf_image =
    grd_vk_dma_buf_image_new (vk_device, vk_format,
                              surface_width, surface_height,
                              VK_IMAGE_USAGE_SAMPLED_BIT,
                              dma_buf_info->fd,
                              dma_buf_info->offset,
                              dma_buf_info->stride,
                              rdp_buffer_info->drm_format_modifier,
                              error);
  if (!rdp_buffer->dma_buf_image)
    return FALSE;

  return TRUE;
}

GrdRdpBuffer *
grd_rdp_buffer_new (GrdRdpPwBuffer    *rdp_pw_buffer,
                    GrdRdpBufferInfo  *rdp_buffer_info,
                    GrdRdpSurface     *rdp_surface,
                    GrdVkDevice       *vk_device,
                    GError           **error)
{
  g_autoptr (GrdRdpBuffer) rdp_buffer = NULL;
  GrdRdpBufferType buffer_type;

  rdp_buffer = g_object_new (GRD_TYPE_RDP_BUFFER, NULL);

  buffer_type = grd_rdp_pw_buffer_get_buffer_type (rdp_pw_buffer);
  if (buffer_type == GRD_RDP_BUFFER_TYPE_DMA_BUF &&
      vk_device &&
      rdp_buffer_info->drm_format_modifier != DRM_FORMAT_MOD_INVALID)
    {
      if (!import_dma_buf_image (rdp_buffer, rdp_pw_buffer, rdp_buffer_info,
                                 rdp_surface, vk_device, error))
        return NULL;
    }

  return g_steal_pointer (&rdp_buffer);
}

static void
grd_rdp_buffer_dispose (GObject *object)
{
  GrdRdpBuffer *rdp_buffer = GRD_RDP_BUFFER (object);

  g_clear_object (&rdp_buffer->dma_buf_image);

  G_OBJECT_CLASS (grd_rdp_buffer_parent_class)->dispose (object);
}

static void
grd_rdp_buffer_init (GrdRdpBuffer *rdp_buffer)
{
}

static void
grd_rdp_buffer_class_init (GrdRdpBufferClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_buffer_dispose;
}
