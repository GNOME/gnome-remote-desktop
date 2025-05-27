/*
 * Copyright (C) 2025 Red Hat Inc.
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

#include "grd-settings-headless.h"

#include <gio/gio.h>

#define GRD_RDP_COMMON_SCHEMA_ID "org.gnome.desktop.remote-desktop.rdp"
#define GRD_RDP_SCHEMA_ID "org.gnome.desktop.remote-desktop.rdp.headless"
#define GRD_VNC_SCHEMA_ID "org.gnome.desktop.remote-desktop.vnc.headless"

struct _GrdSettingsHeadless
{
  GrdSettings parent;

  GSettings *rdp_settings;
  GSettings *common_rdp_settings;
  GSettings *vnc_settings;
};

G_DEFINE_TYPE (GrdSettingsHeadless,
               grd_settings_headless,
               GRD_TYPE_SETTINGS)

GrdSettingsHeadless *
grd_settings_headless_new (void)
{
  return g_object_new (GRD_TYPE_SETTINGS_HEADLESS,
                       "runtime-mode", GRD_RUNTIME_MODE_HEADLESS,
                       "rdp-screen-share-mode", GRD_RDP_SCREEN_SHARE_MODE_EXTEND,
                       "rdp-view-only", FALSE,
                       "vnc-screen-share-mode", GRD_RDP_SCREEN_SHARE_MODE_EXTEND,
                       "vnc-view-only", FALSE,
                       "vnc-auth-method", GRD_VNC_AUTH_METHOD_PASSWORD,
                       NULL);
}

static void
grd_settings_headless_constructed (GObject *object)
{
  GrdSettingsHeadless *settings = GRD_SETTINGS_HEADLESS (object);

  g_settings_bind (settings->rdp_settings, "enable",
                   settings, "rdp-enabled",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->rdp_settings, "port",
                   settings, "rdp-port",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->rdp_settings, "negotiate-port",
                   settings, "rdp-negotiate-port",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->common_rdp_settings, "tls-cert",
                   settings, "rdp-server-cert-path",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->common_rdp_settings, "tls-key",
                   settings, "rdp-server-key-path",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->vnc_settings, "enable",
                   settings, "vnc-enabled",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->vnc_settings, "port",
                   settings, "vnc-port",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (settings->vnc_settings, "negotiate-port",
                   settings, "vnc-negotiate-port",
                   G_SETTINGS_BIND_DEFAULT);

  G_OBJECT_CLASS (grd_settings_headless_parent_class)->constructed (object);
}

static void
grd_settings_headless_finalize (GObject *object)
{
  GrdSettingsHeadless *settings = GRD_SETTINGS_HEADLESS (object);

  g_clear_object (&settings->rdp_settings);
  g_clear_object (&settings->common_rdp_settings);
  g_clear_object (&settings->vnc_settings);

  G_OBJECT_CLASS (grd_settings_headless_parent_class)->finalize (object);
}

static void
grd_settings_headless_init (GrdSettingsHeadless *settings)
{
  settings->rdp_settings = g_settings_new (GRD_RDP_SCHEMA_ID);
  settings->common_rdp_settings = g_settings_new (GRD_RDP_COMMON_SCHEMA_ID);
  settings->vnc_settings = g_settings_new (GRD_VNC_SCHEMA_ID);
}

static void
grd_settings_headless_class_init (GrdSettingsHeadlessClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = grd_settings_headless_constructed;
  object_class->finalize = grd_settings_headless_finalize;
}
