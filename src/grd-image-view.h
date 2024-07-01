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

#ifndef GRD_IMAGE_VIEW_H
#define GRD_IMAGE_VIEW_H

#include <glib-object.h>

#define GRD_TYPE_IMAGE_VIEW (grd_image_view_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdImageView, grd_image_view,
                          GRD, IMAGE_VIEW, GObject)

struct _GrdImageViewClass
{
  GObjectClass parent_class;
};

#endif /* GRD_IMAGE_VIEW_H */
