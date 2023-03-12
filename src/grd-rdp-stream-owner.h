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

#ifndef GRD_RDP_STREAM_OWNER_H
#define GRD_RDP_STREAM_OWNER_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_STREAM_OWNER (grd_rdp_stream_owner_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdRdpStreamOwner, grd_rdp_stream_owner,
                          GRD, RDP_STREAM_OWNER, GObject)

struct _GrdRdpStreamOwnerClass
{
  GObjectClass parent_class;

  void (* on_stream_created) (GrdRdpStreamOwner *stream_owner,
                              uint32_t           stream_id,
                              GrdStream         *stream);
};

void grd_rdp_stream_owner_notify_stream_created (GrdRdpStreamOwner *stream_owner,
                                                 uint32_t           stream_id,
                                                 GrdStream         *stream);

#endif /* GRD_RDP_STREAM_OWNER_H */
