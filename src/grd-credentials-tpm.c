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

#include "config.h"

#include "grd-credentials-tpm.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#include "grd-tpm.h"

struct _GrdCredentialsTpm
{
  GrdCredentials parent;
};

G_DEFINE_TYPE (GrdCredentialsTpm, grd_credentials_tpm, GRD_TYPE_CREDENTIALS)

static const char *
secret_file_name_from_type (GrdCredentialsType type)
{
  switch (type)
    {
    case GRD_CREDENTIALS_TYPE_RDP:
      return "rdp-credentials.priv";
    case GRD_CREDENTIALS_TYPE_VNC:
      return "vnc-credentials.priv";
    }

  g_assert_not_reached ();
}

static gboolean
grd_credentials_tpm_store (GrdCredentials      *credentials,
                           GrdCredentialsType   type,
                           GVariant            *variant,
                           GError             **error)
{
  g_autoptr (GrdTpm) tpm = NULL;
  g_autofree const char *serialized = NULL;
  ESYS_TR primary_handle = 0;
  g_autofree TPMS_CONTEXT *primary = NULL;
  g_autofree TPML_PCR_SELECTION *pcr_selection = NULL;
  g_autofree TPML_DIGEST *pcr_digest = NULL;
  g_autofree TPMS_CONTEXT *secret_tpms_context = NULL;
  g_autoptr (GVariant) secret_variant = NULL;
  g_autofree char *secret_serialized = NULL;
  g_autofree char *dir_path = NULL;
  g_autoptr (GFile) dir = NULL;
  const char *secret_file_name;
  g_autofree char *secret_path = NULL;
  g_autoptr (GFile) secret_file = NULL;

  g_variant_ref_sink (variant);
  serialized = g_variant_print (variant, TRUE);
  g_variant_unref (variant);

  tpm = grd_tpm_new (GRD_TPM_MODE_WRITE, error);
  if (!tpm)
    return FALSE;

  if (!grd_tpm_create_primary (tpm, &primary_handle, error))
    return FALSE;

  if (!grd_tpm_read_pcr (tpm, &pcr_selection, &pcr_digest, error))
    return FALSE;

  if (!grd_tpm_store_secret (tpm,
                             serialized,
                             primary_handle,
                             pcr_selection,
                             pcr_digest,
                             &secret_tpms_context,
                             error))
    return FALSE;

  secret_variant = grd_tpms_context_to_variant (secret_tpms_context);

  dir_path = g_build_path ("/",
                           g_get_user_data_dir (),
                           "gnome-remote-desktop",
                           NULL);
  dir = g_file_new_for_path (dir_path);
  if (!g_file_query_exists (dir, NULL))
    {
      if (!g_file_make_directory_with_parents (dir, NULL, error))
        return FALSE;
    }

  secret_file_name = secret_file_name_from_type (type);
  secret_path = g_build_path ("/", dir_path, secret_file_name, NULL);
  secret_file = g_file_new_for_path (secret_path);

  secret_serialized = g_variant_print (secret_variant, TRUE);

  return g_file_replace_contents (secret_file,
                                  secret_serialized,
                                  strlen (secret_serialized) + 1,
                                  NULL, FALSE,
                                  G_FILE_CREATE_PRIVATE |
                                  G_FILE_CREATE_REPLACE_DESTINATION,
                                  NULL, NULL, error);
}

static GVariant *
grd_credentials_tpm_lookup (GrdCredentials      *credentials,
                            GrdCredentialsType   type,
                            GError             **error)
{
  g_autoptr (GrdTpm) tpm = NULL;
  const char *secret_file_name;
  g_autofree char *secret_path = NULL;
  g_autofree char *serialized = NULL;
  g_autofree TPML_PCR_SELECTION *pcr_selection = NULL;
  g_autofree TPML_DIGEST *pcr_digest = NULL;
  g_autoptr (GVariant) secret_variant = NULL;
  g_autoptr (GError) local_error = NULL;
  g_autofree char *credentials_string = NULL;

  secret_file_name = secret_file_name_from_type (type);
  secret_path = g_build_path ("/",
                              g_get_user_data_dir (),
                              "gnome-remote-desktop",
                              secret_file_name,
                              NULL);

  if (!g_file_get_contents (secret_path,
                            &serialized,
                            NULL,
                            &local_error))
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        return NULL;

      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  secret_variant = g_variant_parse (G_VARIANT_TYPE ("(uutqs)"), serialized,
                                    NULL, NULL, error);
  if (!secret_variant)
    return NULL;

  tpm = grd_tpm_new (GRD_TPM_MODE_READ, error);
  if (!tpm)
    return FALSE;

  if (!grd_tpm_read_pcr (tpm, &pcr_selection, &pcr_digest, error))
    return NULL;

  credentials_string = grd_tpm_restore_secret (tpm,
                                               secret_variant,
                                               pcr_selection,
                                               pcr_digest,
                                               error);
  if (!credentials_string)
    return NULL;

  return g_variant_parse (NULL, credentials_string, NULL, NULL, error);
}

static gboolean
grd_credentials_tpm_clear (GrdCredentials      *credentials,
                           GrdCredentialsType   type,
                           GError             **error)
{
  const char *secret_file_name;
  g_autofree char *secret_path = NULL;
  g_autoptr (GFile) secret_file = NULL;
  g_autoptr (GError) local_error = NULL;

  secret_file_name = secret_file_name_from_type (type);
  secret_path = g_build_path ("/",
                              g_get_user_data_dir (),
                              "gnome-remote-desktop",
                              secret_file_name,
                              NULL);
  secret_file = g_file_new_for_path (secret_path);

  if (!g_file_delete (secret_file, NULL, error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          return TRUE;
        }
      else
        {
          g_propagate_error (error, local_error);
          return FALSE;
        }
    }

  return TRUE;
}

GrdCredentialsTpm *
grd_credentials_tpm_new (GError **error)
{
  return g_object_new (GRD_TYPE_CREDENTIALS_TPM, NULL);
}

static void
grd_credentials_tpm_init (GrdCredentialsTpm *credentials_tpm)
{
}

static void
grd_credentials_tpm_class_init (GrdCredentialsTpmClass *klass)
{
  GrdCredentialsClass *credentials_class = GRD_CREDENTIALS_CLASS (klass);

  credentials_class->store = grd_credentials_tpm_store;
  credentials_class->lookup = grd_credentials_tpm_lookup;
  credentials_class->clear = grd_credentials_tpm_clear;
}
