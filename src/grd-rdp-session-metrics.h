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

#ifndef GRD_RDP_SESSION_METRICS_H
#define GRD_RDP_SESSION_METRICS_H

#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_SESSION_METRICS (grd_rdp_session_metrics_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpSessionMetrics, grd_rdp_session_metrics,
                      GRD, RDP_SESSION_METRICS, GObject)

typedef enum
{
  GRD_RDP_PHASE_SESSION_INITIALIZATION,
  GRD_RDP_PHASE_POST_CONNECT,
  GRD_RDP_PHASE_SESSION_READY,
  GRD_RDP_PHASE_SESSION_STARTED,
} GrdRdpPhase;

GrdRdpSessionMetrics *grd_rdp_session_metrics_new (void);

void grd_rdp_session_metrics_notify_phase_completion (GrdRdpSessionMetrics *session_metrics,
                                                      GrdRdpPhase           phase);

void grd_rdp_session_metrics_notify_frame_reception (GrdRdpSessionMetrics *session_metrics,
                                                     GrdRdpSurface        *rdp_surface);

void grd_rdp_session_metrics_notify_frame_transmission (GrdRdpSessionMetrics *session_metrics,
                                                        GrdRdpSurface        *rdp_surface);

void grd_rdp_session_metrics_notify_layout_change (GrdRdpSessionMetrics *session_metrics);

void grd_rdp_session_metrics_prepare_surface_metrics (GrdRdpSessionMetrics *session_metrics,
                                                      GList                *surfaces);

#endif /* GRD_RDP_SESSION_METRICS_H */
