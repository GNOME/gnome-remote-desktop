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

#ifndef GRD_ENCODE_SESSION_VAAPI_H
#define GRD_ENCODE_SESSION_VAAPI_H

#include <va/va.h>

#include "grd-encode-session.h"

#define GRD_TYPE_ENCODE_SESSION_VAAPI (grd_encode_session_vaapi_get_type ())
G_DECLARE_FINAL_TYPE (GrdEncodeSessionVaapi, grd_encode_session_vaapi,
                      GRD, ENCODE_SESSION_VAAPI, GrdEncodeSession)

GrdEncodeSessionVaapi *grd_encode_session_vaapi_new (GrdVkDevice  *vk_device,
                                                     VADisplay     va_display,
                                                     gboolean      supports_quality_level,
                                                     uint32_t      source_width,
                                                     uint32_t      source_height,
                                                     uint32_t      refresh_rate,
                                                     GError      **error);

#endif /* GRD_ENCODE_SESSION_VAAPI_H */
