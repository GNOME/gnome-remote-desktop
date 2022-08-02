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

#include "config.h"

#include "grd-rdp-display-control.h"

#include "grd-session-rdp.h"

struct _GrdRdpDisplayControl
{
  GObject parent;

  DispServerContext *disp_context;
  HANDLE stop_event;
  gboolean channel_opened;
  gboolean channel_unavailable;

  GrdSessionRdp *session_rdp;
};

G_DEFINE_TYPE (GrdRdpDisplayControl, grd_rdp_display_control, G_TYPE_OBJECT)

static uint32_t
disp_monitor_layout (DispServerContext                        *disp_context,
                     const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *monitor_layout_pdu)
{
  GrdRdpDisplayControl *display_control = disp_context->custom;
  GrdSessionRdp *session_rdp = display_control->session_rdp;
  GrdRdpMonitorConfig *monitor_config;

  if (monitor_layout_pdu->NumMonitors > disp_context->MaxNumMonitors)
    {
      g_warning ("[RDP.DISP] Received invalid monitor layout pdu "
                 "(exceeded maximum monitor count). Ignoring PDU...");
      return CHANNEL_RC_OK;
    }

  g_debug ("[RDP.DISP] Received new monitor layout. PDU contains %i monitors",
           monitor_layout_pdu->NumMonitors);

  monitor_config =
    grd_rdp_monitor_config_new_from_disp_monitor_layout (monitor_layout_pdu);
  if (!monitor_config)
    {
      g_warning ("[RDP.DISP] Monitor layout PDU is invalid. Ignoring PDU...");
      return CHANNEL_RC_OK;
    }

  grd_session_rdp_submit_new_monitor_config (session_rdp, monitor_config);

  return CHANNEL_RC_OK;
}

void
grd_rdp_display_control_maybe_init (GrdRdpDisplayControl *display_control)
{
  DispServerContext *disp_context;

  if (display_control->channel_opened || display_control->channel_unavailable)
    return;

  if (WaitForSingleObject (display_control->stop_event, 0) == WAIT_OBJECT_0)
    return;

  disp_context = display_control->disp_context;
  if (disp_context->Open (disp_context))
    {
      g_warning ("[RDP.DISP] An error occurred while opening the DISP channel.");
      display_control->channel_unavailable = TRUE;
      return;
    }

  g_debug ("[RDP.DISP] Channel opened successfully. "
           "Ready for receiving new monitor layouts");
  disp_context->DisplayControlCaps (disp_context);

  display_control->channel_opened = TRUE;
}

GrdRdpDisplayControl *
grd_rdp_display_control_new (GrdSessionRdp *session_rdp,
                             HANDLE         vcm,
                             HANDLE         stop_event,
                             uint32_t       max_monitor_count)
{
  GrdRdpDisplayControl *display_control;
  DispServerContext *disp_context;

  display_control = g_object_new (GRD_TYPE_RDP_DISPLAY_CONTROL, NULL);
  disp_context = disp_server_context_new (vcm);
  if (!disp_context)
    g_error ("[RDP.DISP] Failed to create server context");

  display_control->disp_context = disp_context;
  display_control->stop_event = stop_event;
  display_control->session_rdp = session_rdp;

  disp_context->MaxNumMonitors = max_monitor_count;
  disp_context->MaxMonitorAreaFactorA = 8192;
  disp_context->MaxMonitorAreaFactorB = 8192;

  disp_context->DispMonitorLayout = disp_monitor_layout;
  disp_context->custom = display_control;

  return display_control;
}

static void
grd_rdp_display_control_dispose (GObject *object)
{
  GrdRdpDisplayControl *display_control = GRD_RDP_DISPLAY_CONTROL (object);

  if (display_control->channel_opened)
    {
      display_control->disp_context->Close (display_control->disp_context);
      display_control->channel_opened = FALSE;
    }

  g_clear_pointer (&display_control->disp_context, disp_server_context_free);

  G_OBJECT_CLASS (grd_rdp_display_control_parent_class)->dispose (object);
}

static void
grd_rdp_display_control_init (GrdRdpDisplayControl *display_control)
{
}

static void
grd_rdp_display_control_class_init (GrdRdpDisplayControlClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_display_control_dispose;
}
