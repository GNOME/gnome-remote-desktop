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

#include "grd-tpm.h"

#include <gio/gio.h>
#include <stdio.h>

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
#include <tss2_esys.h>
#include <tss2_mu.h>
#include <tss2_rc.h>
#include <tss2_tctildr.h>
G_GNUC_END_IGNORE_DEPRECATIONS

#include "grd-debug.h"

struct _GrdTpm
{
  GObject parent;

  TSS2_TCTI_CONTEXT *tcti_context;
  ESYS_CONTEXT *esys_context;

  ESYS_TR hmac_session;
  ESYS_TR trial_session;
  ESYS_TR policy_session;
};

G_DEFINE_TYPE (GrdTpm, grd_tpm, G_TYPE_OBJECT)

static gboolean
check_tpm_existence (GError **error)
{
  g_autoptr (GFile) tpm_dev = NULL;
  g_autoptr (GFile) tpmrm_dev = NULL;

  tpm_dev = g_file_new_for_path ("/dev/tpm0");
  tpmrm_dev = g_file_new_for_path ("/dev/tpmrm0");
  if (!g_file_query_exists (tpm_dev, NULL) ||
      !g_file_query_exists (tpmrm_dev, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No TPM device found");
      return FALSE;
    }

  return TRUE;
}

static gboolean
init_transmission_interface (GrdTpm  *tpm,
                             GError **error)
{
  TSS2_RC rc;

  rc = Tss2_TctiLdr_Initialize (NULL, &tpm->tcti_context);

  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to initialize transmission interface context: %s",
                   Tss2_RC_Decode (rc));
      return FALSE;
    }

  return TRUE;
}

static gboolean
init_tss2_esys (GrdTpm  *tpm,
                GError **error)
{
  TSS2_RC rc;

  rc = Esys_Initialize (&tpm->esys_context, tpm->tcti_context, NULL);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to initialize ESYS context: %s",
                   Tss2_RC_Decode (rc));
      return FALSE;
    }

  return TRUE;
}

static gboolean
start_hmac_session (GrdTpm  *tpm,
                    GError **error)
{
  TPMT_SYM_DEF symmetric = {
    .algorithm = TPM2_ALG_NULL,
  };
  TSS2_RC rc;

  rc = Esys_StartAuthSession (tpm->esys_context,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              NULL,
                              TPM2_SE_HMAC,
                              &symmetric,
                              TPM2_ALG_SHA256,
                              &tpm->hmac_session);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to start auth session: %s (%d)",
                   Tss2_RC_Decode (rc), rc);
      return FALSE;
    }

  return TRUE;
}

static gboolean
start_trial_session (GrdTpm  *tpm,
                     GError **error)
{
  TPMT_SYM_DEF symmetric = {
    .algorithm = TPM2_ALG_NULL,
  };
  TSS2_RC rc;

  rc = Esys_StartAuthSession (tpm->esys_context,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              NULL,
                              TPM2_SE_TRIAL,
                              &symmetric,
                              TPM2_ALG_SHA256,
                              &tpm->trial_session);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to start policy session: %s (%d)",
                   Tss2_RC_Decode (rc), rc);
      return FALSE;
    }

  return TRUE;
}

static gboolean
start_policy_session (GrdTpm  *tpm,
                      GError **error)
{
  TPMT_SYM_DEF symmetric = {
    .algorithm = TPM2_ALG_NULL,
  };
  TSS2_RC rc;

  rc = Esys_StartAuthSession (tpm->esys_context,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              ESYS_TR_NONE,
                              NULL,
                              TPM2_SE_POLICY,
                              &symmetric,
                              TPM2_ALG_SHA256,
                              &tpm->policy_session);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to start policy session: %s (%d)",
                   Tss2_RC_Decode (rc), rc);
      return FALSE;
    }

  return TRUE;
}

