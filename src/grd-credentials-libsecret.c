/*
 * Copyright (C) 2018-2022 Red Hat Inc.
 * Copyright (C) 2020 Pascal Nowack
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

#include "grd-credentials-libsecret.h"

#include <gio/gio.h>
#include <libsecret/secret.h>
#include <stdio.h>

#define GRD_RDP_CREDENTIALS_SCHEMA (get_rdp_schema ())

#define GRD_VNC_PASSWORD_SCHEMA (get_vnc_schema ())
#define GRD_VNC_LEGACY_PASSWORD_SCHEMA (get_legacy_vnc_schema ())

struct _GrdCredentialsLibsecret
{
  GrdCredentials parent;
};

G_DEFINE_TYPE (GrdCredentialsLibsecret,
               grd_credentials_libsecret,
               GRD_TYPE_CREDENTIALS)

static const SecretSchema *
get_rdp_schema (void)
{
  static const SecretSchema grd_rdp_credentials_schema = {
    .name = "org.gnome.RemoteDesktop.RdpCredentials",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {
      { "credentials", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { "NULL", 0 },
    },
  };

  return &grd_rdp_credentials_schema;
}

static const SecretSchema *
get_legacy_vnc_schema (void)
{
  static const SecretSchema grd_vnc_password_schema = {
    .name = "org.gnome.RemoteDesktop.VncPassword",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {
      { "password", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { "NULL", 0 },
    },
  };

  return &grd_vnc_password_schema;
}

static const SecretSchema *
get_vnc_schema (void)
{
  static const SecretSchema grd_vnc_password_schema = {
    .name = "org.gnome.RemoteDesktop.VncCredentials",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {
      { "password", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { "NULL", 0 },
    },
  };

  return &grd_vnc_password_schema;
}

static const SecretSchema *
schema_from_type (GrdCredentialsType type)
{
  switch (type)
    {
    case GRD_CREDENTIALS_TYPE_RDP:
      return GRD_RDP_CREDENTIALS_SCHEMA;
    case GRD_CREDENTIALS_TYPE_VNC:
      return GRD_VNC_PASSWORD_SCHEMA;
    }

  g_assert_not_reached ();
}

static const char *
description_from_type (GrdCredentialsType type)
{
  switch (type)
    {
    case GRD_CREDENTIALS_TYPE_RDP:
      return "GNOME Remote Desktop RDP credentials";
    case GRD_CREDENTIALS_TYPE_VNC:
      return "GNOME Remote Desktop VNC password";
    }

  g_assert_not_reached ();
}

static gboolean
grd_credentials_libsecret_store (GrdCredentials      *credentials,
                                 GrdCredentialsType   type,
                                 GVariant            *variant,
                                 GError             **error)
{
  g_autofree char *serialized = NULL;

  g_variant_ref_sink (variant);
  serialized = g_variant_print (variant, TRUE);
  g_variant_unref (variant);

  return secret_password_store_sync (schema_from_type (type),
                                     SECRET_COLLECTION_DEFAULT,
                                     description_from_type (type),
                                     serialized,
                                     NULL, error,
                                     NULL);
}

static GVariant *
grd_credentials_libsecret_lookup (GrdCredentials      *credentials,
                                  GrdCredentialsType   type,
                                  GError             **error)
{
  g_autofree char *serialized = NULL;

  serialized = secret_password_lookup_sync (schema_from_type (type),
                                            NULL, error,
                                            NULL);
  if (!serialized)
    return NULL;

  return g_variant_parse (NULL, serialized, NULL, NULL, error);
}

static gboolean
grd_credentials_libsecret_clear (GrdCredentials      *credentials,
                                 GrdCredentialsType   type,
                                 GError             **error)
{
  return secret_password_clear_sync (schema_from_type (type),
                                     NULL, error,
                                     NULL);
}

GrdCredentialsLibsecret *
grd_credentials_libsecret_new (void)
{
  return g_object_new (GRD_TYPE_CREDENTIALS_LIBSECRET, NULL);
}

static void
grd_credentials_libsecret_init (GrdCredentialsLibsecret *credentials_libsecret)
{
}

static void
maybe_migrate_legacy_vnc_password (GrdCredentials *credentials)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *password = NULL;

  password = secret_password_lookup_sync (GRD_VNC_LEGACY_PASSWORD_SCHEMA,
                                          NULL, &error,
                                          NULL);
  if (!password)
    {
      if (error)
        {
          g_printerr ("Failed to lookup legacy VNC password schema: %s\n",
                      error->message);
        }
    }
  else
    {
      g_printerr ("Migrating VNC password to new schema... ");

      if (!grd_credentials_store (credentials,
                                  GRD_CREDENTIALS_TYPE_VNC,
                                  g_variant_new_string (password),
                                  &error))
        {
          g_printerr ("Failed to migrate VNC password to new schema: %s\n",
                      error->message);
        }
      else
        {
          if (!secret_password_clear_sync (GRD_VNC_LEGACY_PASSWORD_SCHEMA,
                                           NULL, &error, NULL) &&
              error)
            {
              g_printerr ("Failed to clear VNC password from old schema: %s\n",
                          error->message);
            }
          else
            {
              g_printerr ("OK\n");
            }
        }
    }
}

static void
grd_credentials_libsecret_constructed (GObject *object)
{
  GrdCredentials *credentials = GRD_CREDENTIALS (object);

  maybe_migrate_legacy_vnc_password (credentials);

  G_OBJECT_CLASS (grd_credentials_libsecret_parent_class)->constructed (object);
}

static void
grd_credentials_libsecret_class_init (GrdCredentialsLibsecretClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdCredentialsClass *credentials_class = GRD_CREDENTIALS_CLASS (klass);

  object_class->constructed = grd_credentials_libsecret_constructed;

  credentials_class->store = grd_credentials_libsecret_store;
  credentials_class->lookup = grd_credentials_libsecret_lookup;
  credentials_class->clear = grd_credentials_libsecret_clear;
}
