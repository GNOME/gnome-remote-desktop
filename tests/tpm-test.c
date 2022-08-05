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

#include <gio/gio.h>
#include <stdio.h>

#include "grd-tpm.h"

#define SECRET "secret value"

static GVariant *secret_variant;

static void
test_tpm_caps (void)
{
  g_autoptr (GrdTpm) tpm = NULL;
  g_autoptr (GError) error = NULL;

  tpm = grd_tpm_new (GRD_TPM_MODE_NONE, &error);
  if (!tpm)
    g_error ("Failed to create TPM credentials manager: %s", error->message);

  if (!grd_tpm_check_capabilities (tpm, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        g_debug ("Incompatible TPM 2.0 module: %s", error->message);
      else
        g_error ("Capability check failed: %s", error->message);
    }
}

static gboolean
check_compatibility (GrdTpm *tpm)
{
  g_autoptr (GError) error = NULL;

  if (!grd_tpm_check_capabilities (tpm, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        return FALSE;
      else
        g_error ("Failed tocheck TPM 2.0 compatibility: %s", error->message);
    }
  else
    {
      return TRUE;
    }
}

static void
test_tpm_write (void)
{
  g_autoptr (GrdTpm) tpm = NULL;
  g_autoptr (GError) error = NULL;
  ESYS_TR primary_handle = 0;
  g_autofree TPMS_CONTEXT *primary = NULL;
  g_autofree TPML_PCR_SELECTION *pcr_selection = NULL;
  g_autofree TPML_DIGEST *pcr_digest = NULL;
  g_autofree TPMS_CONTEXT *secret_tpms_context = NULL;

  tpm = grd_tpm_new (GRD_TPM_MODE_WRITE, &error);
  if (!tpm)
    g_error ("Failed to create TPM credentials manager: %s", error->message);

  if (!check_compatibility (tpm))
    {
      g_test_skip ("TPM module not compatible");
      return;
    }

  if (!grd_tpm_create_primary (tpm, &primary_handle, &error))
    g_error ("Failed to create primary: %s", error->message);

  g_assert_cmpuint (primary_handle, >, 0);

  if (!grd_tpm_read_pcr (tpm, &pcr_selection, &pcr_digest, &error))
    g_error ("Failed to read PCR: %s", error->message);

  g_assert_nonnull (pcr_selection);
  g_assert_nonnull (pcr_digest);

  if (!grd_tpm_store_secret (tpm,
                             SECRET,
                             primary_handle,
                             pcr_selection,
                             pcr_digest,
                             &secret_tpms_context,
                             &error))
    g_error ("Failed to store secret: %s", error->message);

  g_assert_nonnull (secret_tpms_context);
  g_assert_cmpuint (secret_tpms_context->sequence, !=, 0);
  g_assert_cmpuint (secret_tpms_context->savedHandle, !=, 0);
  g_assert_cmpuint (secret_tpms_context->hierarchy, !=, 0);
  g_assert_cmpuint (secret_tpms_context->contextBlob.size, >, 0);

  secret_variant = grd_tpms_context_to_variant (secret_tpms_context);
}

static void
test_tpm_read (void)
{
  g_autoptr (GrdTpm) tpm = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree TPML_PCR_SELECTION *pcr_selection = NULL;
  g_autofree TPML_DIGEST *pcr_digest = NULL;
  g_autofree char *secret = NULL;

  tpm = grd_tpm_new (GRD_TPM_MODE_READ, &error);
  if (!tpm)
    g_error ("Failed to create TPM credentials reader: %s", error->message);

  if (!check_compatibility (tpm))
    {
      g_test_skip ("TPM module not compatible");
      return;
    }

  g_assert_nonnull (secret_variant);

  if (!grd_tpm_read_pcr (tpm, &pcr_selection, &pcr_digest, &error))
    g_error ("Failed to read PCR: %s", error->message);

  g_assert_nonnull (pcr_selection);
  g_assert_nonnull (pcr_digest);

  secret = grd_tpm_restore_secret (tpm,
                                   secret_variant,
                                   pcr_selection,
                                   pcr_digest,
                                   &error);
  if (!secret)
    g_error ("Failed to restore secret: %s", error->message);

  g_assert_cmpstr (secret, ==, SECRET);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (GFile) tpm_dev = NULL;
  g_autoptr (GFile) tpmrm_dev = NULL;

  g_test_init (&argc, &argv, NULL);

  tpm_dev = g_file_new_for_path ("/dev/tpm0");
  tpmrm_dev = g_file_new_for_path ("/dev/tpmrm0");
  if (!g_file_query_exists (tpm_dev, NULL) ||
      !g_file_query_exists (tpmrm_dev, NULL))
    {
      g_printerr ("No /dev/tpm0 and /dev/tpmrm0, skipping TPM test");
      return 77;
    }

  g_test_add_func ("/tpm/caps",
                   test_tpm_caps);
  g_test_add_func ("/tpm/write",
                   test_tpm_write);
  g_test_add_func ("/tpm/read",
                   test_tpm_read);

  return g_test_run ();
}
