/*
 * Copyright (C) 2018 Red Hat Inc.
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
 */

#include "config.h"

#include "grd-settings-user.h"

#include <gio/gio.h>

#define GRD_RDP_SCHEMA_ID "org.gnome.desktop.remote-desktop.rdp"
#define GRD_VNC_SCHEMA_ID "org.gnome.desktop.remote-desktop.vnc"

struct _GrdSettingsUser
{
  GrdSettings parent;

  GSettings *rdp_settings;
  GSettings *vnc_settings;
};

G_DEFINE_TYPE (GrdSettingsUser,
               grd_settings_user,
               GRD_TYPE_SETTINGS)

GrdSettingsUser *
grd_settings_user_new (GrdRuntimeMode runtime_mode)
{
  return g_object_new (GRD_TYPE_SETTINGS_USER,
                       "runtime-mode", runtime_mode,
                       NULL);
}

static void
grd_settings_user_constructed (GObject *object)
{
  GrdSettingsUser *settings = GRD_SETTINGS_USER (object);

  g_settings_bind (settings->rdp_settings, "port",
                   settings, "rdp-port",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->rdp_settings, "negotiate-port",
                   settings, "rdp-negotiate-port",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->rdp_settings, "enable",
                   settings, "rdp-enabled",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->rdp_settings, "tls-cert",
                   settings, "rdp-server-cert-path",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->rdp_settings, "tls-key",
                   settings, "rdp-server-key-path",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->vnc_settings, "port",
                   settings, "vnc-port",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->vnc_settings, "negotiate-port",
                   settings, "vnc-negotiate-port",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->vnc_settings, "enable",
                   settings, "vnc-enabled",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->vnc_settings, "auth-method",
                   settings, "vnc-auth-method",
                   G_SETTINGS_BIND_DEFAULT);

  switch (grd_settings_get_runtime_mode (GRD_SETTINGS (settings)))
    {
    case GRD_RUNTIME_MODE_HEADLESS:
      g_object_set (settings,
                    "rdp-view-only", FALSE,
                    "rdp-screen-share-mode", GRD_RDP_SCREEN_SHARE_MODE_EXTEND,
                    "vnc-view-only", FALSE,
                    "vnc-screen-share-mode", GRD_RDP_SCREEN_SHARE_MODE_EXTEND,
                    NULL);
      break;
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
      g_settings_bind (settings->rdp_settings, "view-only",
                       settings, "rdp-view-only",
                       G_SETTINGS_BIND_DEFAULT);
      g_settings_bind (settings->rdp_settings, "screen-share-mode",
                       settings, "rdp-screen-share-mode",
                       G_SETTINGS_BIND_DEFAULT);
      g_settings_bind (settings->vnc_settings, "view-only",
                       settings, "vnc-view-only",
                       G_SETTINGS_BIND_DEFAULT);
      g_settings_bind (settings->vnc_settings, "screen-share-mode",
                       settings, "vnc-screen-share-mode",
                       G_SETTINGS_BIND_DEFAULT);
      break;
    }

  G_OBJECT_CLASS (grd_settings_user_parent_class)->constructed (object);
}

static void
grd_settings_user_finalize (GObject *object)
{
  GrdSettingsUser *settings = GRD_SETTINGS_USER (object);

  g_clear_object (&settings->rdp_settings);
  g_clear_object (&settings->vnc_settings);

  G_OBJECT_CLASS (grd_settings_user_parent_class)->finalize (object);
}

static void
grd_settings_user_init (GrdSettingsUser *settings)
{
  settings->rdp_settings = g_settings_new (GRD_RDP_SCHEMA_ID);
  settings->vnc_settings = g_settings_new (GRD_VNC_SCHEMA_ID);
}

static void
grd_settings_user_class_init (GrdSettingsUserClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = grd_settings_user_constructed;
  object_class->finalize = grd_settings_user_finalize;
}
