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

#ifndef GRD_RDP_BUFFER_H
#define GRD_RDP_BUFFER_H

#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_BUFFER (grd_rdp_buffer_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpBuffer, grd_rdp_buffer,
                      GRD, RDP_BUFFER, GObject)

GrdRdpBuffer *grd_rdp_buffer_new (GrdRdpPwBuffer    *rdp_pw_buffer,
                                  GrdRdpBufferInfo  *rdp_buffer_info,
                                  GrdRdpSurface     *rdp_surface,
                                  GrdVkDevice       *vk_device,
                                  GError           **error);

GrdRdpPwBuffer *grd_rdp_buffer_get_rdp_pw_buffer (GrdRdpBuffer *rdp_buffer);

GrdVkImage *grd_rdp_buffer_get_dma_buf_image (GrdRdpBuffer *rdp_buffer);

gboolean grd_rdp_buffer_is_marked_for_removal (GrdRdpBuffer *rdp_buffer);

void grd_rdp_buffer_mark_for_removal (GrdRdpBuffer *rdp_buffer);

#endif /* GRD_RDP_BUFFER_H */
