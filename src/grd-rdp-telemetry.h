/*
 * Copyright (C) 2022 Pascal Nowack
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

#ifndef GRD_RDP_TELEMETRY_H
#define GRD_RDP_TELEMETRY_H

#include <freerdp/server/telemetry.h>
#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_TELEMETRY (grd_rdp_telemetry_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpTelemetry, grd_rdp_telemetry,
                      GRD, RDP_TELEMETRY, GObject)

GrdRdpTelemetry *grd_rdp_telemetry_new (GrdSessionRdp *session_rdp,
                                        GrdRdpDvc     *rdp_dvc,
                                        HANDLE         vcm,
                                        rdpContext    *rdp_context);

void grd_rdp_telemetry_maybe_init (GrdRdpTelemetry *telemetry);

#endif /* GRD_RDP_TELEMETRY_H */
