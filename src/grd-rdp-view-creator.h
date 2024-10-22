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

#ifndef GRD_RDP_VIEW_CREATOR_H
#define GRD_RDP_VIEW_CREATOR_H

#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_VIEW_CREATOR (grd_rdp_view_creator_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdRdpViewCreator, grd_rdp_view_creator,
                          GRD, RDP_VIEW_CREATOR, GObject)

typedef void (* GrdRdpViewCreatorOnViewCreatedFunc) (GrdRdpFrame *rdp_frame,
                                                     GError      *error);

struct _GrdRdpViewCreatorClass
{
  GObjectClass parent_class;

  gboolean (* create_view) (GrdRdpViewCreator  *view_creator,
                            GList              *image_views,
                            GrdRdpBuffer       *src_buffer_new,
                            GrdRdpBuffer       *src_buffer_old,
                            GError            **error);
  GrdRdpRenderState *(* finish_view) (GrdRdpViewCreator  *view_creator,
                                      GError            **error);
};

gboolean grd_rdp_view_creator_create_view (GrdRdpViewCreator                   *view_creator,
                                           GrdRdpFrame                         *rdp_frame,
                                           GrdRdpViewCreatorOnViewCreatedFunc   on_view_created,
                                           GError                             **error);

#endif /* GRD_RDP_VIEW_CREATOR_H */
