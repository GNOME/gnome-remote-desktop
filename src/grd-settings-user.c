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

#define GRD_USER_RDP_SERVER_PORT 3390
#define GRD_USER_VNC_SERVER_PORT 5901

enum
{
  RDP_ENABLED_CHANGED,
  RDP_VIEW_ONLY_CHANGED,
  RDP_SCREEN_SHARE_MODE_CHANGED,
  RDP_SERVER_CERT_CHANGED,
  RDP_SERVER_KEY_CHANGED,
  VNC_ENABLED_CHANGED,
  VNC_VIEW_ONLY_CHANGED,
  VNC_SCREEN_SHARE_MODE_CHANGED,
  VNC_AUTH_METHOD_CHANGED,
  VNC_ENCRYPTION_CHANGED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _GrdSettingsUser
{
  GrdSettings parent;

  struct
  {
    GSettings *settings;
    gboolean is_enabled;
    gboolean view_only;
    GrdRdpScreenShareMode screen_share_mode;
    char *server_cert;
    char *server_key;
  } rdp;

  struct
  {
    GSettings *settings;
    gboolean is_enabled;
    gboolean view_only;
    GrdVncScreenShareMode screen_share_mode;
    GrdVncAuthMethod auth_method;
  } vnc;
};

G_DEFINE_TYPE (GrdSettingsUser,
               grd_settings_user,
               GRD_TYPE_SETTINGS)

static void
update_rdp_enabled (GrdSettingsUser *settings)
{
  settings->rdp.is_enabled = g_settings_get_boolean (settings->rdp.settings,
                                                     "enable");
}

static void
update_rdp_view_only (GrdSettingsUser *settings)
{
  settings->rdp.view_only = g_settings_get_boolean (settings->rdp.settings,
                                                    "view-only");
}

static void
update_rdp_screen_share_mode (GrdSettingsUser *settings)
{
  settings->rdp.screen_share_mode =
    g_settings_get_enum (settings->rdp.settings, "screen-share-mode");
}

static void
update_rdp_tls_cert (GrdSettingsUser *settings)
{
  g_clear_pointer (&settings->rdp.server_cert, g_free);
  settings->rdp.server_cert = g_settings_get_string (settings->rdp.settings,
                                                     "tls-cert");
}

static void
update_rdp_tls_key (GrdSettingsUser *settings)
{
  g_clear_pointer (&settings->rdp.server_key, g_free);
  settings->rdp.server_key = g_settings_get_string (settings->rdp.settings,
                                                    "tls-key");
}

static void
update_vnc_enabled (GrdSettingsUser *settings)
{
  settings->vnc.is_enabled = g_settings_get_boolean (settings->vnc.settings,
                                                     "enable");
}

static void
update_vnc_view_only (GrdSettingsUser *settings)
{
  settings->vnc.view_only = g_settings_get_boolean (settings->vnc.settings,
                                                    "view-only");
}

static void
update_vnc_screen_share_mode (GrdSettingsUser *settings)
{
  settings->vnc.screen_share_mode =
    g_settings_get_enum (settings->vnc.settings, "screen-share-mode");
}

static void
update_vnc_auth_method (GrdSettingsUser *settings)
{
  settings->vnc.auth_method = g_settings_get_enum (settings->vnc.settings,
                                                   "auth-method");
}

static gboolean
grd_settings_user_is_rdp_enabled (GrdSettings *settings)
{
  return GRD_SETTINGS_USER (settings)->rdp.is_enabled;
}

static gboolean
grd_settings_user_is_vnc_enabled (GrdSettings *settings)
{
  return GRD_SETTINGS_USER (settings)->vnc.is_enabled;
}

static gboolean
grd_settings_user_get_rdp_view_only (GrdSettings *settings)
{
  return GRD_SETTINGS_USER (settings)->rdp.view_only;
}

static gboolean
grd_settings_user_get_vnc_view_only (GrdSettings *settings)
{
  return GRD_SETTINGS_USER (settings)->vnc.view_only;
}

static GrdRdpScreenShareMode
grd_settings_user_get_rdp_screen_share_mode (GrdSettings *settings)
{
  return GRD_SETTINGS_USER (settings)->rdp.screen_share_mode;
}

static GrdVncScreenShareMode
grd_settings_user_get_vnc_screen_share_mode (GrdSettings *settings)
{
  return GRD_SETTINGS_USER (settings)->vnc.screen_share_mode;
}

static char *
grd_settings_user_get_rdp_server_key (GrdSettings *settings)
{
  return GRD_SETTINGS_USER (settings)->rdp.server_key;
}

static char *
grd_settings_user_get_rdp_server_cert (GrdSettings *settings)
{
  return GRD_SETTINGS_USER (settings)->rdp.server_cert;
}

static GrdVncAuthMethod
grd_settings_user_get_vnc_auth_method (GrdSettings *settings)
{
  return GRD_SETTINGS_USER (settings)->vnc.auth_method;
}

GrdSettingsUser *
grd_settings_user_new (GrdContext *context)
{
  return g_object_new (GRD_TYPE_SETTINGS_USER,
                       "context", context,
                       "rdp-port", GRD_USER_RDP_SERVER_PORT,
                       "vnc-port", GRD_USER_VNC_SERVER_PORT,
                       NULL);
}

static void
grd_settings_user_finalize (GObject *object)
{
  GrdSettingsUser *settings = GRD_SETTINGS_USER (object);

  g_clear_pointer (&settings->rdp.server_cert, g_free);
  g_clear_pointer (&settings->rdp.server_key, g_free);

  g_clear_object (&settings->rdp.settings);
  g_clear_object (&settings->vnc.settings);

  G_OBJECT_CLASS (grd_settings_user_parent_class)->finalize (object);
}

static void
on_rdp_settings_changed (GSettings       *rdp_settings,
                         const char      *key,
                         GrdSettingsUser *settings)
{
  if (strcmp (key, "enable") == 0)
    {
      update_rdp_enabled (settings);
      g_signal_emit (settings, signals[RDP_ENABLED_CHANGED], 0);
    }
  else if (strcmp (key, "view-only") == 0)
    {
      update_rdp_view_only (settings);
      g_signal_emit (settings, signals[RDP_VIEW_ONLY_CHANGED], 0);
    }
  else if (strcmp (key, "screen-share-mode") == 0)
    {
      update_rdp_screen_share_mode (settings);
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
}

static void
on_vnc_settings_changed (GSettings       *vnc_settings,
                         const char      *key,
                         GrdSettingsUser *settings)
{
  if (strcmp (key, "enable") == 0)
    {
      update_vnc_enabled (settings);
      g_signal_emit (settings, signals[VNC_ENABLED_CHANGED], 0);
    }
  else if (strcmp (key, "view-only") == 0)
    {
      update_vnc_view_only (settings);
      g_signal_emit (settings, signals[VNC_VIEW_ONLY_CHANGED], 0);
    }
  else if (strcmp (key, "screen-share-mode") == 0)
    {
      update_vnc_screen_share_mode (settings);
      g_signal_emit (settings, signals[VNC_SCREEN_SHARE_MODE_CHANGED], 0);
    }
  else if (strcmp (key, "auth-method") == 0)
    {
      update_vnc_auth_method (settings);
      g_signal_emit (settings, signals[VNC_AUTH_METHOD_CHANGED], 0);
    }
}

static void
grd_settings_user_init (GrdSettingsUser *settings)
{
  settings->rdp.settings = g_settings_new (GRD_RDP_SCHEMA_ID);
  settings->vnc.settings = g_settings_new (GRD_VNC_SCHEMA_ID);
  g_signal_connect (settings->rdp.settings, "changed",
                    G_CALLBACK (on_rdp_settings_changed), settings);
  g_signal_connect (settings->vnc.settings, "changed",
                    G_CALLBACK (on_vnc_settings_changed), settings);

  update_rdp_enabled (settings);
  update_rdp_view_only (settings);
  update_rdp_screen_share_mode (settings);
  update_rdp_tls_cert (settings);
  update_rdp_tls_key (settings);
  update_vnc_enabled (settings);
  update_vnc_view_only (settings);
  update_vnc_screen_share_mode (settings);
  update_vnc_auth_method (settings);
}

static void
grd_settings_user_class_init (GrdSettingsUserClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdSettingsClass *settings_class = GRD_SETTINGS_CLASS (klass);

  object_class->finalize = grd_settings_user_finalize;

  settings_class->is_rdp_enabled = grd_settings_user_is_rdp_enabled;
  settings_class->is_vnc_enabled = grd_settings_user_is_vnc_enabled;
  settings_class->get_rdp_view_only = grd_settings_user_get_rdp_view_only;
  settings_class->get_vnc_view_only = grd_settings_user_get_vnc_view_only;
  settings_class->get_rdp_screen_share_mode = grd_settings_user_get_rdp_screen_share_mode;
  settings_class->get_vnc_screen_share_mode = grd_settings_user_get_vnc_screen_share_mode;
  settings_class->get_rdp_server_key = grd_settings_user_get_rdp_server_key;
  settings_class->get_rdp_server_cert = grd_settings_user_get_rdp_server_cert;
  settings_class->get_vnc_auth_method = grd_settings_user_get_vnc_auth_method;

  signals[RDP_ENABLED_CHANGED] =
    g_signal_new ("rdp-enabled-changed",
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
  signals[VNC_SCREEN_SHARE_MODE_CHANGED] =
    g_signal_new ("vnc-screen-share-mode-changed",
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
