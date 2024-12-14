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

#include "grd-image-view-rgb.h"

struct _GrdImageViewRGB
{
  GrdImageView parent;

  GrdLocalBuffer *local_buffer;
  GrdImageViewRGBReleaseCallback on_release;
  gpointer on_release_user_data;
};

G_DEFINE_TYPE (GrdImageViewRGB, grd_image_view_rgb,
               GRD_TYPE_IMAGE_VIEW)

GrdLocalBuffer *
grd_image_view_rgb_get_local_buffer (GrdImageViewRGB *image_view_rgb)
{
  return image_view_rgb->local_buffer;
}

void
grd_image_view_rgb_attach_local_buffer (GrdImageViewRGB                *image_view_rgb,
                                        GrdLocalBuffer                 *local_buffer,
                                        GrdImageViewRGBReleaseCallback  callback,
                                        gpointer                        user_data)
{
  g_assert (!image_view_rgb->local_buffer);

  image_view_rgb->local_buffer = local_buffer;
  image_view_rgb->on_release = callback;
  image_view_rgb->on_release_user_data = user_data;
}

static void
grd_image_view_rgb_notify_image_view_release (GrdImageView *image_view)
{
  GrdImageViewRGB *image_view_rgb = GRD_IMAGE_VIEW_RGB (image_view);

  if (!image_view_rgb->on_release)
    return;

  image_view_rgb->on_release (image_view_rgb->on_release_user_data,
                              image_view_rgb->local_buffer);

  image_view_rgb->local_buffer = NULL;
  image_view_rgb->on_release = NULL;
  image_view_rgb->on_release_user_data = NULL;
}

GrdImageViewRGB *
grd_image_view_rgb_new (void)
{
  return g_object_new (GRD_TYPE_IMAGE_VIEW_RGB, NULL);
}

static void
grd_image_view_rgb_init (GrdImageViewRGB *image_view_rgb)
{
}

static void
grd_image_view_rgb_class_init (GrdImageViewRGBClass *klass)
{
  GrdImageViewClass *image_view_class = GRD_IMAGE_VIEW_CLASS (klass);

  image_view_class->notify_image_view_release =
    grd_image_view_rgb_notify_image_view_release;
}
