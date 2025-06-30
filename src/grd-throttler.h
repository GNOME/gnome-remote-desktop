/*
 * Copyright (C) 2025 Red Hat
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

#ifndef GRD_THROTTLER_H
#define GRD_THROTTLER_H

#include <gio/gio.h>
#include <glib-object.h>

#include "grd-types.h"

typedef struct _GrdThrottlerLimits GrdThrottlerLimits;

#define GRD_TYPE_THROTTLER (grd_throttler_get_type())
G_DECLARE_FINAL_TYPE (GrdThrottler, grd_throttler, GRD, THROTTLER, GObject)

typedef void (* GrdThrottlerAllowCallback) (GrdThrottler      *throttler,
                                            GSocketConnection *connection,
                                            gpointer           user_data);

void
grd_throttler_handle_connection (GrdThrottler      *throttler,
                                 GSocketConnection *connection);

void
grd_throttler_limits_set_max_global_connections (GrdThrottlerLimits *limits,
                                                 int                 limit);

GrdThrottlerLimits *
grd_throttler_limits_new (GrdContext *context);

GrdThrottler *
grd_throttler_new (GrdThrottlerLimits        *limits,
                   GrdThrottlerAllowCallback  allow_callback,
                   gpointer                   user_data);

#endif /* GRD_THROTTLER_H */
