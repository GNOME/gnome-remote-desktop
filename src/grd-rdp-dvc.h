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

#ifndef GRD_RDP_DVC_H
#define GRD_RDP_DVC_H

#include <glib-object.h>

#include "grd-rdp-dvc-handler.h"
#include "grd-session-rdp.h"

#define GRD_TYPE_RDP_DVC (grd_rdp_dvc_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdRdpDvc, grd_rdp_dvc,
                          GRD, RDP_DVC, GObject)

struct _GrdRdpDvcClass
{
  GObjectClass parent_class;

  void (* maybe_init) (GrdRdpDvc *dvc);
};

void grd_rdp_dvc_initialize_base (GrdRdpDvc        *dvc,
                                  GrdRdpDvcHandler *dvc_handler,
                                  GrdSessionRdp    *session_rdp,
                                  GrdRdpChannel     channel);

void grd_rdp_dvc_maybe_init (GrdRdpDvc *dvc);

void grd_rdp_dvc_queue_channel_tear_down (GrdRdpDvc *dvc);

void grd_rdp_dvc_subscribe_creation_status (GrdRdpDvc                       *dvc,
                                            uint32_t                         channel_id,
                                            GrdRdpDVCCreationStatusCallback  callback,
                                            gpointer                         callback_user_data);

void grd_rdp_dvc_maybe_unsubscribe_creation_status (GrdRdpDvc *dvc);

#endif /* GRD_RDP_DVC_H */
