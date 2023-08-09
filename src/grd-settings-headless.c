/*
 * Copyright (C) 2023 Red Hat Inc.
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

#include "grd-enums.h"
#include "grd-settings-headless.h"

#include <gio/gio.h>

#define GRD_HEADLESS_RDP_SERVER_PORT 3389
#define GRD_HEADLESS_VNC_SERVER_PORT 5900

struct _GrdSettingsHeadless
{
  GrdSettings parent;

  struct
  {
    GSettings *settings;
    char *server_cert;
    char *server_key;
  } rdp;
};

G_DEFINE_TYPE (GrdSettingsHeadless,
               grd_settings_headless,
               GRD_TYPE_SETTINGS)

static void
update_rdp_tls_cert (GrdSettingsHeadless *settings_headless)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *cert_file = NULL;

  cert_file = g_settings_get_string (settings_headless->rdp.settings,
                                     "tls-cert");

  g_clear_pointer (&settings_headless->rdp.server_cert, g_free);

  if (!g_file_get_contents (cert_file,
                            &settings_headless->rdp.server_cert,
                            NULL,
                            &error))
    {
      g_warning ("Error reading server certificate: %s", error->message);
    }
}

static void
update_rdp_tls_key (GrdSettingsHeadless *settings_headless)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *key_file = NULL;

  key_file = g_settings_get_string (settings_headless->rdp.settings,
                                    "tls-key");

  g_clear_pointer (&settings_headless->rdp.server_key, g_free);

  if (!g_file_get_contents (key_file,
                            &settings_headless->rdp.server_key,
                            NULL,
                            &error))
    {
      g_warning ("Error reading server key: %s", error->message);
    }
}

static gboolean
grd_settings_headless_is_rdp_enabled (GrdSettings *settings)
{
  return TRUE;
}

static gboolean
grd_settings_headless_is_vnc_enabled (GrdSettings *settings)
{
  return FALSE;
}

static gboolean
grd_settings_headless_get_rdp_view_only (GrdSettings *settings)
{
  return FALSE;
}

static gboolean
grd_settings_headless_get_vnc_view_only (GrdSettings *settings)
{
  return FALSE;
}

static GrdRdpScreenShareMode
grd_settings_headless_get_rdp_screen_share_mode (GrdSettings *settings)
{
  return GRD_RDP_SCREEN_SHARE_MODE_EXTEND;
}

static GrdVncScreenShareMode
grd_settings_headless_get_vnc_screen_share_mode (GrdSettings *settings)
{
  return GRD_VNC_SCREEN_SHARE_MODE_EXTEND;
}

static char *
grd_settings_headless_get_rdp_server_key (GrdSettings *settings)
{
  return GRD_SETTINGS_HEADLESS (settings)->rdp.server_key;
}

static char *
grd_settings_headless_get_rdp_server_cert (GrdSettings *settings)
{
  return GRD_SETTINGS_HEADLESS (settings)->rdp.server_cert;
}

static void
grd_settings_headless_override_rdp_server_key (GrdSettings *settings,
                                               const char  *key)
{
  GrdSettingsHeadless *settings_headless = GRD_SETTINGS_HEADLESS (settings);

  g_clear_pointer (&settings_headless->rdp.server_key, g_free);
  settings_headless->rdp.server_key = g_strdup (key);
}

static void
grd_settings_headless_override_rdp_server_cert (GrdSettings *settings,
                                                const char  *cert)
{
  GrdSettingsHeadless *settings_headless = GRD_SETTINGS_HEADLESS (settings);

  g_clear_pointer (&settings_headless->rdp.server_cert, g_free);
  settings_headless->rdp.server_cert = g_strdup (cert);
}

static GrdVncAuthMethod
grd_settings_headless_get_vnc_auth_method (GrdSettings *settings)
{
  return GRD_VNC_AUTH_METHOD_PASSWORD;
}

static void
on_rdp_settings_changed (GSettings           *rdp_settings,
                         const char          *key,
                         GrdSettingsHeadless *settings_headless)
{
  if (strcmp (key, "tls-cert") == 0)
    update_rdp_tls_cert (settings_headless);
  else if (strcmp (key, "tls-key") == 0)
    update_rdp_tls_key (settings_headless);
}

GrdSettingsHeadless *
grd_settings_headless_new (GrdContext *context,
                           gboolean    use_gsettings)
{
  GrdSettingsHeadless *settings_headless =
    g_object_new (GRD_TYPE_SETTINGS_HEADLESS,
                  "context", context,
                  "rdp-port", GRD_HEADLESS_RDP_SERVER_PORT,
                  "vnc-port", GRD_HEADLESS_VNC_SERVER_PORT,
                  NULL);

  if (use_gsettings)
    {
      settings_headless->rdp.settings = g_settings_new (GRD_RDP_SCHEMA_ID);

      g_signal_connect (settings_headless->rdp.settings,
                        "changed", G_CALLBACK (on_rdp_settings_changed),
                        settings_headless);

      update_rdp_tls_cert (settings_headless);
      update_rdp_tls_key (settings_headless);
    }

  return settings_headless;
}

static void
grd_settings_headless_finalize (GObject *object)
{
  GrdSettingsHeadless *settings_headless = GRD_SETTINGS_HEADLESS (object);

  g_clear_pointer (&settings_headless->rdp.server_cert, g_free);
  g_clear_pointer (&settings_headless->rdp.server_key, g_free);
  g_clear_object (&settings_headless->rdp.settings);

  G_OBJECT_CLASS (grd_settings_headless_parent_class)->finalize (object);
}

static void
grd_settings_headless_init (GrdSettingsHeadless *settings_headless)
{
}

static void
grd_settings_headless_class_init (GrdSettingsHeadlessClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdSettingsClass *settings_class = GRD_SETTINGS_CLASS (klass);

  object_class->finalize = grd_settings_headless_finalize;

  settings_class->is_rdp_enabled = grd_settings_headless_is_rdp_enabled;
  settings_class->is_vnc_enabled = grd_settings_headless_is_vnc_enabled;
  settings_class->get_rdp_view_only = grd_settings_headless_get_rdp_view_only;
  settings_class->get_vnc_view_only = grd_settings_headless_get_vnc_view_only;
  settings_class->get_rdp_screen_share_mode = grd_settings_headless_get_rdp_screen_share_mode;
  settings_class->get_vnc_screen_share_mode = grd_settings_headless_get_vnc_screen_share_mode;
  settings_class->get_rdp_server_key = grd_settings_headless_get_rdp_server_key;
  settings_class->get_rdp_server_cert = grd_settings_headless_get_rdp_server_cert;
  settings_class->override_rdp_server_key = grd_settings_headless_override_rdp_server_key;
  settings_class->override_rdp_server_cert = grd_settings_headless_override_rdp_server_cert;
  settings_class->get_vnc_auth_method = grd_settings_headless_get_vnc_auth_method;
}
