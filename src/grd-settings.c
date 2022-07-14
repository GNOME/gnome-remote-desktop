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

#include "grd-schemas.h"

#define GRD_RDP_SCHEMA_ID "org.gnome.desktop.remote-desktop.rdp"
#define GRD_VNC_SCHEMA_ID "org.gnome.desktop.remote-desktop.vnc"
#define GRD_RDP_SERVER_PORT 3389
#define GRD_VNC_SERVER_PORT 5900

enum
{
  RDP_ENABLED_CHANGED,
  RDP_SCREEN_SHARE_MODE_CHANGED,
  RDP_SERVER_CERT_CHANGED,
  RDP_SERVER_KEY_CHANGED,
  RDP_VIEW_ONLY_CHANGED,
  VNC_ENABLED_CHANGED,
  VNC_VIEW_ONLY_CHANGED,
  VNC_AUTH_METHOD_CHANGED,
  VNC_ENCRYPTION_CHANGED,
  VNC_SCREEN_SHARE_MODE_CHANGED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _GrdSettings
{
  GObject parent;

  struct {
    GSettings *settings;

    gboolean is_enabled;
    GrdRdpScreenShareMode screen_share_mode;
    char *server_cert;
    char *server_key;
    gboolean view_only;
    int port;
  } rdp;
  struct {
    GSettings *settings;
    gboolean is_enabled;
    gboolean view_only;
    GrdVncAuthMethod auth_method;
    int port;
    GrdVncScreenShareMode screen_share_mode;
  } vnc;
};

G_DEFINE_TYPE (GrdSettings, grd_settings, G_TYPE_OBJECT)

int
grd_settings_get_rdp_port (GrdSettings *settings)
{
  return settings->rdp.port;
}

int
grd_settings_get_vnc_port (GrdSettings *settings)
{
  return settings->vnc.port;
}

void
grd_settings_override_rdp_port (GrdSettings *settings,
                                int          port)
{
  settings->rdp.port = port;
}

void
grd_settings_override_vnc_port (GrdSettings *settings,
                                int          port)
{
  settings->vnc.port = port;
}

GrdRdpScreenShareMode
grd_settings_get_screen_share_mode (GrdSettings *settings)
{
  return settings->rdp.screen_share_mode;
}

GrdVncScreenShareMode
grd_settings_get_vnc_screen_share_mode (GrdSettings *settings)
{
  return settings->vnc.screen_share_mode;
}

char *
grd_settings_get_rdp_server_cert (GrdSettings *settings)
{
  return settings->rdp.server_cert;
}

char *
grd_settings_get_rdp_server_key (GrdSettings *settings)
{
  return settings->rdp.server_key;
}

char *
grd_settings_get_rdp_username (GrdSettings  *settings,
                               GError      **error)
{
  const char *test_username_override;
  g_autofree char *credentials_string = NULL;
  g_autoptr (GVariant) credentials = NULL;
  char *username = NULL;

  test_username_override = g_getenv ("GNOME_REMOTE_DESKTOP_TEST_RDP_USERNAME");
  if (test_username_override)
    return g_strdup (test_username_override);

  credentials_string = secret_password_lookup_sync (GRD_RDP_CREDENTIALS_SCHEMA,
                                                    NULL, error,
                                                    NULL);
  if (!credentials_string)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Credentials not set");
      return NULL;
    }

  credentials = g_variant_parse (NULL, credentials_string, NULL, NULL, NULL);
  if (!credentials)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Unable to parse credentials");
      return NULL;
    }

  g_variant_lookup (credentials, "username", "s", &username);
  if (!username)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Username not set");
      return NULL;
    }

  return username;
}

char *
grd_settings_get_rdp_password (GrdSettings  *settings,
                               GError      **error)
{
  const char *test_password_override;
  g_autofree char *credentials_string = NULL;
  g_autoptr (GVariant) credentials = NULL;
  char *password = NULL;

  test_password_override = g_getenv ("GNOME_REMOTE_DESKTOP_TEST_RDP_PASSWORD");
  if (test_password_override)
    return g_strdup (test_password_override);

  credentials_string = secret_password_lookup_sync (GRD_RDP_CREDENTIALS_SCHEMA,
                                                    NULL, error,
                                                    NULL);
  if (!credentials_string)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Credentials not set");
      return NULL;
    }

  credentials = g_variant_parse (NULL, credentials_string, NULL, NULL, NULL);
  if (!credentials)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Unable to parse credentials");
      return NULL;
    }

  g_variant_lookup (credentials, "password", "s", &password);
  if (!password)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Password not set");
      return NULL;
    }

  return password;
}

char *
grd_settings_get_vnc_password (GrdSettings  *settings,
                               GError      **error)
{
  const char *test_password_override;
  char *password;

  test_password_override = g_getenv ("GNOME_REMOTE_DESKTOP_TEST_VNC_PASSWORD");
  if (test_password_override)
    return g_strdup (test_password_override);

  password = secret_password_lookup_sync (GRD_VNC_PASSWORD_SCHEMA,
                                          NULL, error,
                                          NULL);
  if (!password)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Password not set");
      return NULL;
    }

  return password;
}

gboolean
grd_settings_get_rdp_view_only (GrdSettings *settings)
{
  return settings->rdp.view_only;
}

gboolean
grd_settings_get_vnc_view_only (GrdSettings *settings)
{
  return settings->vnc.view_only;
}

gboolean
grd_settings_is_rdp_enabled (GrdSettings *settings)
{
  return settings->rdp.is_enabled;
}

gboolean
grd_settings_is_vnc_enabled (GrdSettings *settings)
{
  return settings->vnc.is_enabled;
}

