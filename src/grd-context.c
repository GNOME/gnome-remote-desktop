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

#include "grd-dbus-remote-desktop.h"
#include "grd-dbus-screen-cast.h"

static const GDebugKey grd_debug_keys[] = {
  { "vnc", GRD_DEBUG_VNC },
};

struct _GrdContext
{
  GObject parent;

  GMainContext *main_context;

  GrdDBusRemoteDesktop *remote_desktop_proxy;
  GrdDBusScreenCast *screen_cast_proxy;

  GList *sessions;

  GrdDebugFlags debug_flags;
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

GrdDebugFlags
grd_context_get_debug_flags (GrdContext *context)
{
  return context->debug_flags;
}

static void
init_debug_flags (GrdContext *context)
{
  const char *debug_env;

  debug_env = g_getenv ("GNOME_REMOTE_DESKTOP_DEBUG");
  if (debug_env)
    {
      context->debug_flags =
        g_parse_debug_string (debug_env,
                              grd_debug_keys,
                              G_N_ELEMENTS (grd_debug_keys));
    }
}

static void
grd_context_init (GrdContext *context)
{
  context->main_context = g_main_context_default ();

  init_debug_flags (context);
}

static void
grd_context_class_init (GrdContextClass *klass)
{
}
