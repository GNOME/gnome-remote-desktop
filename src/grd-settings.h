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

typedef struct _GrdSettings GrdSettings;

#define GRD_TYPE_SETTINGS (grd_settings_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdSettings, grd_settings,
                          GRD, SETTINGS, GObject)

struct _GrdSettingsClass
{
  GObjectClass parent_class;
};

GrdRuntimeMode grd_settings_get_runtime_mode (GrdSettings *settings);

void grd_settings_override_rdp_port (GrdSettings *settings,
                                     int          port);

void grd_settings_override_vnc_port (GrdSettings *settings,
                                     int          port);

gboolean grd_settings_get_rdp_credentials (GrdSettings  *settings,
                                           char        **username,
                                           char        **password,
                                           GError      **error);

gboolean grd_settings_set_rdp_credentials (GrdSettings  *settings,
                                           const char   *username,
                                           const char   *password,
                                           GError      **error);

gboolean grd_settings_clear_rdp_credentials (GrdSettings  *settings,
                                             GError      **error);

void grd_settings_recreate_rdp_credentials (GrdSettings *settings);

char *grd_settings_get_vnc_password (GrdSettings *settings,
                                      GError     **error);

gboolean grd_settings_set_vnc_password (GrdSettings  *settings,
                                        const char   *password,
                                        GError      **error);

gboolean grd_settings_clear_vnc_password (GrdSettings  *settings,
                                          GError      **error);

#endif /* GRD_SETTINGS_H */
