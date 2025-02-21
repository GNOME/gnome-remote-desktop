/*
 * Copyright (C) 2025 Pascal Nowack
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

#include "grd-local-buffer-wrapper-rdp.h"

#include "grd-rdp-buffer.h"
#include "grd-rdp-pw-buffer.h"

struct _GrdLocalBufferWrapperRdp
{
  GrdLocalBuffer parent;

  uint8_t *buffer;
  int32_t buffer_stride;
};

G_DEFINE_TYPE (GrdLocalBufferWrapperRdp, grd_local_buffer_wrapper_rdp,
               GRD_TYPE_LOCAL_BUFFER)

void
grd_local_buffer_wrapper_rdp_attach_rdp_buffer (GrdLocalBufferWrapperRdp *buffer_wrapper,
                                                GrdRdpBuffer             *rdp_buffer)
{
  GrdRdpPwBuffer *rdp_pw_buffer = grd_rdp_buffer_get_rdp_pw_buffer (rdp_buffer);

  buffer_wrapper->buffer =
    grd_rdp_pw_buffer_get_mapped_data (rdp_pw_buffer,
                                       &buffer_wrapper->buffer_stride);
}

static uint8_t *
grd_local_buffer_wrapper_rdp_get_buffer (GrdLocalBuffer *local_buffer)
{
  GrdLocalBufferWrapperRdp *buffer_wrapper =
    GRD_LOCAL_BUFFER_WRAPPER_RDP (local_buffer);

  return buffer_wrapper->buffer;
}

static uint32_t
grd_local_buffer_wrapper_rdp_get_buffer_stride (GrdLocalBuffer *local_buffer)
{
  GrdLocalBufferWrapperRdp *buffer_wrapper =
    GRD_LOCAL_BUFFER_WRAPPER_RDP (local_buffer);

  return buffer_wrapper->buffer_stride;
}

GrdLocalBufferWrapperRdp *
grd_local_buffer_wrapper_rdp_new (void)
{
  return g_object_new (GRD_TYPE_LOCAL_BUFFER_WRAPPER_RDP, NULL);
}

static void
grd_local_buffer_wrapper_rdp_init (GrdLocalBufferWrapperRdp *local_buffer)
{
}

static void
grd_local_buffer_wrapper_rdp_class_init (GrdLocalBufferWrapperRdpClass *klass)
{
  GrdLocalBufferClass *local_buffer_class = GRD_LOCAL_BUFFER_CLASS (klass);

  local_buffer_class->get_buffer = grd_local_buffer_wrapper_rdp_get_buffer;
  local_buffer_class->get_buffer_stride =
    grd_local_buffer_wrapper_rdp_get_buffer_stride;
}
