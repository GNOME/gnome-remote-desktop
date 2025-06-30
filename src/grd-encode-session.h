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

#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_ENCODE_SESSION (grd_encode_session_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdEncodeSession, grd_encode_session,
                          GRD, ENCODE_SESSION, GObject)

typedef void (* GrdEncodeSessionOnBitstreamLockedFunc) (GrdEncodeSession *encode_session,
                                                        GrdBitstream     *bitstream,
                                                        gpointer          user_data,
                                                        GError           *error);

struct _GrdEncodeSessionClass
{
  GObjectClass parent_class;

  void (* get_surface_size) (GrdEncodeSession *encode_session,
                             uint32_t         *surface_width,
                             uint32_t         *surface_height);
  GList *(* get_image_views) (GrdEncodeSession *encode_session);
  gboolean (* has_pending_frames) (GrdEncodeSession *encode_session);
  gboolean (* encode_frame) (GrdEncodeSession  *encode_session,
                             GrdEncodeContext  *encode_context,
                             GrdImageView      *image_view,
                             GError           **error);
  GrdBitstream *(* lock_bitstream) (GrdEncodeSession  *encode_session,
                                    GrdImageView      *image_view,
                                    GError           **error);
  gboolean (* unlock_bitstream) (GrdEncodeSession  *encode_session,
                                 GrdBitstream      *bitstream,
                                 GError           **error);
};

void grd_encode_session_get_surface_size (GrdEncodeSession *encode_session,
                                          uint32_t         *surface_width,
                                          uint32_t         *surface_height);

GList *grd_encode_session_get_image_views (GrdEncodeSession *encode_session);

gboolean grd_encode_session_has_pending_frames (GrdEncodeSession *encode_session);

gboolean grd_encode_session_encode_frame (GrdEncodeSession  *encode_session,
                                          GrdEncodeContext  *encode_context,
                                          GrdImageView      *image_view,
                                          GError           **error);

void grd_encode_session_lock_bitstream (GrdEncodeSession                      *encode_session,
                                        GrdImageView                          *image_view,
                                        GrdEncodeSessionOnBitstreamLockedFunc  on_bitstream_locked,
                                        gpointer                               user_data);

gboolean grd_encode_session_unlock_bitstream (GrdEncodeSession  *encode_session,
                                              GrdBitstream      *bitstream,
                                              GError           **error);
