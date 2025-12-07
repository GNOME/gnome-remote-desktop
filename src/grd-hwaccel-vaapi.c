/*
 * Copyright (C) 2022 Pascal Nowack
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
 */

#include "config.h"

#include "grd-hwaccel-vaapi.h"

#include <inttypes.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <va/va.h>
#include <va/va_drm.h>

#include "grd-encode-session-vaapi.h"
#include "grd-vk-device.h"

struct _GrdHwAccelVaapi
{
  GObject parent;

  GrdVkDevice *vk_device;

  int drm_fd;
  VADisplay va_display;

  gboolean supports_quality_level;
};

G_DEFINE_TYPE (GrdHwAccelVaapi, grd_hwaccel_vaapi, G_TYPE_OBJECT)

GrdEncodeSession *
grd_hwaccel_vaapi_create_encode_session (GrdHwAccelVaapi  *hwaccel_vaapi,
                                         uint32_t          source_width,
                                         uint32_t          source_height,
                                         uint32_t          refresh_rate,
                                         GError          **error)
{
  return (GrdEncodeSession *) grd_encode_session_vaapi_new (hwaccel_vaapi->vk_device,
                                                            hwaccel_vaapi->va_display,
                                                            hwaccel_vaapi->supports_quality_level,
                                                            source_width, source_height,
                                                            refresh_rate, error);
}

static gboolean
initialize_vaapi (GrdHwAccelVaapi  *hwaccel_vaapi,
                  int              *version_major,
                  int              *version_minor,
                  int64_t           render_minor,
                  GError          **error)
{
  g_autofree char *render_node = NULL;
  VAStatus va_status;

  render_node = g_strdup_printf ("/dev/dri/renderD%" PRIi64, render_minor);

  hwaccel_vaapi->drm_fd = g_open (render_node, O_RDWR | O_CLOEXEC, 600);
  if (hwaccel_vaapi->drm_fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to open render node: %s", g_strerror (errno));
      return FALSE;
    }

  hwaccel_vaapi->va_display = vaGetDisplayDRM (hwaccel_vaapi->drm_fd);
  if (!hwaccel_vaapi->va_display)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to get VA display from DRM fd");
      return FALSE;
    }

  va_status = vaInitialize (hwaccel_vaapi->va_display,
                            version_major, version_minor);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to initialize VA display: %s", vaErrorStr (va_status));
      return FALSE;
    }

  return TRUE;
}

static gboolean
has_device_avc_profile (VAProfile *profiles,
                        int        n_profiles)
{
  int i;

  for (i = 0; i < n_profiles; ++i)
    {
      if (profiles[i] == VAProfileH264High)
        return TRUE;
    }

  return FALSE;
}

static gboolean
has_device_avc_enc_entrypoint (VAEntrypoint *entrypoints,
                               int           n_entrypoints)
{
  int i;

  for (i = 0; i < n_entrypoints; ++i)
    {
      if (entrypoints[i] == VAEntrypointEncSlice)
        return TRUE;
    }

  return FALSE;
}

