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

#include "grd-context.h"
#include "grd-credentials-file.h"
#include "grd-credentials-libsecret.h"
#include "grd-credentials-tpm.h"
#include "grd-enum-types.h"

#define GRD_DEFAULT_RDP_SERVER_PORT 3389
#define GRD_DEFAULT_VNC_SERVER_PORT 5900

enum
{
  PROP_0,

  PROP_RUNTIME_MODE,
  PROP_RDP_PORT,
  PROP_VNC_PORT,
};

typedef struct _GrdSettingsPrivate
{
  GrdRuntimeMode runtime_mode;
  GrdCredentials *credentials;

  struct {
    int port;
  } rdp;
  struct {
    int port;
  } vnc;
} GrdSettingsPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdSettings, grd_settings, G_TYPE_OBJECT)

int
grd_settings_get_rdp_port (GrdSettings *settings)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  return priv->rdp.port;
}

int
grd_settings_get_vnc_port (GrdSettings *settings)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  return priv->vnc.port;
}

void
grd_settings_override_rdp_port (GrdSettings *settings,
                                int          port)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  priv->rdp.port = port;
}

void
grd_settings_override_vnc_port (GrdSettings *settings,
                                int          port)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  priv->vnc.port = port;
}

gboolean
grd_settings_get_rdp_credentials (GrdSettings  *settings,
                                  char        **out_username,
                                  char        **out_password,
                                  GError      **error)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);
  const char *test_username_override;
  const char *test_password_override;
  g_autofree char *credentials_string = NULL;
  g_autoptr (GVariant) credentials = NULL;
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;

  test_username_override = g_getenv ("GNOME_REMOTE_DESKTOP_TEST_RDP_USERNAME");
  test_password_override = g_getenv ("GNOME_REMOTE_DESKTOP_TEST_RDP_PASSWORD");

  if (test_username_override && test_password_override)
    {
      *out_username = g_strdup (test_username_override);
      *out_password = g_strdup (test_password_override);
      return TRUE;
    }

  credentials = grd_credentials_lookup (priv->credentials,
                                        GRD_CREDENTIALS_TYPE_RDP,
                                        error);
  if (!credentials)
    return FALSE;

  if (!test_username_override)
    {
      g_variant_lookup (credentials, "username", "s", &username);
      if (!username)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Username not set");
          return FALSE;
        }
    }
  else
    {
      username = g_strdup (test_username_override);
    }

  if (!test_password_override)
    {
      g_variant_lookup (credentials, "password", "s", &password);
      if (!password)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Username not set");
          return FALSE;
        }
    }
  else
    {
      password = g_strdup (test_password_override);
    }

  *out_username = g_steal_pointer (&username);
  *out_password = g_steal_pointer (&password);
  return TRUE;
}

char *
grd_settings_get_vnc_password (GrdSettings  *settings,
                               GError      **error)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);
  const char *test_password_override;
  g_autoptr (GVariant) password = NULL;

  test_password_override = g_getenv ("GNOME_REMOTE_DESKTOP_TEST_VNC_PASSWORD");
  if (test_password_override)
    return g_strdup (test_password_override);

  password = grd_credentials_lookup (priv->credentials,
                                     GRD_CREDENTIALS_TYPE_VNC,
                                     error);
  if (!password)
    return NULL;

  return g_variant_dup_string (password, NULL);
}

gboolean
grd_settings_is_rdp_enabled (GrdSettings *settings)
{
  return GRD_SETTINGS_GET_CLASS (settings)->is_rdp_enabled (settings);
}

gboolean
grd_settings_is_vnc_enabled (GrdSettings *settings)
{
  return GRD_SETTINGS_GET_CLASS (settings)->is_vnc_enabled (settings);
}

gboolean
grd_settings_get_rdp_view_only (GrdSettings *settings)
{
  return GRD_SETTINGS_GET_CLASS (settings)->get_rdp_view_only (settings);
}

gboolean
grd_settings_get_vnc_view_only (GrdSettings *settings)
{
  return GRD_SETTINGS_GET_CLASS (settings)->get_vnc_view_only (settings);
}

GrdRdpScreenShareMode
grd_settings_get_rdp_screen_share_mode (GrdSettings *settings)
{
  return GRD_SETTINGS_GET_CLASS (settings)->get_rdp_screen_share_mode (settings);
}

GrdVncScreenShareMode
grd_settings_get_vnc_screen_share_mode (GrdSettings *settings)
{
  return GRD_SETTINGS_GET_CLASS (settings)->get_vnc_screen_share_mode (settings);
}

