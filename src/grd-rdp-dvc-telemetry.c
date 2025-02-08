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

#include "config.h"

#include "grd-rdp-dvc-telemetry.h"

#include <freerdp/server/telemetry.h>

#include "grd-session-rdp.h"

struct _GrdRdpDvcTelemetry
{
  GrdRdpDvc parent;

  TelemetryServerContext *telemetry_context;
  gboolean channel_opened;
  gboolean channel_unavailable;

  GrdSessionRdp *session_rdp;

  GSource *channel_teardown_source;
};

G_DEFINE_TYPE (GrdRdpDvcTelemetry, grd_rdp_dvc_telemetry,
               GRD_TYPE_RDP_DVC)

static void
grd_rdp_dvc_telemetry_maybe_init (GrdRdpDvc *dvc)
{
  GrdRdpDvcTelemetry *telemetry = GRD_RDP_DVC_TELEMETRY (dvc);
  TelemetryServerContext *telemetry_context;

  if (telemetry->channel_opened || telemetry->channel_unavailable)
    return;

  telemetry_context = telemetry->telemetry_context;
  if (telemetry_context->Open (telemetry_context))
    {
      g_warning ("[RDP.TELEMETRY] Failed to open channel. "
                 "Terminating protocol");
      telemetry->channel_unavailable = TRUE;
      g_source_set_ready_time (telemetry->channel_teardown_source, 0);
      return;
    }
  telemetry->channel_opened = TRUE;
}

static void
dvc_creation_status (gpointer user_data,
                     int32_t  creation_status)
{
  GrdRdpDvcTelemetry *telemetry = user_data;

  if (creation_status < 0)
    {
      g_debug ("[RDP.TELEMETRY] Failed to open channel (CreationStatus %i). "
               "Terminating protocol", creation_status);
      g_source_set_ready_time (telemetry->channel_teardown_source, 0);
    }
}

static BOOL
telemetry_channel_id_assigned (TelemetryServerContext *telemetry_context,
                               uint32_t                channel_id)
{
  GrdRdpDvcTelemetry *telemetry = telemetry_context->userdata;
  GrdRdpDvc *dvc = GRD_RDP_DVC (telemetry);

  g_debug ("[RDP.TELEMETRY] DVC channel id assigned to id %u", channel_id);

  grd_rdp_dvc_subscribe_creation_status (dvc, channel_id,
                                         dvc_creation_status,
                                         telemetry);

  return TRUE;
}

static uint32_t
telemetry_rdp_telemetry (TelemetryServerContext            *telemetry_context,
                         const TELEMETRY_RDP_TELEMETRY_PDU *rdp_telemetry)
{
  GrdRdpDvcTelemetry *telemetry = telemetry_context->userdata;

  g_debug ("[RDP.TELEMETRY] Client connection metrics: "
           "PromptForCredentialsMillis: %u, "
           "PromptForCredentialsDoneMillis: %u, "
           "GraphicsChannelOpenedMillis: %u, "
           "FirstGraphicsReceivedMillis: %u",
           rdp_telemetry->PromptForCredentialsMillis,
           rdp_telemetry->PromptForCredentialsDoneMillis,
           rdp_telemetry->GraphicsChannelOpenedMillis,
           rdp_telemetry->FirstGraphicsReceivedMillis);

  g_debug ("[RDP.TELEMETRY] diff (CredentialsDone, RDPGFX opened): %ums; "
           "diff (RDPGFX opened, first graphics): %ums; "
           "diff (CredentialsDone, first graphics): %ums",
           rdp_telemetry->GraphicsChannelOpenedMillis -
           rdp_telemetry->PromptForCredentialsDoneMillis,
           rdp_telemetry->FirstGraphicsReceivedMillis -
           rdp_telemetry->GraphicsChannelOpenedMillis,
           rdp_telemetry->FirstGraphicsReceivedMillis -
           rdp_telemetry->PromptForCredentialsDoneMillis);

  g_source_set_ready_time (telemetry->channel_teardown_source, 0);

  return CHANNEL_RC_OK;
}

GrdRdpDvcTelemetry *
grd_rdp_dvc_telemetry_new (GrdSessionRdp    *session_rdp,
                           GrdRdpDvcHandler *dvc_handler,
                           HANDLE            vcm,
                           rdpContext       *rdp_context)
{
  GrdRdpDvcTelemetry *telemetry;
  TelemetryServerContext *telemetry_context;

  telemetry = g_object_new (GRD_TYPE_RDP_DVC_TELEMETRY, NULL);
  telemetry_context = telemetry_server_context_new (vcm);
  if (!telemetry_context)
    g_error ("[RDP.TELEMETRY] Failed to allocate server context (OOM)");

  telemetry->telemetry_context = telemetry_context;
  telemetry->session_rdp = session_rdp;

  grd_rdp_dvc_set_dvc_handler (GRD_RDP_DVC (telemetry), dvc_handler);

  telemetry_context->ChannelIdAssigned = telemetry_channel_id_assigned;
  telemetry_context->RdpTelemetry = telemetry_rdp_telemetry;
  telemetry_context->rdpcontext = rdp_context;
  telemetry_context->userdata = telemetry;

  return telemetry;
}

static void
grd_rdp_dvc_telemetry_dispose (GObject *object)
{
  GrdRdpDvcTelemetry *telemetry = GRD_RDP_DVC_TELEMETRY (object);
  GrdRdpDvc *dvc = GRD_RDP_DVC (telemetry);

  if (telemetry->channel_opened)
    {
      telemetry->telemetry_context->Close (telemetry->telemetry_context);
      telemetry->channel_opened = FALSE;
    }
  grd_rdp_dvc_maybe_unsubscribe_creation_status (dvc);

  if (telemetry->channel_teardown_source)
    {
      g_source_destroy (telemetry->channel_teardown_source);
      g_clear_pointer (&telemetry->channel_teardown_source, g_source_unref);
    }

  g_clear_pointer (&telemetry->telemetry_context,
                   telemetry_server_context_free);

  G_OBJECT_CLASS (grd_rdp_dvc_telemetry_parent_class)->dispose (object);
}

static gboolean
tear_down_channel (gpointer user_data)
{
  GrdRdpDvcTelemetry *telemetry = user_data;

  g_debug ("[RDP.TELEMETRY] Tearing down channel");

  g_clear_pointer (&telemetry->channel_teardown_source, g_source_unref);

  grd_session_rdp_tear_down_channel (telemetry->session_rdp,
                                     GRD_RDP_CHANNEL_TELEMETRY);

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
grd_rdp_dvc_telemetry_init (GrdRdpDvcTelemetry *telemetry)
{
  GSource *channel_teardown_source;

  channel_teardown_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (channel_teardown_source, tear_down_channel,
                         telemetry, NULL);
  g_source_set_ready_time (channel_teardown_source, -1);
  g_source_attach (channel_teardown_source, NULL);
  telemetry->channel_teardown_source = channel_teardown_source;
}

static void
grd_rdp_dvc_telemetry_class_init (GrdRdpDvcTelemetryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdRdpDvcClass *dvc_class = GRD_RDP_DVC_CLASS (klass);

  object_class->dispose = grd_rdp_dvc_telemetry_dispose;

  dvc_class->maybe_init = grd_rdp_dvc_telemetry_maybe_init;
}
