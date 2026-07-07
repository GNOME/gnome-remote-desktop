/*
 * Copyright (C) 2026 Red Hat Inc.
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
 *
 * Written by:
 *     Joan Torres Lopez <joantolo@redhat.com>
 */

#pragma once

#include <freerdp/channels/wtsvc.h>
#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_DEVICE_REDIRECTION (grd_rdp_device_redirection_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpDeviceRedirection, grd_rdp_device_redirection,
                      GRD, RDP_DEVICE_REDIRECTION, GObject)

GrdRdpDeviceRedirection *grd_rdp_device_redirection_new (GrdSessionRdp *session_rdp,
                                                         HANDLE         vcm);

GrdRdpSmartcard *grd_rdp_device_redirection_get_smartcard (GrdRdpDeviceRedirection *device_redirection);
