/*
 * Copyright (C) 2021 Pascal Nowack
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

#ifndef GRD_RDP_PRIVATE_H
#define GRD_RDP_PRIVATE_H

#include <freerdp/freerdp.h>

#include "grd-types.h"

typedef struct _RdpPeerContext
{
  rdpContext rdp_context;

  GrdSessionRdp *session_rdp;

  uint32_t frame_id;
  uint16_t planar_flags;

  RFX_CONTEXT *rfx_context;
  wStream *encode_stream;

  /* Virtual Channel Manager */
  HANDLE vcm;

  GrdClipboardRdp *clipboard_rdp;
  GrdRdpGraphicsPipeline *graphics_pipeline;
} RdpPeerContext;

#endif /* GRD_RDP_PRIVATE_H */
