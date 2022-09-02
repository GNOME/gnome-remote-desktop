/*
 * Copyright (C) 2022 SUSE LLC
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
 * Written by:
 *     Alynx Zhou <alynx.zhou@gmail.com>
 */

#include "config.h"

#include "grd-credentials-file.h"

#include <gio/gio.h>

#define GRD_CREDENTIALS_FILE_KEY "credentials"

struct _GrdCredentialsFile
{
  GrdCredentials parent;

  char *filename;
  GKeyFile *key_file;
};

G_DEFINE_TYPE (GrdCredentialsFile, grd_credentials_file, GRD_TYPE_CREDENTIALS)

static const char *
group_name_from_type (GrdCredentialsType type)
{
  switch (type)
    {
    case GRD_CREDENTIALS_TYPE_RDP:
      return "RDP";
    case GRD_CREDENTIALS_TYPE_VNC:
      return "VNC";
    }

  g_assert_not_reached ();
}

static gboolean
grd_credentials_file_store (GrdCredentials      *credentials,
                            GrdCredentialsType   type,
                            GVariant            *variant,
                            GError             **error)
{
  GrdCredentialsFile *credentials_file = GRD_CREDENTIALS_FILE (credentials);
  g_autofree const char *serialized = NULL;

  g_variant_ref_sink (variant);
  serialized = g_variant_print (variant, TRUE);
  g_variant_unref (variant);

  g_key_file_set_string (credentials_file->key_file,
                         group_name_from_type (type),
                         GRD_CREDENTIALS_FILE_KEY, serialized);

  return g_key_file_save_to_file (credentials_file->key_file,
                                  credentials_file->filename, error);
}

static GVariant *
grd_credentials_file_lookup (GrdCredentials      *credentials,
                             GrdCredentialsType   type,
                             GError             **error)
{
  GrdCredentialsFile *credentials_file = GRD_CREDENTIALS_FILE (credentials);
  g_autofree const char *serialized = NULL;
  g_autoptr (GError) local_error = NULL;

  serialized = g_key_file_get_string (credentials_file->key_file,
                                      group_name_from_type (type),
                                      GRD_CREDENTIALS_FILE_KEY, &local_error);
  if (!serialized)
    {
      if (!g_error_matches (local_error,
                            G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND) &&
          !g_error_matches (local_error,
                            G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        g_propagate_error (error, g_steal_pointer (&local_error));

      return NULL;
    }

  return g_variant_parse (NULL, serialized, NULL, NULL, error);
}

static gboolean
grd_credentials_file_clear (GrdCredentials      *credentials,
                            GrdCredentialsType   type,
                            GError             **error)
{
  GrdCredentialsFile *credentials_file = GRD_CREDENTIALS_FILE (credentials);
  g_autoptr (GError) local_error = NULL;
  gboolean removed;

  removed = g_key_file_remove_key (credentials_file->key_file,
                                   group_name_from_type (type),
                                   GRD_CREDENTIALS_FILE_KEY, &local_error);
  if (!removed)
    {
      if (!g_error_matches (local_error,
                            G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND) &&
          !g_error_matches (local_error,
                            G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
      else
        {
          return TRUE;
        }
    }

  return g_key_file_save_to_file (credentials_file->key_file,
                                  credentials_file->filename, error);
}

GrdCredentialsFile *
grd_credentials_file_new (GError **error)
{
  g_autoptr (GrdCredentialsFile) credentials_file = NULL;
  g_autofree char *dir_path = NULL;
  g_autoptr (GFile) dir = NULL;

  credentials_file = g_object_new (GRD_TYPE_CREDENTIALS_FILE, NULL);

  dir_path = g_build_path ("/",
                           g_get_user_data_dir (),
                           "gnome-remote-desktop",
                           NULL);
  dir = g_file_new_for_path (dir_path);
  if (!g_file_query_exists (dir, NULL))
    {
      if (!g_file_make_directory_with_parents (dir, NULL, error))
        return NULL;
    }
  credentials_file->filename = g_build_path ("/",
                                             dir_path,
                                             "credentials.ini",
                                             NULL);

  if (!g_file_test (credentials_file->filename, G_FILE_TEST_IS_REGULAR))
    if (!g_file_set_contents (credentials_file->filename, "", -1, error))
      return NULL;

  if (!g_key_file_load_from_file (credentials_file->key_file,
                                  credentials_file->filename,
                                  G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                  error))
    return NULL;

  return g_steal_pointer (&credentials_file);
}

static void
grd_credentials_file_finalize (GObject *object)
{
  GrdCredentialsFile *credentials_file = GRD_CREDENTIALS_FILE (object);

  g_clear_pointer (&credentials_file->filename, g_free);
  g_clear_pointer (&credentials_file->key_file, g_key_file_unref);

  G_OBJECT_CLASS (grd_credentials_file_parent_class)->finalize (object);
}

static void
grd_credentials_file_init (GrdCredentialsFile *credentials_file)
{
  credentials_file->key_file = g_key_file_new ();
}

static void
grd_credentials_file_class_init (GrdCredentialsFileClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdCredentialsClass *credentials_class = GRD_CREDENTIALS_CLASS (klass);

  object_class->finalize = grd_credentials_file_finalize;

  credentials_class->store = grd_credentials_file_store;
  credentials_class->lookup = grd_credentials_file_lookup;
  credentials_class->clear = grd_credentials_file_clear;
}
