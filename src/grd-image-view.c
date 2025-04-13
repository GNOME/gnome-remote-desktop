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

#include "grd-image-view.h"

G_DEFINE_ABSTRACT_TYPE (GrdImageView, grd_image_view,
                        G_TYPE_OBJECT)

void
grd_image_view_notify_image_view_release (GrdImageView *image_view)
{
  GrdImageViewClass *klass = GRD_IMAGE_VIEW_GET_CLASS (image_view);

  klass->notify_image_view_release (image_view);
}

static void
grd_image_view_init (GrdImageView *image_view)
{
}

static void
grd_image_view_class_init (GrdImageViewClass *klass)
{
}
