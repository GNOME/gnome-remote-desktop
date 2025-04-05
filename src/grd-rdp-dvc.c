/*
 * Copyright (C) 2025 Pascal Nowack
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

#include "grd-rdp-dvc.h"

typedef struct
{
  GrdRdpDvcHandler *dvc_handler;
  GrdSessionRdp *session_rdp;
  GrdRdpChannel channel;

  gboolean channel_unavailable;

  uint32_t channel_id;
  uint32_t subscription_id;
  gboolean is_subscribed;

  GSource *channel_teardown_source;
} GrdRdpDvcPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GrdRdpDvc, grd_rdp_dvc,
                                     G_TYPE_OBJECT)

void
grd_rdp_dvc_initialize_base (GrdRdpDvc        *dvc,
                             GrdRdpDvcHandler *dvc_handler,
                             GrdSessionRdp    *session_rdp,
                             GrdRdpChannel     channel)
{
  GrdRdpDvcPrivate *priv = grd_rdp_dvc_get_instance_private (dvc);

  priv->dvc_handler = dvc_handler;
  priv->session_rdp = session_rdp;
  priv->channel = channel;
}

void
grd_rdp_dvc_maybe_init (GrdRdpDvc *dvc)
{
  GrdRdpDvcPrivate *priv = grd_rdp_dvc_get_instance_private (dvc);
  GrdRdpDvcClass *klass = GRD_RDP_DVC_GET_CLASS (dvc);

  if (priv->channel_unavailable)
    return;

  klass->maybe_init (dvc);
}

void
grd_rdp_dvc_queue_channel_tear_down (GrdRdpDvc *dvc)
{
  GrdRdpDvcPrivate *priv = grd_rdp_dvc_get_instance_private (dvc);

  priv->channel_unavailable = TRUE;

  g_source_set_ready_time (priv->channel_teardown_source, 0);
}

void
grd_rdp_dvc_subscribe_creation_status (GrdRdpDvc                       *dvc,
                                       uint32_t                         channel_id,
                                       GrdRdpDVCCreationStatusCallback  callback,
                                       gpointer                         callback_user_data)
{
  GrdRdpDvcPrivate *priv = grd_rdp_dvc_get_instance_private (dvc);

  g_assert (!priv->is_subscribed);

  priv->channel_id = channel_id;

  priv->subscription_id =
    grd_rdp_dvc_handler_subscribe_dvc_creation_status (priv->dvc_handler,
                                                       channel_id,
                                                       callback,
                                                       callback_user_data);
  priv->is_subscribed = TRUE;
}

void
grd_rdp_dvc_maybe_unsubscribe_creation_status (GrdRdpDvc *dvc)
{
  GrdRdpDvcPrivate *priv = grd_rdp_dvc_get_instance_private (dvc);

  if (!priv->is_subscribed)
    return;

  grd_rdp_dvc_handler_unsubscribe_dvc_creation_status (priv->dvc_handler,
                                                       priv->channel_id,
                                                       priv->subscription_id);
  priv->is_subscribed = FALSE;
}

static void
grd_rdp_dvc_dispose (GObject *object)
{
  GrdRdpDvc *dvc = GRD_RDP_DVC (object);
  GrdRdpDvcPrivate *priv = grd_rdp_dvc_get_instance_private (dvc);

  if (priv->channel_teardown_source)
    {
      g_source_destroy (priv->channel_teardown_source);
      g_clear_pointer (&priv->channel_teardown_source, g_source_unref);
    }

  G_OBJECT_CLASS (grd_rdp_dvc_parent_class)->dispose (object);
}

static const char *
channel_to_string (GrdRdpChannel channel)
{
  switch (channel)
    {
    case GRD_RDP_CHANNEL_NONE:
      g_assert_not_reached ();
      break;
    case GRD_RDP_CHANNEL_AUDIO_INPUT:
      return "AUDIO_INPUT";
    case GRD_RDP_CHANNEL_AUDIO_PLAYBACK:
      return "AUDIO_PLAYBACK";
    case GRD_RDP_CHANNEL_DISPLAY_CONTROL:
      return "DISP";
    case GRD_RDP_CHANNEL_GRAPHICS_PIPELINE:
      return "RDPGFX";
    case GRD_RDP_CHANNEL_INPUT:
      return "INPUT";
    case GRD_RDP_CHANNEL_TELEMETRY:
      return "TELEMETRY";
    }

  g_assert_not_reached ();
}

static gboolean
tear_down_channel (gpointer user_data)
{
  GrdRdpDvc *dvc = user_data;
  GrdRdpDvcPrivate *priv = grd_rdp_dvc_get_instance_private (dvc);

  g_debug ("[RDP.%s] Tearing down channel", channel_to_string (priv->channel));

  g_clear_pointer (&priv->channel_teardown_source, g_source_unref);
  grd_session_rdp_tear_down_channel (priv->session_rdp, priv->channel);

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
grd_rdp_dvc_init (GrdRdpDvc *dvc)
{
  GrdRdpDvcPrivate *priv = grd_rdp_dvc_get_instance_private (dvc);
  GSource *channel_teardown_source;

  channel_teardown_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (channel_teardown_source, tear_down_channel,
                         dvc, NULL);
  g_source_set_ready_time (channel_teardown_source, -1);
  g_source_attach (channel_teardown_source, NULL);
  priv->channel_teardown_source = channel_teardown_source;
}

static void
grd_rdp_dvc_class_init (GrdRdpDvcClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_dvc_dispose;
}
