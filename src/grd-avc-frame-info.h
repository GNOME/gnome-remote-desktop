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

#pragma once

#include <glib.h>
#include <stdint.h>

#include "grd-types.h"

typedef enum
{
  GRD_AVC_FRAME_TYPE_P,
  GRD_AVC_FRAME_TYPE_I,
} GrdAVCFrameType;

GrdAVCFrameInfo *grd_avc_frame_info_new (GrdAVCFrameType frame_type,
                                         uint8_t         qp,
                                         uint8_t         quality);

gboolean grd_avc_frame_info_get_frame_type (GrdAVCFrameInfo *avc_frame_info);

uint8_t grd_avc_frame_info_get_qp (GrdAVCFrameInfo *avc_frame_info);

uint8_t grd_avc_frame_info_get_quality_value (GrdAVCFrameInfo *avc_frame_info);
