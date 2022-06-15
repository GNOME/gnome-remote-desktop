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

#define GRD_TYPE_RDP_CAMERA_STREAM (grd_rdp_camera_stream_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpCameraStream, grd_rdp_camera_stream,
                      GRD, RDP_CAMERA_STREAM, GObject)

GrdRdpCameraStream *grd_rdp_camera_stream_new (GrdRdpDvcCameraDevice  *device,
                                               GMainContext           *camera_context,
                                               struct pw_core         *pipewire_core,
                                               const char             *device_name,
                                               uint8_t                 stream_index,
                                               GList                  *media_type_descriptions,
                                               GError                **error);

uint8_t grd_rdp_camera_stream_get_stream_index (GrdRdpCameraStream *camera_stream);

gboolean grd_rdp_camera_stream_notify_stream_started (GrdRdpCameraStream *camera_stream,
                                                      uint32_t            run_sequence);

void grd_rdp_camera_stream_inhibit_camera_loop (GrdRdpCameraStream *camera_stream);

void grd_rdp_camera_stream_uninhibit_camera_loop (GrdRdpCameraStream *camera_stream);

gboolean grd_rdp_camera_stream_announce_new_sample (GrdRdpCameraStream *camera_stream,
                                                    GrdSampleBuffer    *sample_buffer);

void grd_rdp_camera_stream_submit_sample (GrdRdpCameraStream *camera_stream,
                                          GrdSampleBuffer    *sample_buffer,
                                          gboolean            success);
