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

#include "config.h"

#include "grd-context.h"

#include <gst/gst.h>
#include <pinos/client/pinos.h>

#include "grd-dbus-remote-desktop.h"
#include "grd-stream-monitor.h"

struct _GrdContext
{
  GObject parent;

  GMainContext *main_context;
  GrdStreamMonitor *stream_monitor;

  GrdDBusRemoteDesktop *proxy;
};

G_DEFINE_TYPE (GrdContext, grd_context, G_TYPE_OBJECT);

GrdDBusRemoteDesktop *
grd_context_get_dbus_proxy (GrdContext *context)
{
  return context->proxy;
}

void
grd_context_set_dbus_proxy (GrdContext           *context,
                            GrdDBusRemoteDesktop *proxy)
{
  context->proxy = proxy;
}

GrdStreamMonitor *
grd_context_get_stream_monitor (GrdContext *context)
{
  return context->stream_monitor;
}

GMainContext *
grd_context_get_main_context (GrdContext *context)
{
  return context->main_context;
}

static void
grd_context_constructed (GObject *object)
{
  GrdContext *context = GRD_CONTEXT (object);

  context->stream_monitor = grd_stream_monitor_new (context);
}

static void
grd_context_init (GrdContext *context)
{
  gst_init (NULL, NULL);
  pinos_init (NULL, NULL);

  context->main_context = g_main_context_default ();
}

static void
grd_context_class_init (GrdContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = grd_context_constructed;
}
