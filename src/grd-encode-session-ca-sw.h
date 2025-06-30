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

#include "grd-encode-session.h"
#include "grd-types.h"

#define GRD_TYPE_ENCODE_SESSION_CA_SW (grd_encode_session_ca_sw_get_type ())
G_DECLARE_FINAL_TYPE (GrdEncodeSessionCaSw, grd_encode_session_ca_sw,
                      GRD, ENCODE_SESSION_CA_SW, GrdEncodeSession)

GrdEncodeSessionCaSw *grd_encode_session_ca_sw_new (GrdRdpSwEncoderCa  *encoder_ca,
                                                    uint32_t            source_width,
                                                    uint32_t            source_height,
                                                    GError            **error);
