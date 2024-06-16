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

#ifndef GRD_RDP_PW_BUFFER_H
#define GRD_RDP_PW_BUFFER_H

#include <gio/gio.h>
#include <pipewire/stream.h>

#include "grd-rdp-buffer-info.h"
#include "grd-types.h"

typedef struct
{
  int fd;
  uint32_t offset;
  int32_t stride;
} GrdRdpPwBufferDmaBufInfo;

GrdRdpPwBuffer *grd_rdp_pw_buffer_new (struct pw_buffer  *pw_buffer,
                                       GError           **error);

void grd_rdp_pw_buffer_free (GrdRdpPwBuffer *rdp_pw_buffer);

GrdRdpBufferType grd_rdp_pw_buffer_get_buffer_type (GrdRdpPwBuffer *rdp_pw_buffer);

const GrdRdpPwBufferDmaBufInfo *grd_rdp_pw_buffer_get_dma_buf_info (GrdRdpPwBuffer *rdp_pw_buffer);

uint8_t *grd_rdp_pw_buffer_get_mapped_data (GrdRdpPwBuffer *rdp_pw_buffer,
                                            int32_t        *stride);

void grd_rdp_pw_buffer_ensure_unlocked (GrdRdpPwBuffer *rdp_pw_buffer);

void grd_rdp_pw_buffer_acquire_lock (GrdRdpPwBuffer *rdp_pw_buffer);

void grd_rdp_pw_buffer_maybe_release_lock (GrdRdpPwBuffer *rdp_pw_buffer);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdRdpPwBuffer, grd_rdp_pw_buffer_free)

#endif /* GRD_RDP_PW_BUFFER_H */
