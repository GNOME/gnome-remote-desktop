/*
 * Copyright (C) 2021 Pascal Nowack
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

#ifndef GRD_RDP_NETWORK_AUTODETECTION_H
#define GRD_RDP_NETWORK_AUTODETECTION_H

#include <freerdp/freerdp.h>
#include <glib-object.h>

#define GRD_TYPE_RDP_NETWORK_AUTODETECTION (grd_rdp_network_autodetection_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpNetworkAutodetection, grd_rdp_network_autodetection,
                      GRD, RDP_NETWORK_AUTODETECTION, GObject)

typedef enum _GrdRdpNwAutodetectRTTConsumer
{
  GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_NONE   = 0,
  GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_RDPGFX = 1 << 0,
} GrdRdpNwAutodetectRTTConsumer;

typedef enum _GrdRdpNwAutodetectRTTNecessity
{
  GRD_RDP_NW_AUTODETECT_RTT_NEC_HIGH,
  GRD_RDP_NW_AUTODETECT_RTT_NEC_LOW,
} GrdRdpNwAutodetectRTTNecessity;

GrdRdpNetworkAutodetection *grd_rdp_network_autodetection_new (rdpContext *rdp_context);

void grd_rdp_network_autodetection_invoke_shutdown (GrdRdpNetworkAutodetection *network_autodetection);

void grd_rdp_network_autodetection_ensure_rtt_consumer (GrdRdpNetworkAutodetection    *network_autodetection,
                                                        GrdRdpNwAutodetectRTTConsumer  rtt_consumer);

void grd_rdp_network_autodetection_remove_rtt_consumer (GrdRdpNetworkAutodetection    *network_autodetection,
                                                        GrdRdpNwAutodetectRTTConsumer  rtt_consumer);

void grd_rdp_network_autodetection_set_rtt_consumer_necessity (GrdRdpNetworkAutodetection     *network_autodetection,
                                                               GrdRdpNwAutodetectRTTConsumer   rtt_consumer,
                                                               GrdRdpNwAutodetectRTTNecessity  rtt_necessity);

#endif /* GRD_RDP_NETWORK_AUTODETECTION_H */
