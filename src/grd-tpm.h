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

#ifndef GRD_TPM_H
#define GRD_TPM_H

#include <glib-object.h>
#include <tss2_esys.h>
#include <tss2_tpm2_types.h>

typedef enum _GrdTpmMode
{
  GRD_TPM_MODE_WRITE,
  GRD_TPM_MODE_READ,
} GrdTpmMode;

#define GRD_TYPE_TPM (grd_tpm_get_type ())
G_DECLARE_FINAL_TYPE (GrdTpm, grd_tpm, GRD, TPM, GObject)

GrdTpm * grd_tpm_new (GrdTpmMode   mode,
                      GError     **error);

gboolean grd_tpm_create_primary (GrdTpm   *tpm,
                                 ESYS_TR  *primary_handle_out,
                                 GError  **error);

gboolean grd_tpm_read_pcr (GrdTpm              *tpm,
                           TPML_PCR_SELECTION **pcr_selection_out,
                           TPML_DIGEST        **pcr_digest_out,
                           GError             **error);

gboolean grd_tpm_store_secret (GrdTpm              *tpm,
                               const char          *secret,
                               ESYS_TR              primary_handle,
                               TPML_PCR_SELECTION  *pcr_selection,
                               TPML_DIGEST         *pcr_digest,
                               TPMS_CONTEXT       **secret_tpms_context_out,
                               GError             **error);

GVariant * grd_tpms_context_to_variant (TPMS_CONTEXT *tpms_context);

char * grd_tpm_restore_secret (GrdTpm              *tpm,
                               GVariant            *secret_variant,
                               TPML_PCR_SELECTION  *pcr_selection,
                               TPML_DIGEST         *pcr_digest,
                               GError             **error);

#endif /* GRD_TPM_H */
