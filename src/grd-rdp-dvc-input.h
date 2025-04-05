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

#ifndef GRD_RDP_DVC_INPUT_H
#define GRD_RDP_DVC_INPUT_H

#include "grd-rdp-dvc.h"

#define GRD_TYPE_RDP_DVC_INPUT (grd_rdp_dvc_input_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpDvcInput, grd_rdp_dvc_input,
                      GRD, RDP_DVC_INPUT, GrdRdpDvc)

GrdRdpDvcInput *grd_rdp_dvc_input_new (GrdRdpLayoutManager *layout_manager,
                                       GrdSessionRdp       *session_rdp,
                                       GrdRdpDvcHandler    *dvc_handler,
                                       HANDLE               vcm);

#endif /* GRD_RDP_DVC_INPUT_H */
