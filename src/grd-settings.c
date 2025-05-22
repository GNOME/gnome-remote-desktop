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

#ifdef HAVE_RDP
#include <freerdp/freerdp.h>
#endif

#include "grd-credentials-file.h"
#include "grd-credentials-libsecret.h"
#include "grd-credentials-one-time.h"
#include "grd-credentials-tpm.h"
#include "grd-enum-types.h"

#define GRD_DEFAULT_RDP_SERVER_PORT 3389
#define GRD_DEFAULT_VNC_SERVER_PORT 5900

enum
{
  PROP_0,

  PROP_RUNTIME_MODE,
  PROP_RDP_PORT,
  PROP_RDP_NEGOTIATE_PORT,
  PROP_VNC_PORT,
  PROP_VNC_NEGOTIATE_PORT,
  PROP_RDP_ENABLED,
  PROP_VNC_ENABLED,
  PROP_RDP_VIEW_ONLY,
  PROP_VNC_VIEW_ONLY,
  PROP_RDP_SCREEN_SHARE_MODE,
  PROP_VNC_SCREEN_SHARE_MODE,
  PROP_RDP_SERVER_CERT,
  PROP_RDP_SERVER_FINGERPRINT,
  PROP_RDP_SERVER_KEY,
  PROP_RDP_SERVER_CERT_PATH,
  PROP_RDP_SERVER_KEY_PATH,
  PROP_VNC_AUTH_METHOD,
};

typedef struct _GrdSettingsPrivate
{
  GrdRuntimeMode runtime_mode;
  GrdCredentials *credentials;

  struct {
    int port;
    gboolean negotiate_port;
    gboolean is_enabled;
    gboolean view_only;
    GrdRdpScreenShareMode screen_share_mode;
    char *server_cert;
    char *server_fingerprint;
    char *server_key;
    char *server_cert_path;
    char *server_key_path;
  } rdp;
  struct {
    int port;
    gboolean negotiate_port;
    gboolean is_enabled;
    gboolean view_only;
    GrdVncScreenShareMode screen_share_mode;
    GrdVncAuthMethod auth_method;
  } vnc;
} GrdSettingsPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdSettings, grd_settings, G_TYPE_OBJECT)

#ifdef HAVE_RDP
G_DEFINE_AUTOPTR_CLEANUP_FUNC (rdpCertificate, freerdp_certificate_free)
#endif

GrdRuntimeMode
grd_settings_get_runtime_mode (GrdSettings *settings)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  return priv->runtime_mode;
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

gboolean
grd_settings_set_rdp_credentials (GrdSettings  *settings,
                                  const char   *username,
                                  const char   *password,
                                  GError      **error)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);
  GVariantBuilder builder;

  g_assert (username);
  g_assert (password);

  if (!g_utf8_validate (username, -1, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Username is not a valid UTF-8 string");
      return FALSE;
    }
  if (!g_utf8_validate (password, -1, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Password is not a valid UTF-8 string");
      return FALSE;
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}",
                         "username", g_variant_new_string (username));
  g_variant_builder_add (&builder, "{sv}",
                         "password", g_variant_new_string (password));

  return grd_credentials_store (priv->credentials,
                                GRD_CREDENTIALS_TYPE_RDP,
                                g_variant_builder_end (&builder),
                                error);
}

gboolean
grd_settings_clear_rdp_credentials (GrdSettings  *settings,
                                    GError      **error)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  return grd_credentials_clear (priv->credentials,
                                GRD_CREDENTIALS_TYPE_RDP,
                                error);
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
grd_settings_set_vnc_password (GrdSettings  *settings,
                               const char   *password,
                               GError      **error)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  g_assert (password);

  if (!g_utf8_validate (password, -1, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Password is not a valid UTF-8 string");
      return FALSE;
    }

  return grd_credentials_store (priv->credentials,
                                GRD_CREDENTIALS_TYPE_VNC,
                                g_variant_new_string (password),
                                error);
}

gboolean
grd_settings_clear_vnc_password (GrdSettings  *settings,
                                 GError      **error)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  return grd_credentials_clear (priv->credentials,
                                GRD_CREDENTIALS_TYPE_VNC,
                                error);
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
    case GRD_RUNTIME_MODE_SYSTEM:
      return create_headless_credentials ();
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
      return GRD_CREDENTIALS (grd_credentials_libsecret_new ());
    case GRD_RUNTIME_MODE_HANDOVER:
      return GRD_CREDENTIALS (grd_credentials_one_time_new ());
    }

  g_assert_not_reached ();
}

