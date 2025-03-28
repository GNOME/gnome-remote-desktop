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

#ifndef GRD_RDP_BUFFER_INFO_H
#define GRD_RDP_BUFFER_INFO_H

#include <stdint.h>

typedef enum
{
  GRD_RDP_BUFFER_TYPE_NONE,
  GRD_RDP_BUFFER_TYPE_DMA_BUF,
  GRD_RDP_BUFFER_TYPE_MEM_FD,
} GrdRdpBufferType;

struct _GrdRdpBufferInfo
{
  GrdRdpBufferType buffer_type;

  uint32_t drm_format;
  uint64_t drm_format_modifier;
};

#endif /* GRD_RDP_BUFFER_INFO_H */
