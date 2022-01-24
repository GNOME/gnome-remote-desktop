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

#include "grd-egl-thread.h"

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

  GrdEglThread *egl_thread;

  GrdSettings *settings;

  GrdDebugFlags debug_flags;
};

G_DEFINE_TYPE (GrdContext, grd_context, G_TYPE_OBJECT)

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
  g_clear_object (&context->remote_desktop_proxy);
  context->remote_desktop_proxy = proxy;
}

void
grd_context_set_screen_cast_proxy (GrdContext        *context,
                                   GrdDBusScreenCast *proxy)
{
  g_clear_object (&context->screen_cast_proxy);
  context->screen_cast_proxy = proxy;
}

GMainContext *
grd_context_get_main_context (GrdContext *context)
{
  return context->main_context;
}

GrdSettings *
grd_context_get_settings (GrdContext *context)
{
  return context->settings;
}

GrdEglThread *
grd_context_get_egl_thread (GrdContext *context)
{
  return context->egl_thread;
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
grd_context_finalize (GObject *object)
{
  GrdContext *context = GRD_CONTEXT (object);

  g_clear_object (&context->remote_desktop_proxy);
  g_clear_object (&context->screen_cast_proxy);
  g_clear_pointer (&context->egl_thread, grd_egl_thread_free);
  g_clear_object (&context->settings);

  G_OBJECT_CLASS (grd_context_parent_class)->finalize (object);
}

static void
grd_context_init (GrdContext *context)
{
  g_autoptr (GError) error = NULL;

  context->main_context = g_main_context_default ();

  init_debug_flags (context);

  context->settings = g_object_new (GRD_TYPE_SETTINGS, NULL);

  context->egl_thread = grd_egl_thread_new (&error);
  if (!context->egl_thread)
    g_debug ("Failed to create EGL thread: %s", error->message);
}

static void
grd_context_class_init (GrdContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_context_finalize;
}
