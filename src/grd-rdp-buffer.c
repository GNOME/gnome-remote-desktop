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

  GrdRdpPwBuffer *rdp_pw_buffer;
  GrdRdpBufferInfo *rdp_buffer_info;

  GrdVkImage *dma_buf_image;

  gboolean marked_for_removal;
};

G_DEFINE_TYPE (GrdRdpBuffer, grd_rdp_buffer, G_TYPE_OBJECT)

GrdRdpPwBuffer *
grd_rdp_buffer_get_rdp_pw_buffer (GrdRdpBuffer *rdp_buffer)
{
  return rdp_buffer->rdp_pw_buffer;
}

const GrdRdpBufferInfo *
grd_rdp_buffer_get_rdp_buffer_info (GrdRdpBuffer *rdp_buffer)
{
  return rdp_buffer->rdp_buffer_info;
}

GrdVkImage *
grd_rdp_buffer_get_dma_buf_image (GrdRdpBuffer *rdp_buffer)
{
  return rdp_buffer->dma_buf_image;
}

gboolean
grd_rdp_buffer_is_marked_for_removal (GrdRdpBuffer *rdp_buffer)
{
  return rdp_buffer->marked_for_removal;
}

void
grd_rdp_buffer_mark_for_removal (GrdRdpBuffer *rdp_buffer)
{
  rdp_buffer->marked_for_removal = TRUE;
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

  if (!grd_vk_get_vk_format_from_drm_format (rdp_buffer_info->drm_format,
                                             &vk_format, error))
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

  rdp_buffer_info->has_vk_image = TRUE;

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
  rdp_buffer->rdp_pw_buffer = rdp_pw_buffer;
  rdp_buffer->rdp_buffer_info =
    g_memdup2 (rdp_buffer_info, sizeof (GrdRdpBufferInfo));

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
  g_clear_pointer (&rdp_buffer->rdp_buffer_info, g_free);

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
