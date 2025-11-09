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

#pragma once

#include <glib-object.h>
#include <pipewire/pipewire.h>

#include "grd-types.h"

#define GRD_TYPE_DECODE_SESSION (grd_decode_session_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdDecodeSession, grd_decode_session,
                          GRD, DECODE_SESSION, GObject)

typedef void (* GrdDecodeSessionOnFrameReadyFunc) (GrdDecodeSession *decode_session,
                                                   GrdSampleBuffer  *sample_buffer,
                                                   gpointer          user_data,
                                                   GError           *error);

struct _GrdDecodeSessionClass
{
  GObjectClass parent_class;

  void (* get_drm_format_modifiers) (GrdDecodeSession  *decode_session,
                                     uint32_t           drm_format,
                                     uint32_t          *out_n_modifiers,
                                     uint64_t         **out_modifiers);
  gboolean (* reset) (GrdDecodeSession  *decode_session,
                      uint32_t           surface_width,
                      uint32_t           surface_height,
                      uint64_t           drm_format_modifier,
                      GError           **error);
  gboolean (* register_buffer) (GrdDecodeSession  *decode_session,
                                struct pw_buffer  *pw_buffer,
                                GError           **error);
  void (* unregister_buffer) (GrdDecodeSession *decode_session,
                              struct pw_buffer *pw_buffer);
  GrdSampleBuffer *(* get_sample_buffer) (GrdDecodeSession *decode_session,
                                          struct pw_buffer *pw_buffer);
  gboolean (* submit_sample) (GrdDecodeSession  *decode_session,
                              GrdSampleBuffer   *sample_buffer,
                              GError           **error);
  uint32_t (* get_n_pending_frames) (GrdDecodeSession *decode_session);
  gboolean (* decode_frame) (GrdDecodeSession  *decode_session,
                             GrdSampleBuffer   *sample_buffer,
                             GError           **error);
};

void grd_decode_session_get_drm_format_modifiers (GrdDecodeSession  *decode_session,
                                                  uint32_t           drm_format,
                                                  uint32_t          *out_n_modifiers,
                                                  uint64_t         **out_modifiers);

gboolean grd_decode_session_reset (GrdDecodeSession  *decode_session,
                                   uint32_t           surface_width,
                                   uint32_t           surface_height,
                                   uint64_t           drm_format_modifier,
                                   GError           **error);

gboolean grd_decode_session_register_buffer (GrdDecodeSession  *decode_session,
                                             struct pw_buffer  *pw_buffer,
                                             GError           **error);

void grd_decode_session_unregister_buffer (GrdDecodeSession *decode_session,
                                           struct pw_buffer *pw_buffer);

GrdSampleBuffer *grd_decode_session_get_sample_buffer (GrdDecodeSession *decode_session,
                                                       struct pw_buffer *pw_buffer);

uint32_t grd_decode_session_get_n_pending_frames (GrdDecodeSession *decode_session);

gboolean grd_decode_session_decode_frame (GrdDecodeSession                  *decode_session,
                                          GrdSampleBuffer                   *sample_buffer,
                                          GrdDecodeSessionOnFrameReadyFunc   on_frame_ready,
                                          gpointer                           user_data,
                                          GError                           **error);
