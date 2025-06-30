/*
 * Copyright (C) 2022 Red Hat Inc.
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

#pragma once

#include <glib-object.h>

typedef enum _GrdCredentialsType
{
  GRD_CREDENTIALS_TYPE_RDP,
  GRD_CREDENTIALS_TYPE_VNC,
} GrdCredentialsType;

#define GRD_TYPE_CREDENTIALS (grd_credentials_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdCredentials, grd_credentials,
                          GRD, CREDENTIALS, GObject)

struct _GrdCredentialsClass
{
  GObjectClass parent_class;

  gboolean (* store) (GrdCredentials      *credentials,
                      GrdCredentialsType   type,
                      GVariant            *variant,
                      GError             **error);

  GVariant * (* lookup) (GrdCredentials      *credentials,
                         GrdCredentialsType   type,
                         GError             **error);

  gboolean (* clear) (GrdCredentials      *credentials,
                      GrdCredentialsType   type,
                      GError             **error);
};

gboolean grd_credentials_store (GrdCredentials      *credentials,
                                GrdCredentialsType   type,
                                GVariant            *variant,
                                GError             **error);

GVariant * grd_credentials_lookup (GrdCredentials      *credentials,
                                   GrdCredentialsType   type,
                                   GError             **error);

gboolean grd_credentials_clear (GrdCredentials      *credentials,
                                GrdCredentialsType   type,
                                GError             **error);