void
grd_settings_recreate_rdp_credentials (GrdSettings *settings)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);

  g_clear_object (&priv->credentials);
  priv->credentials = create_credentials (priv->runtime_mode);
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

  g_clear_pointer (&priv->rdp.server_cert, g_free);
  g_clear_pointer (&priv->rdp.server_fingerprint, g_free);
  g_clear_pointer (&priv->rdp.server_key, g_free);
  g_clear_pointer (&priv->rdp.server_cert_path, g_free);
  g_clear_pointer (&priv->rdp.server_key_path, g_free);
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
    case PROP_RDP_NEGOTIATE_PORT:
      g_value_set_boolean (value, priv->rdp.negotiate_port);
      break;
    case PROP_VNC_PORT:
      g_value_set_int (value, priv->vnc.port);
      break;
    case PROP_VNC_NEGOTIATE_PORT:
      g_value_set_boolean (value, priv->vnc.negotiate_port);
      break;
    case PROP_RDP_ENABLED:
      g_value_set_boolean (value, priv->rdp.is_enabled);
      break;
    case PROP_VNC_ENABLED:
      g_value_set_boolean (value, priv->vnc.is_enabled);
      break;
    case PROP_RDP_VIEW_ONLY:
      g_value_set_boolean (value, priv->rdp.view_only);
      break;
    case PROP_VNC_VIEW_ONLY:
      g_value_set_boolean (value, priv->vnc.view_only);
      break;
    case PROP_RDP_SCREEN_SHARE_MODE:
      g_value_set_enum (value, priv->rdp.screen_share_mode);
      break;
    case PROP_VNC_SCREEN_SHARE_MODE:
      g_value_set_enum (value, priv->vnc.screen_share_mode);
      break;
    case PROP_RDP_SERVER_CERT:
      g_value_set_string (value, priv->rdp.server_cert);
      break;
    case PROP_RDP_SERVER_FINGERPRINT:
      g_value_set_string (value, priv->rdp.server_fingerprint);
      break;
    case PROP_RDP_SERVER_KEY:
      g_value_set_string (value, priv->rdp.server_key);
      break;
    case PROP_RDP_SERVER_CERT_PATH:
      g_value_set_string (value, priv->rdp.server_cert_path);
      break;
    case PROP_RDP_SERVER_KEY_PATH:
      g_value_set_string (value, priv->rdp.server_key_path);
      break;
    case PROP_VNC_AUTH_METHOD:
      if (g_getenv ("GNOME_REMOTE_DESKTOP_TEST_VNC_PASSWORD"))
        g_value_set_enum (value, GRD_VNC_AUTH_METHOD_PASSWORD);
      else
        g_value_set_enum (value, priv->vnc.auth_method);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
update_rdp_server_fingerprint (GrdSettings *settings)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);
#ifdef HAVE_RDP
  g_autoptr (rdpCertificate) rdp_certificate = NULL;
#endif
  g_autofree char *fingerprint = NULL;

  if (!priv->rdp.server_cert_path)
    return;

#ifdef HAVE_RDP
  rdp_certificate = freerdp_certificate_new_from_file (priv->rdp.server_cert_path);
  if (!rdp_certificate)
    {
      g_warning ("RDP server certificate is invalid");
      return;
    }

  fingerprint = freerdp_certificate_get_fingerprint (rdp_certificate);
  if (!fingerprint)
    {
      g_warning ("Could not compute RDP server certificate fingerprint");
      return;
    }
#endif

  if (fingerprint)
    g_object_set (G_OBJECT (settings), "rdp-server-fingerprint", fingerprint, NULL);
}

static void
update_rdp_server_cert (GrdSettings *settings)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);
  g_autofree char *server_cert = NULL;

  if (!priv->rdp.server_cert_path)
    return;

  g_file_get_contents (priv->rdp.server_cert_path,
                       &server_cert,
                       NULL, NULL);
  g_object_set (G_OBJECT (settings), "rdp-server-cert", server_cert, NULL);
}

