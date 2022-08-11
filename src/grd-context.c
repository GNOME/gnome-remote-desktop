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

#include "grd-credentials-file.h"
#include "grd-credentials-libsecret.h"
#include "grd-credentials-tpm.h"
#include "grd-egl-thread.h"
#include "grd-settings.h"

#include "grd-dbus-mutter-remote-desktop.h"
#include "grd-dbus-mutter-screen-cast.h"

struct _GrdContext
{
  GObject parent;

  GrdDBusMutterRemoteDesktop *mutter_remote_desktop_proxy;
  GrdDBusMutterScreenCast *mutter_screen_cast_proxy;

  GrdEglThread *egl_thread;

  GrdCredentials *credentials;
  GrdSettings *settings;
};

G_DEFINE_TYPE (GrdContext, grd_context, G_TYPE_OBJECT)

GrdDBusMutterRemoteDesktop *
grd_context_get_mutter_remote_desktop_proxy (GrdContext *context)
{
  return context->mutter_remote_desktop_proxy;
}

GrdDBusMutterScreenCast *
grd_context_get_mutter_screen_cast_proxy (GrdContext *context)
{
  return context->mutter_screen_cast_proxy;
}

void
grd_context_set_mutter_remote_desktop_proxy (GrdContext                 *context,
                                             GrdDBusMutterRemoteDesktop *proxy)
{
  g_clear_object (&context->mutter_remote_desktop_proxy);
  context->mutter_remote_desktop_proxy = proxy;
}

void
grd_context_set_mutter_screen_cast_proxy (GrdContext              *context,
                                          GrdDBusMutterScreenCast *proxy)
{
  g_clear_object (&context->mutter_screen_cast_proxy);
  context->mutter_screen_cast_proxy = proxy;
}

GrdSettings *
grd_context_get_settings (GrdContext *context)
{
  return context->settings;
}

GrdCredentials *
grd_context_get_credentials (GrdContext *context)
{
  return context->credentials;
}

GrdEglThread *
grd_context_get_egl_thread (GrdContext *context)
{
  return context->egl_thread;
}

void
grd_context_notify_daemon_ready (GrdContext *context)
{
  g_autoptr (GError) error = NULL;

  if (context->egl_thread)
    return;

  context->egl_thread = grd_egl_thread_new (&error);
  if (!context->egl_thread)
    g_debug ("Failed to create EGL thread: %s", error->message);
}

GrdContext *
grd_context_new (GrdRuntimeMode   runtime_mode,
                 GError         **error)
{
  g_autoptr (GrdContext) context = NULL;
  g_autoptr (GError) local_error = NULL;

  context = g_object_new (GRD_TYPE_CONTEXT, NULL);

  switch (runtime_mode)
    {
    case GRD_RUNTIME_MODE_HEADLESS:
      context->credentials = GRD_CREDENTIALS (grd_credentials_tpm_new (&local_error));
      if (!context->credentials)
        {
          g_warning ("Init TPM credentials failed because %s, using GKeyFile as fallback",
                     local_error->message);
          context->credentials =
            GRD_CREDENTIALS (grd_credentials_file_new (error));
        }
      break;
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
      context->credentials = GRD_CREDENTIALS (grd_credentials_libsecret_new ());
      break;
    }

  if (!context->credentials)
    return NULL;

  context->settings = grd_settings_new (context);

  return g_steal_pointer (&context);
}

static void
grd_context_finalize (GObject *object)
{
  GrdContext *context = GRD_CONTEXT (object);

  g_clear_object (&context->mutter_remote_desktop_proxy);
  g_clear_object (&context->mutter_screen_cast_proxy);
  g_clear_pointer (&context->egl_thread, grd_egl_thread_free);
  g_clear_object (&context->settings);
  g_clear_object (&context->credentials);

  G_OBJECT_CLASS (grd_context_parent_class)->finalize (object);
}

static void
grd_context_init (GrdContext *context)
{
}

static void
grd_context_class_init (GrdContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_context_finalize;
}
