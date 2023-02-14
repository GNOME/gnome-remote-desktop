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

#include "grd-rdp-dvc.h"
#include "grd-session-rdp.h"

struct _GrdRdpDisplayControl
{
  GObject parent;

  DispServerContext *disp_context;
  gboolean channel_opened;
  gboolean channel_unavailable;
  gboolean pending_caps;

  uint32_t channel_id;
  uint32_t dvc_subscription_id;
  gboolean subscribed_status;

  GrdSessionRdp *session_rdp;
  GrdRdpDvc *rdp_dvc;

  GSource *channel_teardown_source;
};

G_DEFINE_TYPE (GrdRdpDisplayControl, grd_rdp_display_control, G_TYPE_OBJECT)

void
grd_rdp_display_control_maybe_init (GrdRdpDisplayControl *display_control)
{
  DispServerContext *disp_context;

  if (display_control->channel_opened || display_control->channel_unavailable)
    return;

  disp_context = display_control->disp_context;
  if (disp_context->Open (disp_context))
    {
      g_warning ("[RDP.DISP] Failed to open channel. Terminating protocol");
      display_control->channel_unavailable = TRUE;
      g_source_set_ready_time (display_control->channel_teardown_source, 0);
      return;
    }
  display_control->channel_opened = TRUE;
}

static void
dvc_creation_status (gpointer user_data,
                     int32_t  creation_status)
{
  GrdRdpDisplayControl *display_control = user_data;
  DispServerContext *disp_context = display_control->disp_context;

  if (creation_status < 0)
    {
      g_debug ("[RDP.DISP] Failed to open channel (CreationStatus %i). "
               "Terminating protocol", creation_status);
      g_source_set_ready_time (display_control->channel_teardown_source, 0);
      return;
    }

  g_debug ("[RDP.DISP] Channel opened successfully. "
           "Ready for receiving new monitor layouts");
  display_control->pending_caps = FALSE;
  disp_context->DisplayControlCaps (disp_context);
}

static BOOL
disp_channel_id_assigned (DispServerContext *disp_context,
                          uint32_t           channel_id)
{
  GrdRdpDisplayControl *display_control = disp_context->custom;

  g_debug ("[RDP.DISP] DVC channel id assigned to id %u", channel_id);
  display_control->channel_id = channel_id;

  display_control->dvc_subscription_id =
    grd_rdp_dvc_subscribe_dvc_creation_status (display_control->rdp_dvc,
                                               channel_id,
                                               dvc_creation_status,
                                               display_control);
  display_control->subscribed_status = TRUE;

  return TRUE;
}

static uint32_t
disp_monitor_layout (DispServerContext                        *disp_context,
                     const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *monitor_layout_pdu)
{
  GrdRdpDisplayControl *display_control = disp_context->custom;
  GrdSessionRdp *session_rdp = display_control->session_rdp;
  GrdRdpMonitorConfig *monitor_config;
  g_autoptr (GError) error = NULL;

  if (display_control->pending_caps)
    {
      g_warning ("[RDP.DISP] Protocol violation: Received monitor layout PDU "
                 "before being able to send capabilities. Terminating session");
      grd_session_rdp_notify_error (session_rdp,
                                    GRD_SESSION_RDP_ERROR_BAD_MONITOR_DATA);
      return CHANNEL_RC_OK;
    }

  if (monitor_layout_pdu->NumMonitors > disp_context->MaxNumMonitors)
    {
      g_warning ("[RDP.DISP] Protocol violation: Invalid monitor layout: "
                 "(exceeded maximum monitor count). Terminating session");
      grd_session_rdp_notify_error (session_rdp,
                                    GRD_SESSION_RDP_ERROR_BAD_MONITOR_DATA);
      return CHANNEL_RC_OK;
    }

  monitor_config =
    grd_rdp_monitor_config_new_from_disp_monitor_layout (monitor_layout_pdu,
                                                         &error);
  if (!monitor_config)
    {
      g_warning ("[RDP.DISP] Received invalid monitor layout from client: %s. "
                 "Terminating session", error->message);
      grd_session_rdp_notify_error (session_rdp,
                                    GRD_SESSION_RDP_ERROR_BAD_MONITOR_DATA);
      return CHANNEL_RC_OK;
    }

  g_debug ("[RDP.DISP] Received new monitor layout. PDU contains %i monitors",
           monitor_layout_pdu->NumMonitors);

  grd_session_rdp_submit_new_monitor_config (session_rdp, monitor_config);

  return CHANNEL_RC_OK;
}

GrdRdpDisplayControl *
grd_rdp_display_control_new (GrdSessionRdp *session_rdp,
                             GrdRdpDvc     *rdp_dvc,
                             HANDLE         vcm,
                             uint32_t       max_monitor_count)
{
  GrdRdpDisplayControl *display_control;
  DispServerContext *disp_context;

  display_control = g_object_new (GRD_TYPE_RDP_DISPLAY_CONTROL, NULL);
  disp_context = disp_server_context_new (vcm);
  if (!disp_context)
    g_error ("[RDP.DISP] Failed to create server context");

  display_control->disp_context = disp_context;
  display_control->session_rdp = session_rdp;
  display_control->rdp_dvc = rdp_dvc;

  disp_context->MaxNumMonitors = max_monitor_count;
  disp_context->MaxMonitorAreaFactorA = 8192;
  disp_context->MaxMonitorAreaFactorB = 8192;

  disp_context->ChannelIdAssigned = disp_channel_id_assigned;
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
  if (display_control->subscribed_status)
    {
      grd_rdp_dvc_unsubscribe_dvc_creation_status (display_control->rdp_dvc,
                                                   display_control->channel_id,
                                                   display_control->dvc_subscription_id);
      display_control->subscribed_status = FALSE;
    }

  if (display_control->channel_teardown_source)
    {
      g_source_destroy (display_control->channel_teardown_source);
      g_clear_pointer (&display_control->channel_teardown_source, g_source_unref);
    }

  g_clear_pointer (&display_control->disp_context, disp_server_context_free);

  G_OBJECT_CLASS (grd_rdp_display_control_parent_class)->dispose (object);
}

static gboolean
tear_down_channel (gpointer user_data)
{
  GrdRdpDisplayControl *display_control = user_data;

  g_debug ("[RDP.DISP] Tearing down channel");

  g_clear_pointer (&display_control->channel_teardown_source, g_source_unref);

  grd_session_rdp_tear_down_channel (display_control->session_rdp,
                                     GRD_RDP_CHANNEL_DISPLAY_CONTROL);

  return G_SOURCE_REMOVE;
}

static gboolean
source_dispatch (GSource     *source,
                 GSourceFunc  callback,
                 gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs source_funcs =
{
  .dispatch = source_dispatch,
};

static void
grd_rdp_display_control_init (GrdRdpDisplayControl *display_control)
{
  GSource *channel_teardown_source;

  display_control->pending_caps = TRUE;

  channel_teardown_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (channel_teardown_source, tear_down_channel,
                         display_control, NULL);
  g_source_set_ready_time (channel_teardown_source, -1);
  g_source_attach (channel_teardown_source, NULL);
  display_control->channel_teardown_source = channel_teardown_source;
}

static void
grd_rdp_display_control_class_init (GrdRdpDisplayControlClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_display_control_dispose;
}
