/*
 * Copyright (C) 2015 Red Hat Inc.
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
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#ifndef GRD_DAEMON_H
#define GRD_DAEMON_H

#include <gio/gio.h>

#include "grd-context.h"

typedef struct _GrdDaemon GrdDaemon;

#define GRD_TYPE_DAEMON (grd_daemon_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdDaemon, grd_daemon, GRD, DAEMON, GApplication)

struct _GrdDaemonClass
{
    GApplicationClass parent_class;

    gboolean (*is_daemon_ready) (GrdDaemon *daemon);
};

GrdRdpServer *grd_daemon_get_rdp_server (GrdDaemon *daemon);

GCancellable *grd_daemon_get_cancellable (GrdDaemon *daemon);

GrdContext *grd_daemon_get_context (GrdDaemon *daemon);

void grd_daemon_maybe_enable_services (GrdDaemon *daemon);

void grd_daemon_acquire_mutter_dbus_proxies (GrdDaemon *daemon);

#endif /* GRD_DAEMON_H */
