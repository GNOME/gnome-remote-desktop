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

  uint32_t channel_id;
  uint32_t subscription_id;
  gboolean is_subscribed;
} GrdRdpDvcPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GrdRdpDvc, grd_rdp_dvc,
                                     G_TYPE_OBJECT)

void
grd_rdp_dvc_set_dvc_handler (GrdRdpDvc        *dvc,
                             GrdRdpDvcHandler *dvc_handler)
{
  GrdRdpDvcPrivate *priv = grd_rdp_dvc_get_instance_private (dvc);

  priv->dvc_handler = dvc_handler;
}

void
grd_rdp_dvc_maybe_init (GrdRdpDvc *dvc)
{
  GrdRdpDvcClass *klass = GRD_RDP_DVC_GET_CLASS (dvc);

  klass->maybe_init (dvc);
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
grd_rdp_dvc_init (GrdRdpDvc *dvc)
{
}

static void
grd_rdp_dvc_class_init (GrdRdpDvcClass *klass)
{
}
