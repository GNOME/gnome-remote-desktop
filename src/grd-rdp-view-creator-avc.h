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

#ifndef GRD_RDP_VIEW_CREATOR_AVC_H
#define GRD_RDP_VIEW_CREATOR_AVC_H

#include <stdint.h>

#include "grd-rdp-view-creator.h"

#define GRD_TYPE_RDP_VIEW_CREATOR_AVC (grd_rdp_view_creator_avc_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpViewCreatorAVC, grd_rdp_view_creator_avc,
                      GRD, RDP_VIEW_CREATOR_AVC, GrdRdpViewCreator)

GrdRdpViewCreatorAVC *grd_rdp_view_creator_avc_new (GrdVkDevice  *device,
                                                    uint32_t      target_width,
                                                    uint32_t      target_height,
                                                    uint32_t      source_width,
                                                    uint32_t      source_height,
                                                    GError      **error);

#endif /* GRD_RDP_VIEW_CREATOR_AVC_H */