gboolean
grd_tpm_create_primary (GrdTpm   *tpm,
                        ESYS_TR  *primary_handle_out,
                        GError  **error)
{
  TPM2B_SENSITIVE_CREATE in_sensitive_primary = {};
  TPM2B_PUBLIC in_public = {
    .publicArea = {
      .type = TPM2_ALG_RSA,
      .nameAlg = TPM2_ALG_SHA256,
      .objectAttributes = (TPMA_OBJECT_RESTRICTED |
                           TPMA_OBJECT_DECRYPT |
                           TPMA_OBJECT_FIXEDTPM |
                           TPMA_OBJECT_FIXEDPARENT |
                           TPMA_OBJECT_SENSITIVEDATAORIGIN |
                           TPMA_OBJECT_USERWITHAUTH),
    },
  };
  TPM2B_DATA outside_info = {};
  TPML_PCR_SELECTION creation_pcr = {};
  TSS2_RC rc;

  g_assert (tpm->hmac_session);

  in_public.publicArea.parameters.rsaDetail.keyBits = 2048;
  in_public.publicArea.parameters.asymDetail.scheme.scheme = TPM2_ALG_NULL;
  in_public.publicArea.parameters.symDetail.sym.algorithm = TPM2_ALG_AES;
  in_public.publicArea.parameters.symDetail.sym.keyBits.sym = 128;
  in_public.publicArea.parameters.symDetail.sym.mode.sym = TPM2_ALG_CFB;

  rc = Esys_CreatePrimary (tpm->esys_context,
                           ESYS_TR_RH_OWNER,
                           tpm->hmac_session,
                           ESYS_TR_NONE,
                           ESYS_TR_NONE,
                           &in_sensitive_primary,
                           &in_public,
                           &outside_info,
                           &creation_pcr,
                           primary_handle_out,
                           NULL, NULL, NULL, NULL);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Esys_CreatePrimary failed: %s",
                   Tss2_RC_Decode (rc));
      return FALSE;
    }

  return TRUE;
}

gboolean
grd_tpm_read_pcr (GrdTpm              *tpm,
                  TPML_PCR_SELECTION **pcr_selection_out,
                  TPML_DIGEST        **pcr_digest_out,
                  GError             **error)
{
  TPML_PCR_SELECTION pcr_selection_in = {
    .count = 1,
    .pcrSelections = {
      {
        .hash = TPM2_ALG_SHA256,
        .sizeofSelect = 3,
        .pcrSelect = { 1 << 0 | 1 << 1 | 1 << 2 | 1 << 3, },
      },
    }
  };
  TSS2_RC rc;

  rc = Esys_PCR_Read (tpm->esys_context,
                      ESYS_TR_NONE,
                      ESYS_TR_NONE,
                      ESYS_TR_NONE,
                      &pcr_selection_in,
                      NULL,
                      pcr_selection_out,
                      pcr_digest_out);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Esys_PCR_Read failed: %s",
                   Tss2_RC_Decode (rc));
      return FALSE;
    }

  return TRUE;
}

gboolean
do_create_policy (GrdTpm              *tpm,
                  ESYS_TR              session,
                  TPML_PCR_SELECTION  *pcr_selection,
                  TPML_DIGEST         *pcr_digest,
                  TPM2B_DIGEST       **policy_digest_out,
                  GError             **error)
{
  g_autoptr (GChecksum) checksum = NULL;
  TPM2B_DIGEST pcr_hash_digest = {};
  size_t size;
  int i;
  TSS2_RC rc;

  checksum = g_checksum_new (G_CHECKSUM_SHA256);
  for (i = 0; i < pcr_digest->count; i++)
    {
      g_checksum_update (checksum,
                         pcr_digest->digests[i].buffer,
                         pcr_digest->digests[i].size);
    }

  size = G_N_ELEMENTS (pcr_hash_digest.buffer);
  g_checksum_get_digest (checksum,
                         (uint8_t *) &pcr_hash_digest.buffer,
                         &size);
  pcr_hash_digest.size = size;

  rc = Esys_PolicyPCR (tpm->esys_context, session,
                       ESYS_TR_NONE,
                       ESYS_TR_NONE,
                       ESYS_TR_NONE,
                       &pcr_hash_digest,
                       pcr_selection);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Esys_PolicyPCR failed: %s",
                   Tss2_RC_Decode (rc));
      return FALSE;
    }

  rc = Esys_PolicyGetDigest (tpm->esys_context, session,
                             ESYS_TR_NONE,
                             ESYS_TR_NONE,
                             ESYS_TR_NONE,
                             policy_digest_out);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Esys_PolicyGetDigest failed: %s",
                   Tss2_RC_Decode (rc));
      return FALSE;
    }

  return TRUE;
}

