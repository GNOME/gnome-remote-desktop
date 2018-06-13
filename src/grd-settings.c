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

#include "grd-settings.h"

#include <gio/gio.h>
#include <string.h>

#define GRD_VNC_SCHEMA_ID "org.gnome.desktop.remote-desktop.vnc"

enum
{
  VNC_VIEW_ONLY_CHANGED,
  VNC_AUTH_METHOD_CHANGED,
  VNC_ENCRYPTION_CHANGED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _GrdSettings
{
  GObject parent;

  struct {
    GSettings *settings;
    gboolean view_only;
    GrdVncAuthMethod auth_method;
  } vnc;
};

G_DEFINE_TYPE (GrdSettings, grd_settings, G_TYPE_OBJECT)

const SecretSchema *
grd_vnc_password_get_schema (void)
{
  static const SecretSchema grd_vnc_password_schema = {
    .name = "org.gnome.RemoteDesktop.VncPassword",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {
      { "password", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { "NULL", 0 },
    },
  };

  return &grd_vnc_password_schema;
}

char *
grd_settings_get_vnc_password (GrdSettings  *settings,
                               GError      **error)
{
  return secret_password_lookup_sync (GRD_VNC_PASSWORD_SCHEMA,
                                      NULL, error,
                                      NULL);
}

gboolean
grd_settings_get_vnc_view_only (GrdSettings *settings)
{
  return settings->vnc.view_only;
}

GrdVncAuthMethod
grd_settings_get_vnc_auth_method (GrdSettings *settings)
{
  return settings->vnc.auth_method;
}

static void
update_vnc_view_only (GrdSettings *settings)
{
  settings->vnc.view_only = g_settings_get_boolean (settings->vnc.settings,
                                                    "view-only");
}

static void
update_vnc_auth_method (GrdSettings *settings)
{
  settings->vnc.auth_method = g_settings_get_enum (settings->vnc.settings,
                                                   "auth-method");
}

static void
on_vnc_settings_changed (GSettings   *vnc_settings,
                         const char  *key,
                         GrdSettings *settings)
{
  if (strcmp (key, "view-only") == 0)
    {
      update_vnc_view_only (settings);
      g_signal_emit (settings, signals[VNC_VIEW_ONLY_CHANGED], 0);
    }
  else if (strcmp (key, "auth-method") == 0)
    {
      update_vnc_auth_method (settings);
      g_signal_emit (settings, signals[VNC_AUTH_METHOD_CHANGED], 0);
    }
}

static void
grd_settings_finalize (GObject *object)
{
  GrdSettings *settings = GRD_SETTINGS (object);

  g_clear_object (&settings->vnc.settings);

  G_OBJECT_CLASS (grd_settings_parent_class)->finalize (object);
}

static void
grd_settings_init (GrdSettings *settings)
{
  settings->vnc.settings = g_settings_new (GRD_VNC_SCHEMA_ID);
  g_signal_connect (settings->vnc.settings, "changed",
                    G_CALLBACK (on_vnc_settings_changed), settings);

  update_vnc_view_only (settings);
  update_vnc_auth_method (settings);
}

static void
grd_settings_class_init (GrdSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_settings_finalize;

  signals[VNC_VIEW_ONLY_CHANGED] =
    g_signal_new ("vnc-view-only-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  signals[VNC_AUTH_METHOD_CHANGED] =
    g_signal_new ("vnc-auth-method-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}
