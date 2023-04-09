/*
 * Copyright (C) 2023 Pascal Nowack
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

#ifndef GRD_RDP_CONNECT_TIME_AUTODETECTION_H
#define GRD_RDP_CONNECT_TIME_AUTODETECTION_H

#include <glib-object.h>

#include "grd-rdp-network-autodetection.h"

#define GRD_TYPE_RDP_CONNECT_TIME_AUTODETECTION (grd_rdp_connect_time_autodetection_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpConnectTimeAutodetection, grd_rdp_connect_time_autodetection,
                      GRD, RDP_CONNECT_TIME_AUTODETECTION, GObject)

GrdRdpConnectTimeAutodetection *grd_rdp_connect_time_autodetection_new (GrdRdpNetworkAutodetection *network_autodetection,
                                                                        rdpAutoDetect              *rdp_autodetect);

gboolean grd_rdp_connect_time_autodetection_is_complete (GrdRdpConnectTimeAutodetection *ct_autodetection);

void grd_rdp_connect_time_autodetection_start_detection (GrdRdpConnectTimeAutodetection *ct_autodetection);

void grd_rdp_connect_time_autodetection_invoke_shutdown (GrdRdpConnectTimeAutodetection *ct_autodetection);

BOOL grd_rdp_connect_time_autodetection_notify_bw_measure_results (GrdRdpConnectTimeAutodetection *ct_autodetection,
                                                                   uint32_t                        time_delta_ms,
                                                                   uint32_t                        byte_count);

void grd_rdp_connect_time_autodetection_notify_rtt_measure_response (GrdRdpConnectTimeAutodetection *ct_autodetection,
                                                                     uint16_t                        sequence_number,
                                                                     int64_t                         base_round_trip_time_us,
                                                                     int64_t                         avg_round_trip_time_us);

#endif /* GRD_RDP_CONNECT_TIME_AUTODETECTION_H */