gboolean
grd_tpm_store_secret (GrdTpm              *tpm,
                      const char          *secret,
                      ESYS_TR              primary_handle,
                      TPML_PCR_SELECTION  *pcr_selection,
                      TPML_DIGEST         *pcr_digest,
                      TPMS_CONTEXT       **secret_tpms_context_out,
                      GError             **error)
{
  size_t secret_size;
  size_t offset = 0;
  g_autofree TPM2B_DIGEST *policy_digest = NULL;
  TPM2B_TEMPLATE template = {};
  TPM2B_PUBLIC template_public;
  TPM2B_SENSITIVE_CREATE in_sensitive;
  ESYS_TR secret_handle;
  TSS2_RC rc;

  g_assert (tpm->trial_session);

  secret_size = strlen (secret) + 1;
  if (secret_size > G_N_ELEMENTS (in_sensitive.sensitive.data.buffer))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Secret too large (%lu exceeds %lu max)",
                   secret_size,
                   G_N_ELEMENTS (in_sensitive.sensitive.data.buffer));
      return FALSE;
    }

  if (!do_create_policy (tpm, tpm->trial_session,
                         pcr_selection,
                         pcr_digest,
                         &policy_digest,
                         error))
    return FALSE;

  template_public = (TPM2B_PUBLIC) {
    .publicArea = {
      .type = TPM2_ALG_KEYEDHASH,
      .nameAlg = TPM2_ALG_SHA256,
      .objectAttributes = TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT,
      .parameters.keyedHashDetail.scheme.scheme = TPM2_ALG_NULL,
    },
  };
  memcpy (template_public.publicArea.authPolicy.buffer,
          policy_digest->buffer,
          policy_digest->size);
  template_public.publicArea.authPolicy.size = policy_digest->size;

  rc = Tss2_MU_TPMT_PUBLIC_Marshal (&template_public.publicArea,
                                    template.buffer,
                                    G_N_ELEMENTS (template.buffer),
                                    &offset);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to marshal public template: %s",
                   Tss2_RC_Decode (rc));
      return FALSE;
    }
  template.size = offset;

  in_sensitive = (TPM2B_SENSITIVE_CREATE) {
    .sensitive.data.size = secret_size,
  };
  memcpy (in_sensitive.sensitive.data.buffer, secret, secret_size);

  rc = Esys_CreateLoaded (tpm->esys_context,
                          primary_handle,
                          tpm->hmac_session,
                          ESYS_TR_NONE,
                          ESYS_TR_NONE,
                          &in_sensitive,
                          &template,
                          &secret_handle,
                          NULL, NULL);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Esys_CreateLoaded failed: %s",
                   Tss2_RC_Decode (rc));
      return FALSE;
    }

  rc = Esys_ContextSave (tpm->esys_context,
                         secret_handle,
                         secret_tpms_context_out);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Esys_ContextSave failed: %s",
                   Tss2_RC_Decode (rc));
      return FALSE;
    }

  return TRUE;
}

GVariant *
grd_tpms_context_to_variant (TPMS_CONTEXT *tpms_context)
{
  g_autofree char *base64 = NULL;
  GVariant *variant;

  base64 = g_base64_encode (tpms_context->contextBlob.buffer,
                            tpms_context->contextBlob.size);

  variant = g_variant_new ("(uutqs)",
                           tpms_context->hierarchy,
                           tpms_context->savedHandle,
                           tpms_context->sequence,
                           tpms_context->contextBlob.size,
                           base64);

  return variant;
}

static gboolean
decode_variant (GVariant      *variant,
                TPMS_CONTEXT  *tpms_context,
                GError       **error)
{
  g_autofree char *base64 = NULL;
  g_autofree uint8_t *decoded = NULL;
  size_t size;

  if (!g_variant_is_of_type (variant, G_VARIANT_TYPE ("(uutqs)")))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid tpms context variant type");
      return FALSE;
    }
  g_variant_get (variant, "(uutqs)",
                 &tpms_context->hierarchy,
                 &tpms_context->savedHandle,
                 &tpms_context->sequence,
                 &tpms_context->contextBlob.size,
                 &base64);

  decoded = g_base64_decode (base64, &size);
  if (size != tpms_context->contextBlob.size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid tpms context blob length");
      return FALSE;
    }

  memcpy (tpms_context->contextBlob.buffer, decoded, size);

  return TRUE;
}

