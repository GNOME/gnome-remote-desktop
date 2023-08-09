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

#ifndef GRD_SETTINGS_H
#define GRD_SETTINGS_H

#include <glib-object.h>

#include "grd-enums.h"
#include "grd-types.h"

#define GRD_RDP_SCHEMA_ID "org.gnome.desktop.remote-desktop.rdp"
#define GRD_VNC_SCHEMA_ID "org.gnome.desktop.remote-desktop.vnc"

typedef struct _GrdSettings GrdSettings;

#define GRD_TYPE_SETTINGS (grd_settings_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdSettings, grd_settings,
                          GRD, SETTINGS, GObject)

struct _GrdSettingsClass
{
  GObjectClass parent_class;

  gboolean (* is_rdp_enabled) (GrdSettings *settings);

  gboolean (* is_vnc_enabled) (GrdSettings *settings);

  gboolean (* get_rdp_view_only) (GrdSettings *settings);

  gboolean (* get_vnc_view_only) (GrdSettings *settings);

  GrdRdpScreenShareMode (* get_rdp_screen_share_mode) (GrdSettings *settings);

  GrdVncScreenShareMode (* get_vnc_screen_share_mode) (GrdSettings *settings);

  char * (* get_rdp_server_cert) (GrdSettings *settings);

  char * (* get_rdp_server_key) (GrdSettings *settings);

  void (* override_rdp_server_cert) (GrdSettings *settings,
                                     const char  *cert);

  void (* override_rdp_server_key) (GrdSettings *settings,
                                    const char  *key);

  GrdVncAuthMethod (* get_vnc_auth_method) (GrdSettings *settings);
};

int grd_settings_get_rdp_port (GrdSettings *settings);

int grd_settings_get_vnc_port (GrdSettings *settings);

void grd_settings_override_rdp_port (GrdSettings *settings,
                                     int          port);

void grd_settings_override_vnc_port (GrdSettings *settings,
                                     int          port);

gboolean grd_settings_get_rdp_credentials (GrdSettings  *settings,
                                           char        **username,
                                           char        **password,
                                           GError      **error);

char *grd_settings_get_vnc_password (GrdSettings *settings,
                                      GError     **error);

gboolean grd_settings_is_rdp_enabled (GrdSettings *settings);

gboolean grd_settings_is_vnc_enabled (GrdSettings *settings);

gboolean grd_settings_get_rdp_view_only (GrdSettings *settings);

gboolean grd_settings_get_vnc_view_only (GrdSettings *settings);

GrdRdpScreenShareMode grd_settings_get_rdp_screen_share_mode (GrdSettings *settings);

GrdVncScreenShareMode grd_settings_get_vnc_screen_share_mode (GrdSettings *settings);

char *grd_settings_get_rdp_server_cert (GrdSettings *settings);

char *grd_settings_get_rdp_server_key (GrdSettings *settings);

void grd_settings_override_rdp_server_cert (GrdSettings *settings,
                                            const char  *cert);

void grd_settings_override_rdp_server_key (GrdSettings *settings,
                                           const char  *key);

GrdVncAuthMethod grd_settings_get_vnc_auth_method (GrdSettings *settings);

#endif /* GRD_SETTINGS_H */