static gboolean
check_device_capabilities (GrdHwAccelVaapi  *hwaccel_vaapi,
                           GError          **error)
{
  g_autofree VAProfile *profiles = NULL;
  g_autofree VAEntrypoint *entrypoints = NULL;
  VAConfigAttrib attributes[] =
  {
    { .type = VAConfigAttribRTFormat },
    { .type = VAConfigAttribRateControl },
    { .type = VAConfigAttribEncMaxRefFrames },
    { .type = VAConfigAttribEncPackedHeaders },
    { .type = VAConfigAttribEncQualityRange },
  };
  int max_profiles;
  int max_num_entrypoints;
  int n_profiles = 0;
  int n_entrypoints = 0;
  uint32_t max_ref_l0;
  VAStatus va_status;

  max_profiles = vaMaxNumProfiles (hwaccel_vaapi->va_display);
  if (max_profiles < 1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid max num profiles: %i", max_profiles);
      return FALSE;
    }
  profiles = g_new0 (VAProfile, max_profiles);

  va_status = vaQueryConfigProfiles (hwaccel_vaapi->va_display,
                                     profiles, &n_profiles);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to query config profiles: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }

  if (!has_device_avc_profile (profiles, n_profiles))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unsuitable device, missing required AVC profile");
      return FALSE;
    }

  max_num_entrypoints = vaMaxNumEntrypoints (hwaccel_vaapi->va_display);
  if (max_num_entrypoints < 1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid max num entrypoints: %i", max_num_entrypoints);
      return FALSE;
    }
  entrypoints = g_new0 (VAEntrypoint, max_num_entrypoints);

  va_status = vaQueryConfigEntrypoints (hwaccel_vaapi->va_display,
                                        VAProfileH264High,
                                        entrypoints, &n_entrypoints);
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to query config entrypoints: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }

  if (!has_device_avc_enc_entrypoint (entrypoints, n_entrypoints))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unsuitable device, missing required AVC encoding "
                   "entrypoint");
      return FALSE;
    }

  va_status = vaGetConfigAttributes (hwaccel_vaapi->va_display,
                                     VAProfileH264High, VAEntrypointEncSlice,
                                     attributes, G_N_ELEMENTS (attributes));
  if (va_status != VA_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to get config attributes: %s",
                   vaErrorStr (va_status));
      return FALSE;
    }

  g_assert (attributes[0].type == VAConfigAttribRTFormat);
  if (attributes[0].value == VA_ATTRIB_NOT_SUPPORTED ||
      !(attributes[0].value & VA_RT_FORMAT_YUV420))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unsuitable device, device does not support YUV420 format");
      return FALSE;
    }

  g_assert (attributes[1].type == VAConfigAttribRateControl);
  if (attributes[1].value == VA_ATTRIB_NOT_SUPPORTED ||
      !(attributes[1].value & VA_RC_CQP))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unsuitable device, device does not support CQP");
      return FALSE;
    }

  g_assert (attributes[2].type == VAConfigAttribEncMaxRefFrames);
  if (attributes[2].value == VA_ATTRIB_NOT_SUPPORTED)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unsuitable device, invalid max ref frames attribute: %u",
                   attributes[2].value);
      return FALSE;
    }

  max_ref_l0 = attributes[2].value & 0xFFFF;
  if (max_ref_l0 < 1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unsuitable device, device cannot handle reference frames");
      return FALSE;
    }

  g_assert (attributes[3].type == VAConfigAttribEncPackedHeaders);
  if (attributes[3].value == VA_ATTRIB_NOT_SUPPORTED ||
      !(attributes[3].value & VA_ENC_PACKED_HEADER_SEQUENCE) ||
      !(attributes[3].value & VA_ENC_PACKED_HEADER_PICTURE) ||
      !(attributes[3].value & VA_ENC_PACKED_HEADER_SLICE) ||
      !(attributes[3].value & VA_ENC_PACKED_HEADER_RAW_DATA))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unsuitable device, device does not support required packed "
                   "headers (only supports 0x%08X)", attributes[3].value);
      return FALSE;
    }

  g_assert (attributes[4].type == VAConfigAttribEncQualityRange);
  hwaccel_vaapi->supports_quality_level = attributes[4].value !=
                                          VA_ATTRIB_NOT_SUPPORTED;

  return TRUE;
}

GrdHwAccelVaapi *
grd_hwaccel_vaapi_new (GrdVkDevice  *vk_device,
                       GError      **error)
{
  g_autoptr (GrdHwAccelVaapi) hwaccel_vaapi = NULL;
  int version_major = 0;
  int version_minor = 0;

  g_assert (vk_device);

  hwaccel_vaapi = g_object_new (GRD_TYPE_HWACCEL_VAAPI, NULL);
  hwaccel_vaapi->vk_device = vk_device;

  if (!initialize_vaapi (hwaccel_vaapi, &version_major, &version_minor,
                         grd_vk_device_get_drm_render_node (vk_device),
                         error))
    return NULL;

  if (!check_device_capabilities (hwaccel_vaapi, error))
    return NULL;

  g_message ("[HWAccel.VAAPI] Successfully initialized VAAPI %i.%i with "
             "vendor: %s", version_major, version_minor,
             vaQueryVendorString (hwaccel_vaapi->va_display));

  return g_steal_pointer (&hwaccel_vaapi);
}

static void
grd_hwaccel_vaapi_dispose (GObject *object)
{
  GrdHwAccelVaapi *hwaccel_vaapi = GRD_HWACCEL_VAAPI (object);

  g_clear_pointer (&hwaccel_vaapi->va_display, vaTerminate);
  g_clear_fd (&hwaccel_vaapi->drm_fd, NULL);

  G_OBJECT_CLASS (grd_hwaccel_vaapi_parent_class)->dispose (object);
}

static void
grd_hwaccel_vaapi_init (GrdHwAccelVaapi *hwaccel_vaapi)
{
}

static void
grd_hwaccel_vaapi_class_init (GrdHwAccelVaapiClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_hwaccel_vaapi_dispose;
}