GrdVncAuthMethod
grd_settings_get_vnc_auth_method (GrdSettings *settings)
{
  if (g_getenv ("GNOME_REMOTE_DESKTOP_TEST_VNC_PASSWORD"))
    return GRD_VNC_AUTH_METHOD_PASSWORD;
  else
    return settings->vnc.auth_method;
}

static void
update_screen_share_mode (GrdSettings *settings)
{
  settings->rdp.screen_share_mode =
    g_settings_get_enum (settings->rdp.settings, "screen-share-mode");
}

static void
update_vnc_screen_share_mode (GrdSettings *settings)
{
  settings->vnc.screen_share_mode =
    g_settings_get_enum (settings->vnc.settings, "screen-share-mode");
}

static void
update_rdp_enabled (GrdSettings *settings)
{
  settings->rdp.is_enabled = g_settings_get_boolean (settings->rdp.settings,
                                                     "enable");
}

static void
update_rdp_tls_cert (GrdSettings *settings)
{
  g_clear_pointer (&settings->rdp.server_cert, g_free);
  settings->rdp.server_cert = g_settings_get_string (settings->rdp.settings,
                                                     "tls-cert");
}

static void
update_rdp_tls_key (GrdSettings *settings)
{
  g_clear_pointer (&settings->rdp.server_key, g_free);
  settings->rdp.server_key = g_settings_get_string (settings->rdp.settings,
                                                    "tls-key");
}

static void
update_rdp_view_only (GrdSettings *settings)
{
  settings->rdp.view_only = g_settings_get_boolean (settings->rdp.settings,
                                                    "view-only");
}

static void
update_vnc_enabled (GrdSettings *settings)
{
  settings->vnc.is_enabled = g_settings_get_boolean (settings->vnc.settings,
                                                     "enable");
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
on_rdp_settings_changed (GSettings   *rdp_settings,
                         const char  *key,
                         GrdSettings *settings)
{
  if (strcmp (key, "enable") == 0)
    {
      update_rdp_enabled (settings);
      g_signal_emit (settings, signals[RDP_ENABLED_CHANGED], 0);
    }
  else if (strcmp (key, "screen-share-mode") == 0)
    {
      update_screen_share_mode (settings);
      g_signal_emit (settings, signals[RDP_SCREEN_SHARE_MODE_CHANGED], 0);
    }
  else if (strcmp (key, "tls-cert") == 0)
    {
      update_rdp_tls_cert (settings);
      g_signal_emit (settings, signals[RDP_SERVER_CERT_CHANGED], 0);
    }
  else if (strcmp (key, "tls-key") == 0)
    {
      update_rdp_tls_key (settings);
      g_signal_emit (settings, signals[RDP_SERVER_KEY_CHANGED], 0);
    }
  else if (strcmp (key, "view-only") == 0)
    {
      update_rdp_view_only (settings);
      g_signal_emit (settings, signals[RDP_VIEW_ONLY_CHANGED], 0);
    }
}

static void
on_vnc_settings_changed (GSettings   *vnc_settings,
                         const char  *key,
                         GrdSettings *settings)
{
  if (strcmp (key, "enable") == 0)
    {
      update_vnc_enabled (settings);
      g_signal_emit (settings, signals[VNC_ENABLED_CHANGED], 0);
    }
  else if (strcmp (key, "screen-share-mode") == 0)
    {
      update_vnc_screen_share_mode (settings);
      g_signal_emit (settings, signals[VNC_SCREEN_SHARE_MODE_CHANGED], 0);
    }
  else if (strcmp (key, "view-only") == 0)
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

  g_clear_pointer (&settings->rdp.server_cert, g_free);
  g_clear_pointer (&settings->rdp.server_key, g_free);

  g_clear_object (&settings->rdp.settings);
  g_clear_object (&settings->vnc.settings);

  G_OBJECT_CLASS (grd_settings_parent_class)->finalize (object);
}

static void
grd_settings_init (GrdSettings *settings)
{
  settings->rdp.settings = g_settings_new (GRD_RDP_SCHEMA_ID);
  settings->vnc.settings = g_settings_new (GRD_VNC_SCHEMA_ID);
  g_signal_connect (settings->rdp.settings, "changed",
                    G_CALLBACK (on_rdp_settings_changed), settings);
  g_signal_connect (settings->vnc.settings, "changed",
                    G_CALLBACK (on_vnc_settings_changed), settings);

  update_rdp_enabled (settings);
  update_screen_share_mode (settings);
  update_rdp_tls_cert (settings);
  update_rdp_tls_key (settings);
  update_rdp_view_only (settings);
  update_vnc_enabled (settings);
  update_vnc_view_only (settings);
  update_vnc_auth_method (settings);
  update_vnc_screen_share_mode (settings);

  settings->rdp.port = GRD_RDP_SERVER_PORT;
  settings->vnc.port = GRD_VNC_SERVER_PORT;
}

static void
grd_settings_class_init (GrdSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_settings_finalize;

  signals[RDP_ENABLED_CHANGED] =
    g_signal_new ("rdp-enabled-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  signals[RDP_SCREEN_SHARE_MODE_CHANGED] =
    g_signal_new ("rdp-screen-share-mode-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  signals[RDP_SERVER_CERT_CHANGED] =
    g_signal_new ("rdp-tls-cert-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  signals[RDP_SERVER_KEY_CHANGED] =
    g_signal_new ("rdp-tls-key-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  signals[RDP_VIEW_ONLY_CHANGED] =
    g_signal_new ("rdp-view-only-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  signals[VNC_ENABLED_CHANGED] =
    g_signal_new ("vnc-enabled-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
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
  signals[VNC_SCREEN_SHARE_MODE_CHANGED] =
    g_signal_new ("vnc-screen-share-mode-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}
