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

#define GRD_TYPE_SETTINGS (grd_settings_get_type ())
G_DECLARE_FINAL_TYPE (GrdSettings, grd_settings,
                      GRD, SETTINGS, GObject)

gboolean grd_settings_get_vnc_view_only (GrdSettings *settings);

#endif /* GRD_SETTINGS_H */
