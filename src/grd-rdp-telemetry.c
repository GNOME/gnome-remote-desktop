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

#include "grd-rdp-telemetry.h"

#include "grd-session-rdp.h"

#define PROTOCOL_TIMEOUT_MS (10 * 1000)

struct _GrdRdpTelemetry
{
  GObject parent;

  TelemetryServerContext *telemetry_context;
  HANDLE stop_event;
  gboolean channel_opened;
  gboolean channel_unavailable;

  uint32_t channel_id;
  uint32_t dvc_subscription_id;
  gboolean subscribed_status;

  GrdSessionRdp *session_rdp;

  GMutex protocol_timeout_mutex;
  GSource *channel_teardown_source;
  GSource *protocol_timeout_source;
};

G_DEFINE_TYPE (GrdRdpTelemetry, grd_rdp_telemetry, G_TYPE_OBJECT)

static gboolean
initiate_channel_teardown (gpointer user_data)
{
  GrdRdpTelemetry *telemetry = user_data;

  g_debug ("[RDP.TELEMETRY] Client did not respond to protocol initiation. "
           "Terminating protocol");

  g_mutex_lock (&telemetry->protocol_timeout_mutex);
  g_clear_pointer (&telemetry->protocol_timeout_source, g_source_unref);
  g_mutex_unlock (&telemetry->protocol_timeout_mutex);

  g_source_set_ready_time (telemetry->channel_teardown_source, 0);

  return G_SOURCE_REMOVE;
}

void
grd_rdp_telemetry_maybe_init (GrdRdpTelemetry *telemetry)
{
  TelemetryServerContext *telemetry_context;

  if (telemetry->channel_opened || telemetry->channel_unavailable)
    return;

  if (WaitForSingleObject (telemetry->stop_event, 0) != WAIT_TIMEOUT)
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

  g_assert (!telemetry->protocol_timeout_source);

  telemetry->protocol_timeout_source =
    g_timeout_source_new (PROTOCOL_TIMEOUT_MS);
  g_source_set_callback (telemetry->protocol_timeout_source,
                         initiate_channel_teardown, telemetry, NULL);
  g_source_attach (telemetry->protocol_timeout_source, NULL);
}

static void
dvc_creation_status (gpointer user_data,
                     int32_t  creation_status)
{
  GrdRdpTelemetry *telemetry = user_data;

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
  GrdRdpTelemetry *telemetry = telemetry_context->userdata;

  g_debug ("[RDP.TELEMETRY] DVC channel id assigned to id %u", channel_id);
  telemetry->channel_id = channel_id;

  telemetry->dvc_subscription_id =
    grd_session_rdp_subscribe_dvc_creation_status (telemetry->session_rdp,
                                                   channel_id,
                                                   dvc_creation_status,
                                                   telemetry);
  telemetry->subscribed_status = TRUE;

  return TRUE;
}

static uint32_t
telemetry_rdp_telemetry (TelemetryServerContext            *telemetry_context,
                         const TELEMETRY_RDP_TELEMETRY_PDU *rdp_telemetry)
{
  GrdRdpTelemetry *telemetry = telemetry_context->userdata;

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

  g_mutex_lock (&telemetry->protocol_timeout_mutex);
  if (telemetry->protocol_timeout_source)
    {
      g_source_destroy (telemetry->protocol_timeout_source);
      g_clear_pointer (&telemetry->protocol_timeout_source, g_source_unref);
    }
  g_mutex_unlock (&telemetry->protocol_timeout_mutex);

  g_source_set_ready_time (telemetry->channel_teardown_source, 0);

  return CHANNEL_RC_OK;
}

GrdRdpTelemetry *
grd_rdp_telemetry_new (GrdSessionRdp *session_rdp,
                       HANDLE         vcm,
                       HANDLE         stop_event,
                       rdpContext    *rdp_context)
{
  GrdRdpTelemetry *telemetry;
  TelemetryServerContext *telemetry_context;

  telemetry = g_object_new (GRD_TYPE_RDP_TELEMETRY, NULL);
  telemetry_context = telemetry_server_context_new (vcm);
  if (!telemetry_context)
    g_error ("[RDP.TELEMETRY] Failed to allocate server context (OOM)");

  telemetry->telemetry_context = telemetry_context;
  telemetry->stop_event = stop_event;
  telemetry->session_rdp = session_rdp;

  telemetry_context->ChannelIdAssigned = telemetry_channel_id_assigned;
  telemetry_context->RdpTelemetry = telemetry_rdp_telemetry;
  telemetry_context->rdpcontext = rdp_context;
  telemetry_context->userdata = telemetry;

  return telemetry;
}

static void
grd_rdp_telemetry_dispose (GObject *object)
{
  GrdRdpTelemetry *telemetry = GRD_RDP_TELEMETRY (object);

  if (telemetry->channel_opened)
    {
      telemetry->telemetry_context->Close (telemetry->telemetry_context);
      telemetry->channel_opened = FALSE;
    }
  if (telemetry->subscribed_status)
    {
      grd_session_rdp_unsubscribe_dvc_creation_status (telemetry->session_rdp,
                                                       telemetry->channel_id,
                                                       telemetry->dvc_subscription_id);
      telemetry->subscribed_status = FALSE;
    }

  if (telemetry->protocol_timeout_source)
    {
      g_source_destroy (telemetry->protocol_timeout_source);
      g_clear_pointer (&telemetry->protocol_timeout_source,  g_source_unref);
    }
  if (telemetry->channel_teardown_source)
    {
      g_source_destroy (telemetry->channel_teardown_source);
      g_clear_pointer (&telemetry->channel_teardown_source, g_source_unref);
    }

  g_clear_pointer (&telemetry->telemetry_context,
                   telemetry_server_context_free);

  G_OBJECT_CLASS (grd_rdp_telemetry_parent_class)->dispose (object);
}

static void
grd_rdp_telemetry_finalize (GObject *object)
{
  GrdRdpTelemetry *telemetry = GRD_RDP_TELEMETRY (object);

  g_mutex_clear (&telemetry->protocol_timeout_mutex);

  G_OBJECT_CLASS (grd_rdp_telemetry_parent_class)->finalize (object);
}

static gboolean
tear_down_channel (gpointer user_data)
{
  GrdRdpTelemetry *telemetry = user_data;

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
grd_rdp_telemetry_init (GrdRdpTelemetry *telemetry)
{
  GSource *channel_teardown_source;

  g_mutex_init (&telemetry->protocol_timeout_mutex);

  channel_teardown_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (channel_teardown_source, tear_down_channel,
                         telemetry, NULL);
  g_source_set_ready_time (channel_teardown_source, -1);
  g_source_attach (channel_teardown_source, NULL);
  telemetry->channel_teardown_source = channel_teardown_source;
}

static void
grd_rdp_telemetry_class_init (GrdRdpTelemetryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_telemetry_dispose;
  object_class->finalize = grd_rdp_telemetry_finalize;
}