char *
grd_settings_get_rdp_server_cert (GrdSettings *settings)
{
  return GRD_SETTINGS_GET_CLASS (settings)->get_rdp_server_cert (settings);
}

char *
grd_settings_get_rdp_server_key (GrdSettings *settings)
{
  return GRD_SETTINGS_GET_CLASS (settings)->get_rdp_server_key (settings);
}

GrdVncAuthMethod
grd_settings_get_vnc_auth_method (GrdSettings *settings)
{
  if (g_getenv ("GNOME_REMOTE_DESKTOP_TEST_VNC_PASSWORD"))
    return GRD_VNC_AUTH_METHOD_PASSWORD;
  else
    return GRD_SETTINGS_GET_CLASS (settings)->get_vnc_auth_method (settings);
}

static GrdCredentials *
create_headless_credentials (void)
{
  g_autoptr (GrdCredentials) credentials = NULL;
  g_autoptr (GError) error = NULL;

  credentials = GRD_CREDENTIALS (grd_credentials_tpm_new (&error));
  if (credentials)
    return g_steal_pointer (&credentials);

  g_warning ("Init TPM credentials failed because %s, using GKeyFile as fallback",
             error->message);

  g_clear_error (&error);
  credentials = GRD_CREDENTIALS (grd_credentials_file_new (&error));
  if (credentials)
    return g_steal_pointer (&credentials);

  g_warning ("Init file credentials failed: %s", error->message);

  return NULL;
}

static GrdCredentials *
create_credentials (GrdRuntimeMode runtime_mode)
{
  switch (runtime_mode)
    {
    case GRD_RUNTIME_MODE_HEADLESS:
       return create_headless_credentials ();
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
      return GRD_CREDENTIALS (grd_credentials_libsecret_new ());
    }

  g_assert_not_reached ();
}

static void
grd_settings_constructed (GObject *object)
{
  GrdSettings *settings = GRD_SETTINGS (object);
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  priv->credentials = create_credentials (priv->runtime_mode);
  g_assert (priv->credentials);
}

static void
grd_settings_finalize (GObject *object)
{
  GrdSettings *settings = GRD_SETTINGS (object);
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  g_clear_object (&priv->credentials);

  G_OBJECT_CLASS (grd_settings_parent_class)->finalize (object);
}

static void
grd_settings_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  GrdSettings *settings = GRD_SETTINGS (object);
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  switch (prop_id)
    {
    case PROP_RUNTIME_MODE:
      g_value_set_enum (value, priv->runtime_mode);
      break;
    case PROP_RDP_PORT:
      g_value_set_int (value, priv->rdp.port);
      break;
    case PROP_VNC_PORT:
      g_value_set_int (value, priv->vnc.port);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_settings_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  GrdSettings *settings = GRD_SETTINGS (object);
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  switch (prop_id)
    {
    case PROP_RUNTIME_MODE:
      priv->runtime_mode = g_value_get_enum (value);
      break;
    case PROP_RDP_PORT:
      priv->rdp.port = g_value_get_int (value);
      break;
    case PROP_VNC_PORT:
      priv->vnc.port = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_settings_init (GrdSettings *settings)
{
}

static void
grd_settings_class_init (GrdSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = grd_settings_constructed;
  object_class->finalize = grd_settings_finalize;
  object_class->get_property = grd_settings_get_property;
  object_class->set_property = grd_settings_set_property;

  g_object_class_install_property (object_class,
                                   PROP_RUNTIME_MODE,
                                   g_param_spec_enum ("runtime-mode",
                                                      "runtime mode",
                                                      "runtime mode",
                                                      GRD_TYPE_RUNTIME_MODE,
                                                      GRD_RUNTIME_MODE_SCREEN_SHARE,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT |
                                                      G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_RDP_PORT,
                                   g_param_spec_int ("rdp-port",
                                                     "rdp port",
                                                     "rdp port",
                                                     0,
                                                     G_MAXUINT16,
                                                     GRD_DEFAULT_RDP_SERVER_PORT,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY |
                                                     G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_VNC_PORT,
                                   g_param_spec_int ("vnc-port",
                                                     "vnc port",
                                                     "vnc port",
                                                     0,
                                                     G_MAXUINT16,
                                                     GRD_DEFAULT_VNC_SERVER_PORT,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY |
                                                     G_PARAM_STATIC_STRINGS));
}
