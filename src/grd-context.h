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

#ifndef GRD_CONTEXT_H
#define GRD_CONTEXT_H

#include <glib-object.h>

#include "grd-dbus-mutter-remote-desktop.h"
#include "grd-dbus-mutter-screen-cast.h"
#include "grd-settings.h"
#include "grd-types.h"

typedef enum _GrdRuntimeMode
{
  GRD_RUNTIME_MODE_SCREEN_SHARE,
  GRD_RUNTIME_MODE_HEADLESS,
} GrdRuntimeMode;

#define GRD_TYPE_CONTEXT (grd_context_get_type ())
G_DECLARE_FINAL_TYPE (GrdContext, grd_context, GRD, CONTEXT, GObject)

GrdContext * grd_context_new (GrdRuntimeMode   runtime_mode,
                              GError         **error);

GrdDBusMutterRemoteDesktop * grd_context_get_mutter_remote_desktop_proxy (GrdContext *context);

GrdDBusMutterScreenCast * grd_context_get_mutter_screen_cast_proxy (GrdContext *context);

void grd_context_set_mutter_remote_desktop_proxy (GrdContext                 *context,
                                                  GrdDBusMutterRemoteDesktop *proxy);

void grd_context_set_mutter_screen_cast_proxy (GrdContext              *context,
                                               GrdDBusMutterScreenCast *proxy);

GrdSettings * grd_context_get_settings (GrdContext *context);

GrdCredentials * grd_context_get_credentials (GrdContext *context);

GrdEglThread * grd_context_get_egl_thread (GrdContext *context);

GrdRuntimeMode grd_context_get_runtime_mode (GrdContext *context);

void grd_context_notify_daemon_ready (GrdContext *context);

#endif /* GRD_CONTEXT_H */
