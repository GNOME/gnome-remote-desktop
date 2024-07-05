/*
 * Copyright (C) 2020-2023 Pascal Nowack
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

#ifndef GRD_SESSION_RDP_H
#define GRD_SESSION_RDP_H

#include <gio/gio.h>
#include <glib-object.h>

#include "grd-session.h"
#include "grd-types.h"

#define GRD_TYPE_SESSION_RDP (grd_session_rdp_get_type ())
G_DECLARE_FINAL_TYPE (GrdSessionRdp,
                      grd_session_rdp,
                      GRD, SESSION_RDP,
                      GrdSession)

typedef enum _GrdSessionRdpError
{
  GRD_SESSION_RDP_ERROR_NONE,
  GRD_SESSION_RDP_ERROR_BAD_CAPS,
  GRD_SESSION_RDP_ERROR_BAD_MONITOR_DATA,
  GRD_SESSION_RDP_ERROR_CLOSE_STACK_ON_DRIVER_FAILURE,
  GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED,
  GRD_SESSION_RDP_ERROR_SERVER_REDIRECTION,
} GrdSessionRdpError;

typedef enum _GrdRdpChannel
{
  GRD_RDP_CHANNEL_NONE,
  GRD_RDP_CHANNEL_AUDIO_INPUT,
  GRD_RDP_CHANNEL_AUDIO_PLAYBACK,
  GRD_RDP_CHANNEL_DISPLAY_CONTROL,
  GRD_RDP_CHANNEL_TELEMETRY,
} GrdRdpChannel;

GrdSessionRdp *grd_session_rdp_new (GrdRdpServer      *rdp_server,
                                    GSocketConnection *connection,
                                    GrdHwAccelNvidia  *hwaccel_nvidia);

GrdRdpSessionMetrics *grd_session_rdp_get_session_metrics (GrdSessionRdp *session_rdp);

void grd_session_rdp_notify_error (GrdSessionRdp      *session_rdp,
                                   GrdSessionRdpError  error_info);

void grd_session_rdp_tear_down_channel (GrdSessionRdp *session_rdp,
                                        GrdRdpChannel  channel);

uint32_t grd_session_rdp_acquire_stream_id (GrdSessionRdp     *session_rdp,
                                            GrdRdpStreamOwner *stream_owner);

void grd_session_rdp_release_stream_id (GrdSessionRdp *session_rdp,
                                        uint32_t       stream_id);

gboolean grd_session_rdp_is_client_mstsc (GrdSessionRdp *session_rdp);

gboolean grd_session_rdp_send_server_redirection (GrdSessionRdp *session_rdp,
                                                  const char    *routing_token,
                                                  const char    *username,
                                                  const char    *password,
                                                  const char    *certificate);

#endif /* GRD_SESSION_RDP_H */