char *
grd_tpm_restore_secret (GrdTpm              *tpm,
                        GVariant            *variant,
                        TPML_PCR_SELECTION  *pcr_selection,
                        TPML_DIGEST         *pcr_digest,
                        GError             **error)
{
  TSS2_RC rc;
  TPMS_CONTEXT secret_tpms_context;
  ESYS_TR loaded_secret_handle;
  TPM2B_SENSITIVE_DATA *unsealed_data;

  g_assert (tpm->policy_session);

  if (!decode_variant (variant, &secret_tpms_context, error))
    return NULL;

  rc = Esys_ContextLoad (tpm->esys_context, &secret_tpms_context,
                         &loaded_secret_handle);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Esys_ContextLoad failed: %s",
                   Tss2_RC_Decode (rc));
      return NULL;
    }

  if (!do_create_policy (tpm, tpm->policy_session,
                         pcr_selection,
                         pcr_digest,
                         NULL,
                         error))
    return NULL;

  rc = Esys_Unseal (tpm->esys_context,
                    loaded_secret_handle,
                    tpm->policy_session,
                    ESYS_TR_NONE,
                    ESYS_TR_NONE,
                    &unsealed_data);
  if (rc != TSS2_RC_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Esys_Unseal failed: %s",
                   Tss2_RC_Decode (rc));
      return NULL;
    }

  if (strnlen ((char *) unsealed_data->buffer, unsealed_data->size) !=
      unsealed_data->size - 1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Secret not a zero terminated string");
      return NULL;
    }

  return g_strdup ((char *) unsealed_data->buffer);
}

static const char *
get_algorithm_name (uint32_t algorithm)
{
  switch (algorithm)
    {
    case TPM2_ALG_SHA256:
      return "SHA-256";
    case TPM2_ALG_RSA:
      return "RSA";
    default:
      return "unknown";
    }
}

static const char *
get_command_name (uint32_t command)
{
  switch (command)
    {
    case TPM2_CC_CreatePrimary:
      return "CreatePrimary";
    case TPM2_CC_StartAuthSession:
      return "StartAuthSession";
    case TPM2_CC_FlushContext:
      return "FlushContext";
    case TPM2_CC_PCR_Read:
      return "PCR_Read";
    case TPM2_CC_PolicyPCR:
      return "PolicyPCR";
    case TPM2_CC_PolicyGetDigest:
      return "PolicyGetDigest";
    case TPM2_CC_CreateLoaded:
      return "CreateLoaded";
    case TPM2_CC_ContextSave:
      return "ContextSave";
    case TPM2_CC_ContextLoad:
      return "ContextLoad";
    case TPM2_CC_Unseal:
      return "Unseal";
    default:
      return "unkown";
    }
}

