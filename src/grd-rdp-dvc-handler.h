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

#pragma once

#include <freerdp/channels/wtsvc.h>
#include <glib-object.h>

#define GRD_TYPE_RDP_DVC_HANDLER (grd_rdp_dvc_handler_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpDvcHandler, grd_rdp_dvc_handler,
                      GRD, RDP_DVC_HANDLER, GObject)

typedef void (* GrdRdpDVCCreationStatusCallback) (gpointer user_data,
                                                  int32_t  creation_status);

GrdRdpDvcHandler *grd_rdp_dvc_handler_new (HANDLE vcm);

uint32_t grd_rdp_dvc_handler_subscribe_dvc_creation_status (GrdRdpDvcHandler                *dvc_handler,
                                                            uint32_t                         channel_id,
                                                            GrdRdpDVCCreationStatusCallback  callback,
                                                            gpointer                         callback_user_data);

void grd_rdp_dvc_handler_unsubscribe_dvc_creation_status (GrdRdpDvcHandler *dvc_handler,
                                                          uint32_t          channel_id,
                                                          uint32_t          subscription_id);
