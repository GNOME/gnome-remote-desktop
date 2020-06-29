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
#include <libsecret/secret.h>

#include "grd-enums.h"

#define GRD_TYPE_SETTINGS (grd_settings_get_type ())
G_DECLARE_FINAL_TYPE (GrdSettings, grd_settings,
                      GRD, SETTINGS, GObject)

const SecretSchema * cc_grd_rdp_credentials_get_schema (void);
const SecretSchema * cc_grd_vnc_password_get_schema (void);
#define GRD_RDP_CREDENTIALS_SCHEMA grd_rdp_credentials_get_schema ()
#define GRD_VNC_PASSWORD_SCHEMA grd_vnc_password_get_schema ()

int grd_settings_get_rdp_port (GrdSettings *settings);

int grd_settings_get_vnc_port (GrdSettings *settings);

void grd_settings_override_rdp_port (GrdSettings *settings,
                                     int          port);

void grd_settings_override_vnc_port (GrdSettings *settings,
                                     int          port);

char * grd_settings_get_rdp_server_cert (GrdSettings *settings);

char * grd_settings_get_rdp_server_key (GrdSettings *settings);

char * grd_settings_get_rdp_password (GrdSettings  *settings,
                                      GError      **error);

char * grd_settings_get_vnc_password (GrdSettings  *settings,
                                      GError      **error);

char * grd_settings_get_rdp_username (GrdSettings  *settings,
                                      GError      **error);

gboolean grd_settings_get_rdp_view_only (GrdSettings *settings);

gboolean grd_settings_get_vnc_view_only (GrdSettings *settings);

GrdVncAuthMethod grd_settings_get_vnc_auth_method (GrdSettings *settings);

#endif /* GRD_SETTINGS_H */
