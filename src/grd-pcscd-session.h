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

#include <gio/gio.h>

#define GRD_TYPE_PCSCD_SESSION (grd_pcscd_session_get_type ())
G_DECLARE_FINAL_TYPE (GrdPcscdSession, grd_pcscd_session,
                      GRD, PCSCD_SESSION, GObject)

GrdPcscdSession *grd_pcscd_session_new (const char      *session_id,
                                        int              fd,
                                        GDBusConnection *system_connection);

const char *grd_pcscd_session_get_session_id (GrdPcscdSession *session);
