/*
 * Copyright (C) 2026 Red Hat Inc.
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
 *
 * Written by:
 *     Joan Torres Lopez <joantolo@redhat.com>
 */

#pragma once

#include <freerdp/server/rdpdr.h>
#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_SMARTCARD (grd_rdp_smartcard_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpSmartcard, grd_rdp_smartcard,
                      GRD, RDP_SMARTCARD, GObject)

GrdRdpSmartcard *grd_rdp_smartcard_new (GrdSessionRdp      *session_rdp,
                                        RdpdrServerContext *rdpdr_context);

void grd_rdp_smartcard_invoke_shutdown (GrdRdpSmartcard *smartcard);
