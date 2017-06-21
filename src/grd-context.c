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
#include <pipewire/client/pipewire.h>

#include "grd-dbus-remote-desktop.h"
#include "grd-dbus-screen-cast.h"
#include "grd-pipewire-stream-monitor.h"

struct _GrdContext
{
  GObject parent;

  GMainContext *main_context;
  GrdPipeWireStreamMonitor *pipewire_stream_monitor;

  GrdDBusRemoteDesktop *remote_desktop_proxy;
  GrdDBusScreenCast *screen_cast_proxy;

  GList *sessions;
};

G_DEFINE_TYPE (GrdContext, grd_context, G_TYPE_OBJECT);

GrdDBusRemoteDesktop *
grd_context_get_remote_desktop_proxy (GrdContext *context)
{
  return context->remote_desktop_proxy;
}

GrdDBusScreenCast *
grd_context_get_screen_cast_proxy (GrdContext *context)
{
  return context->screen_cast_proxy;
}

void
grd_context_set_remote_desktop_proxy (GrdContext           *context,
                                      GrdDBusRemoteDesktop *proxy)
{
  context->remote_desktop_proxy = proxy;
}

void
grd_context_set_screen_cast_proxy (GrdContext        *context,
                                   GrdDBusScreenCast *proxy)
{
  context->screen_cast_proxy = proxy;
}

GrdPipeWireStreamMonitor *
grd_context_get_pipewire_stream_monitor (GrdContext *context)
{
  return context->pipewire_stream_monitor;
}

GMainContext *
grd_context_get_main_context (GrdContext *context)
{
  return context->main_context;
}

static void
on_session_stopped (GrdSession *session,
                    GrdContext *context)
{
  context->sessions = g_list_remove (context->sessions, session);
}

void
grd_context_add_session (GrdContext *context,
                         GrdSession *session)
{
  context->sessions = g_list_append (context->sessions, session);
  g_signal_connect (session, "stopped",
                    G_CALLBACK (on_session_stopped), context);
}

GList *
grd_context_get_sessions (GrdContext *context)
{
  return context->sessions;
}

static void
grd_context_constructed (GObject *object)
{
  GrdContext *context = GRD_CONTEXT (object);

  context->pipewire_stream_monitor = grd_pipewire_stream_monitor_new (context);
}

static void
grd_context_init (GrdContext *context)
{
  gst_init (NULL, NULL);
  pw_init (NULL, NULL);

  context->main_context = g_main_context_default ();
}

static void
grd_context_class_init (GrdContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = grd_context_constructed;
}
