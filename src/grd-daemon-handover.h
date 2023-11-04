/*
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
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
 *     Joan Torres <joan.torres@suse.com>
 */

#ifndef GRD_DAEMON_HANDOVER_H
#define GRD_DAEMON_HANDOVER_H

#include "grd-daemon.h"

#define GRD_TYPE_DAEMON_HANDOVER (grd_daemon_handover_get_type ())
G_DECLARE_FINAL_TYPE (GrdDaemonHandover,
                      grd_daemon_handover,
                      GRD, DAEMON_HANDOVER,
                      GrdDaemon)

GrdDaemonHandover *grd_daemon_handover_new (GError **error);

#endif /* GRD_DAEMON_HANDOVER */
