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

#include "grd-image-view-nv12.h"

#include "grd-vk-image.h"

struct _GrdImageViewNV12
{
  GrdImageView parent;

  GrdVkImage *vk_y_layer;
  GrdVkImage *vk_uv_layer;
};

G_DEFINE_TYPE (GrdImageViewNV12, grd_image_view_nv12,
               GRD_TYPE_IMAGE_VIEW)

VkImageView
grd_image_view_nv12_get_y_layer (GrdImageViewNV12 *image_view_nv12)
{
  return grd_vk_image_get_image_view (image_view_nv12->vk_y_layer);
}

VkImageView
grd_image_view_nv12_get_uv_layer (GrdImageViewNV12 *image_view_nv12)
{
  return grd_vk_image_get_image_view (image_view_nv12->vk_uv_layer);
}

GList *
grd_image_view_nv12_get_images (GrdImageViewNV12 *image_view_nv12)
{
  GList *images = NULL;

  if (image_view_nv12->vk_y_layer)
    images = g_list_append (images, image_view_nv12->vk_y_layer);
  if (image_view_nv12->vk_uv_layer)
    images = g_list_append (images, image_view_nv12->vk_uv_layer);

  return images;
}

VkImageLayout
grd_image_view_nv12_get_image_layout (GrdImageViewNV12 *image_view_nv12)
{
  g_assert (grd_vk_image_get_image_layout (image_view_nv12->vk_y_layer) ==
            grd_vk_image_get_image_layout (image_view_nv12->vk_uv_layer));

  return grd_vk_image_get_image_layout (image_view_nv12->vk_y_layer);
}

void
grd_image_view_nv12_set_image_layout (GrdImageViewNV12 *image_view_nv12,
                                      VkImageLayout     vk_image_layout)
{
  grd_vk_image_set_image_layout (image_view_nv12->vk_y_layer, vk_image_layout);
  grd_vk_image_set_image_layout (image_view_nv12->vk_uv_layer, vk_image_layout);
}

void
grd_image_view_nv12_notify_image_view_release (GrdImageView *image_view)
{
}

GrdImageViewNV12 *
grd_image_view_nv12_new (GrdVkImage *vk_y_layer,
                         GrdVkImage *vk_uv_layer)
{
  GrdImageViewNV12 *image_view_nv12;

  image_view_nv12 = g_object_new (GRD_TYPE_IMAGE_VIEW_NV12, NULL);
  image_view_nv12->vk_y_layer = vk_y_layer;
  image_view_nv12->vk_uv_layer = vk_uv_layer;

  return image_view_nv12;
}

static void
grd_image_view_nv12_dispose (GObject *object)
{
  GrdImageViewNV12 *image_view_nv12 = GRD_IMAGE_VIEW_NV12 (object);

  g_clear_object (&image_view_nv12->vk_uv_layer);
  g_clear_object (&image_view_nv12->vk_y_layer);

  G_OBJECT_CLASS (grd_image_view_nv12_parent_class)->dispose (object);
}

static void
grd_image_view_nv12_init (GrdImageViewNV12 *image_view_nv12)
{
}

static void
grd_image_view_nv12_class_init (GrdImageViewNV12Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdImageViewClass *image_view_class = GRD_IMAGE_VIEW_CLASS (klass);

  object_class->dispose = grd_image_view_nv12_dispose;

  image_view_class->notify_image_view_release =
    grd_image_view_nv12_notify_image_view_release;
}