gboolean
grd_tpm_check_capabilities (GrdTpm  *tpm,
                            GError **error)
{
  const struct
  {
    TPM2_CAP capability;
    uint32_t property;
    int count;
  } capabilities[] = {
    { .capability = TPM2_CAP_ALGS, .property = TPM2_ALG_SHA256 },
    { .capability = TPM2_CAP_ALGS, .property = TPM2_ALG_RSA },
    { .capability = TPM2_CAP_COMMANDS, .property = TPM2_CC_CreatePrimary },
    { .capability = TPM2_CAP_COMMANDS, .property = TPM2_CC_StartAuthSession },
    { .capability = TPM2_CAP_COMMANDS, .property = TPM2_CC_FlushContext },
    { .capability = TPM2_CAP_COMMANDS, .property = TPM2_CC_PCR_Read },
    { .capability = TPM2_CAP_COMMANDS, .property = TPM2_CC_PolicyPCR },
    { .capability = TPM2_CAP_COMMANDS, .property = TPM2_CC_PolicyGetDigest },
    { .capability = TPM2_CAP_COMMANDS, .property = TPM2_CC_CreateLoaded },
    { .capability = TPM2_CAP_COMMANDS, .property = TPM2_CC_ContextSave },
    { .capability = TPM2_CAP_COMMANDS, .property = TPM2_CC_ContextLoad },
  };
  gboolean sha256_pcrs_found = FALSE;
  uint32_t property = 0;
  uint32_t count = TPM2_MAX_TPM_PROPERTIES;
  int i;

  for (i = 0; i < G_N_ELEMENTS (capabilities); i++)
    {
      g_autofree TPMS_CAPABILITY_DATA *data = NULL;
      TSS2_RC rc;

      rc = Esys_GetCapability (tpm->esys_context,
                               ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                               capabilities[i].capability,
                               capabilities[i].property,
                               1,
                               NULL,
                               &data);
      if (rc != TSS2_RC_SUCCESS)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Failed to get capability: %s",
                       Tss2_RC_Decode (rc));
          return FALSE;
        }

      switch (capabilities[i].capability)
        {
        case TPM2_CAP_ALGS:
          g_warn_if_fail (data->data.algorithms.count == 1);

          if (capabilities[i].property !=
              data->data.algorithms.algProperties[0].alg)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Algorithm %s not supported",
                           get_algorithm_name (capabilities[i].property));
              return FALSE;
            }

          break;

        case TPM2_CAP_COMMANDS:
          g_warn_if_fail (data->data.command.count == 1);

          if ((data->data.command.commandAttributes[0] &
               TPMA_CC_COMMANDINDEX_MASK) !=
              capabilities[i].property)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Command %s not supported",
                           get_command_name (capabilities[i].property));
              return FALSE;
            }

          break;

        case TPM2_CAP_PCRS:
          g_warn_if_fail (data->data.assignedPCR.count == 1);
          break;
        }
    }

  while (TRUE)
    {
      TPMI_YES_NO more_data = TPM2_NO;
      g_autofree TPMS_CAPABILITY_DATA *data = NULL;
      TSS2_RC rc;

      rc = Esys_GetCapability (tpm->esys_context,
                               ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                               TPM2_CAP_PCRS,
                               property,
                               count,
                               &more_data,
                               &data);
      if (rc != TSS2_RC_SUCCESS)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Failed to get capability: %s",
                       Tss2_RC_Decode (rc));
          return FALSE;
        }

      property += data->data.assignedPCR.count;
      count -= data->data.assignedPCR.count;

      for (i = 0; i < data->data.assignedPCR.count; i++)
        {
          if (data->data.assignedPCR.pcrSelections[i].hash != TPM2_ALG_SHA256)
            continue;

          if (data->data.assignedPCR.pcrSelections[i].pcrSelect[0] &
              (1 << 0 | 1 << 1 | 1 << 2))
            {
              sha256_pcrs_found = TRUE;
              break;
            }
        }

      if (more_data == TPM2_NO)
        break;
    }

  if (!sha256_pcrs_found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "SHA-256 PCRs not available");
      return FALSE;
    }

  return TRUE;
}

GrdTpm *
grd_tpm_new (GrdTpmMode   mode,
             GError     **error)
{
  g_autoptr (GrdTpm) tpm = NULL;

  tpm = g_object_new (GRD_TYPE_TPM, NULL);

  if (!(grd_get_debug_flags () & GRD_DEBUG_TPM))
    g_setenv ("TSS2_LOGFILE", "/dev/null", TRUE);

  if (!check_tpm_existence (error))
    return NULL;

  if (!init_transmission_interface (tpm, error))
    return NULL;

  if (!init_tss2_esys (tpm, error))
    return NULL;

  switch (mode)
    {
    case GRD_TPM_MODE_NONE:
      break;
    case GRD_TPM_MODE_WRITE:
      if (!start_hmac_session (tpm, error))
        return NULL;
      if (!start_trial_session (tpm, error))
        return NULL;
      break;
    case GRD_TPM_MODE_READ:
      if (!start_hmac_session (tpm, error))
        return NULL;
      if (!start_policy_session (tpm, error))
        return NULL;
      break;
    }

  return g_steal_pointer (&tpm);
}

static void
grd_tpm_finalize (GObject *object)
{
  GrdTpm *tpm = GRD_TPM (object);

  if (tpm->policy_session)
    Esys_FlushContext (tpm->esys_context, tpm->policy_session);
  if (tpm->trial_session)
    Esys_FlushContext (tpm->esys_context, tpm->trial_session);
  if (tpm->hmac_session)
    Esys_FlushContext (tpm->esys_context, tpm->hmac_session);
  if (tpm->esys_context)
    Esys_Finalize (&tpm->esys_context);
  if (tpm->tcti_context)
    Tss2_TctiLdr_Finalize (&tpm->tcti_context);

  G_OBJECT_CLASS (grd_tpm_parent_class)->finalize (object);
}

static void
grd_tpm_init (GrdTpm *tpm)
{
}

static void
grd_tpm_class_init (GrdTpmClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_tpm_finalize;
}
