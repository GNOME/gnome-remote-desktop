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

#include <freerdp/server/rdpecam.h>

#include "grd-rdp-dvc.h"
#include "grd-types.h"

#define GRD_TYPE_RDP_DVC_CAMERA_DEVICE (grd_rdp_dvc_camera_device_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpDvcCameraDevice, grd_rdp_dvc_camera_device,
                      GRD, RDP_DVC_CAMERA_DEVICE, GrdRdpDvc)

GrdRdpDvcCameraDevice *grd_rdp_dvc_camera_device_new (GrdRdpDvcHandler *dvc_handler,
                                                      HANDLE            vcm,
                                                      rdpContext       *rdp_context,
                                                      uint8_t           protocol_version,
                                                      const char       *dvc_name,
                                                      const char       *device_name);

const char *grd_rdp_dvc_camera_device_get_dvc_name (GrdRdpDvcCameraDevice *device);

void grd_rdp_dvc_camera_device_start_stream (GrdRdpDvcCameraDevice      *device,
                                             GrdRdpCameraStream         *camera_stream,
                                             CAM_MEDIA_TYPE_DESCRIPTION *media_type_description,
                                             uint32_t                    run_sequence);

void grd_rdp_dvc_camera_device_stop_stream (GrdRdpDvcCameraDevice *device,
                                            GrdRdpCameraStream    *camera_stream);

void grd_rdp_dvc_camera_device_request_sample (GrdRdpDvcCameraDevice *device,
                                               GrdRdpCameraStream    *camera_stream,
                                               GrdSampleBuffer       *sample_buffer);
