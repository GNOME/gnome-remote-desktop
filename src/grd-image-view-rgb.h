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

#ifndef GRD_IMAGE_VIEW_RGB_H
#define GRD_IMAGE_VIEW_RGB_H

#include "grd-image-view.h"
#include "grd-types.h"

#define GRD_TYPE_IMAGE_VIEW_RGB (grd_image_view_rgb_get_type ())
G_DECLARE_FINAL_TYPE (GrdImageViewRGB, grd_image_view_rgb,
                      GRD, IMAGE_VIEW_RGB, GrdImageView)

typedef void (* GrdImageViewRGBReleaseCallback) (gpointer        user_data,
                                                 GrdLocalBuffer *local_buffer);

GrdImageViewRGB *grd_image_view_rgb_new (void);

GrdLocalBuffer *grd_image_view_rgb_get_local_buffer (GrdImageViewRGB *image_view_rgb);

void grd_image_view_rgb_attach_local_buffer (GrdImageViewRGB                *image_view_rgb,
                                             GrdLocalBuffer                 *local_buffer,
                                             GrdImageViewRGBReleaseCallback  callback,
                                             gpointer                        user_data);

#endif /* GRD_IMAGE_VIEW_RGB_H */