static void
update_rdp_server_key (GrdSettings *settings)
{
  GrdSettingsPrivate *priv = grd_settings_get_instance_private (settings);
  g_autofree char *server_key = NULL;

  if (!priv->rdp.server_key_path)
    return;

  g_file_get_contents (priv->rdp.server_key_path,
                       &server_key,
                       NULL, NULL);
  g_object_set (G_OBJECT (settings), "rdp-server-key", server_key, NULL);
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
    case PROP_RDP_NEGOTIATE_PORT:
      priv->rdp.negotiate_port = g_value_get_boolean (value);
      break;
    case PROP_VNC_PORT:
      priv->vnc.port = g_value_get_int (value);
      break;
    case PROP_VNC_NEGOTIATE_PORT:
      priv->vnc.negotiate_port = g_value_get_boolean (value);
      break;
    case PROP_RDP_ENABLED:
      priv->rdp.is_enabled = g_value_get_boolean (value);
      break;
    case PROP_VNC_ENABLED:
      priv->vnc.is_enabled = g_value_get_boolean (value);
      break;
    case PROP_RDP_VIEW_ONLY:
      priv->rdp.view_only = g_value_get_boolean (value);
      break;
    case PROP_VNC_VIEW_ONLY:
      priv->vnc.view_only = g_value_get_boolean (value);
      break;
    case PROP_RDP_SCREEN_SHARE_MODE:
      priv->rdp.screen_share_mode = g_value_get_enum (value);
      break;
    case PROP_VNC_SCREEN_SHARE_MODE:
      priv->vnc.screen_share_mode = g_value_get_enum (value);
      break;
    case PROP_RDP_SERVER_CERT:
      g_clear_pointer (&priv->rdp.server_cert, g_free);
      priv->rdp.server_cert = g_strdup (g_value_get_string (value));
      break;
    case PROP_RDP_SERVER_FINGERPRINT:
      g_clear_pointer (&priv->rdp.server_fingerprint, g_free);
      priv->rdp.server_fingerprint = g_strdup (g_value_get_string (value));
      break;
    case PROP_RDP_SERVER_KEY:
      g_clear_pointer (&priv->rdp.server_key, g_free);
      priv->rdp.server_key = g_strdup (g_value_get_string (value));
      break;
    case PROP_RDP_SERVER_CERT_PATH:
      g_clear_pointer (&priv->rdp.server_cert_path, g_free);
      priv->rdp.server_cert_path = g_strdup (g_value_get_string (value));
      update_rdp_server_cert (settings);
      update_rdp_server_fingerprint (settings);
      break;
    case PROP_RDP_SERVER_KEY_PATH:
      g_clear_pointer (&priv->rdp.server_key_path, g_free);
      priv->rdp.server_key_path = g_strdup (g_value_get_string (value));
      update_rdp_server_key (settings);
      break;
    case PROP_VNC_AUTH_METHOD:
      priv->vnc.auth_method = g_value_get_enum (value);
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
                                                     G_PARAM_CONSTRUCT |
                                                     G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_RDP_NEGOTIATE_PORT,
                                   g_param_spec_boolean ("rdp-negotiate-port",
                                                         "rdp negotiate port",
                                                         "rdp negotiate port",
                                                         FALSE,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT |
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
                                                     G_PARAM_CONSTRUCT |
                                                     G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_VNC_NEGOTIATE_PORT,
                                   g_param_spec_boolean ("vnc-negotiate-port",
                                                         "vnc negotiate port",
                                                         "vnc negotiate port",
                                                         FALSE,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT |
                                                         G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_RDP_ENABLED,
                                   g_param_spec_boolean ("rdp-enabled",
                                                         "rdp enabled",
                                                         "rdp enabled",
                                                         FALSE,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT |
                                                         G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_VNC_ENABLED,
                                   g_param_spec_boolean ("vnc-enabled",
                                                         "vnc enabled",
                                                         "vnc enabled",
                                                         FALSE,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT |
                                                         G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_RDP_VIEW_ONLY,
                                   g_param_spec_boolean ("rdp-view-only",
                                                         "rdp view only",
                                                         "rdp view only",
                                                         FALSE,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT |
                                                         G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_VNC_VIEW_ONLY,
                                   g_param_spec_boolean ("vnc-view-only",
                                                         "vnc view only",
                                                         "vnc view only",
                                                         FALSE,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT |
                                                         G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_RDP_SCREEN_SHARE_MODE,
                                   g_param_spec_enum ("rdp-screen-share-mode",
                                                      "rdp screen share mode",
                                                      "rdp screen share mode",
                                                      GRD_TYPE_RDP_SCREEN_SHARE_MODE,
                                                      GRD_RDP_SCREEN_SHARE_MODE_MIRROR_PRIMARY,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT |
                                                      G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_VNC_SCREEN_SHARE_MODE,
                                   g_param_spec_enum ("vnc-screen-share-mode",
                                                      "vnc screen share mode",
                                                      "vnc screen share mode",
                                                      GRD_TYPE_VNC_SCREEN_SHARE_MODE,
                                                      GRD_VNC_SCREEN_SHARE_MODE_MIRROR_PRIMARY,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT |
                                                      G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_RDP_SERVER_CERT,
                                   g_param_spec_string ("rdp-server-cert",
                                                        "rdp server cert",
                                                        "rdp server cert",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_RDP_SERVER_FINGERPRINT,
                                   g_param_spec_string ("rdp-server-fingerprint",
                                                        "rdp server fingerprint",
                                                        "rdp server fingerprint",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_RDP_SERVER_KEY,
                                   g_param_spec_string ("rdp-server-key",
                                                        "rdp server key",
                                                        "rdp server key",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_RDP_SERVER_CERT_PATH,
                                   g_param_spec_string ("rdp-server-cert-path",
                                                        "rdp server cert path",
                                                        "rdp server cert path",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_RDP_SERVER_KEY_PATH,
                                   g_param_spec_string ("rdp-server-key-path",
                                                        "rdp server key path",
                                                        "rdp server key path",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_VNC_AUTH_METHOD,
                                   g_param_spec_enum ("vnc-auth-method",
                                                      "vnc auth method",
                                                      "vnc auth method",
                                                      GRD_TYPE_VNC_AUTH_METHOD,
                                                      GRD_VNC_AUTH_METHOD_PROMPT,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT |
                                                      G_PARAM_STATIC_STRINGS));
}
