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

#include "grd-rdp-dvc-camera-enumerator.h"

#include <freerdp/server/rdpecam-enumerator.h>

#include "grd-rdp-dvc-camera-device.h"

#define SERVER_VERSION 2
#define MAX_DVC_NAME_LEN 256

typedef struct _DeviceInfo
{
  char *device_name;
  char *dvc_name;
} DeviceInfo;

struct _GrdRdpDvcCameraEnumerator
{
  GrdRdpDvc parent;

  CamDevEnumServerContext *enumerator_context;
  gboolean channel_opened;

  GrdRdpDvcHandler *dvc_handler;

  uint8_t protocol_version;
  gboolean initialized;

  GHashTable *device_table;

  GSource *device_source;
  GMutex device_queue_mutex;
  GHashTable *devices_to_add;
  GHashTable *devices_to_remove;
};

G_DEFINE_TYPE (GrdRdpDvcCameraEnumerator, grd_rdp_dvc_camera_enumerator,
               GRD_TYPE_RDP_DVC)

static void
grd_rdp_dvc_camera_enumerator_maybe_init (GrdRdpDvc *dvc)
{
  GrdRdpDvcCameraEnumerator *enumerator = GRD_RDP_DVC_CAMERA_ENUMERATOR (dvc);
  CamDevEnumServerContext *enumerator_context;

  if (enumerator->channel_opened)
    return;

  enumerator_context = enumerator->enumerator_context;
  if (enumerator_context->Open (enumerator_context))
    {
      g_warning ("[RDP.CAM_ENUMERATOR] Failed to open channel. "
                 "Terminating protocol");
      grd_rdp_dvc_queue_channel_tear_down (GRD_RDP_DVC (enumerator));
      return;
    }
  enumerator->channel_opened = TRUE;
}

static void
dvc_creation_status_cb (gpointer user_data,
                        int32_t  creation_status)
{
  GrdRdpDvcCameraEnumerator *enumerator = user_data;

  if (creation_status < 0)
    {
      g_debug ("[RDP.CAM_ENUMERATOR] Failed to open channel "
               "(CreationStatus %i). Terminating protocol", creation_status);
      grd_rdp_dvc_queue_channel_tear_down (GRD_RDP_DVC (enumerator));
    }
}

static BOOL
enumerator_channel_id_assigned (CamDevEnumServerContext *enumerator_context,
                                uint32_t                 channel_id)
{
  GrdRdpDvcCameraEnumerator *enumerator = enumerator_context->userdata;
  GrdRdpDvc *dvc = GRD_RDP_DVC (enumerator);

  g_debug ("[RDP.CAM_ENUMERATOR] DVC channel id assigned to id %u", channel_id);

  grd_rdp_dvc_subscribe_creation_status (dvc, channel_id,
                                         dvc_creation_status_cb,
                                         enumerator);

  return TRUE;
}

static uint32_t
enumerator_select_version_request (CamDevEnumServerContext          *enumerator_context,
                                   const CAM_SELECT_VERSION_REQUEST *select_version_request)
{
  GrdRdpDvcCameraEnumerator *enumerator = enumerator_context->userdata;
  CAM_SELECT_VERSION_RESPONSE select_version_response = {};

  if (enumerator->initialized)
    {
      g_warning ("[RDP.CAM_ENUMERATOR] Protocol violation: Received version "
                 "request, but protocol is already initialized");
      grd_rdp_dvc_queue_channel_tear_down (GRD_RDP_DVC (enumerator));
      return CHANNEL_RC_INITIALIZATION_ERROR;
    }

  g_debug ("[RDP.CAM_ENUMERATOR] Client supports protocol version %u",
           select_version_request->Header.Version);
  enumerator->protocol_version = MIN (SERVER_VERSION,
                                      select_version_request->Header.Version);
  enumerator->initialized = TRUE;

  select_version_response.Header.Version = enumerator->protocol_version;
  select_version_response.Header.MessageId = CAM_MSG_ID_SelectVersionResponse;

  return enumerator_context->SelectVersionResponse (enumerator_context,
                                                    &select_version_response);
}

static const char *reserved_svc_names[] =
{
  "encomsp", /* [MS-RDPEMC] */
  "CLIPRDR", /* [MS-RDPECLIP] */
  "DRDYNVC", /* [MS-RDPEDYC] */
  "RDPDR", /* [MS-RDPEFS] */
  "RDPSND", /* [MS-RDPEA] */
  "RAIL", /* [MS-RDPERP] */
};

static const char *reserved_dvc_names[] =
{
  "AUDIO_INPUT", /* [MS-RDPEAI] */
  "AUDIO_PLAYBACK_DVC", /* [MS-RDPEA] */
  "AUDIO_PLAYBACK_LOSSY_DVC", /* [MS-RDPEA] */
  "dwmprox", /* [MS-RDPCR2] */
  "ECHO", /* [MS-RDPEECO] */
  "FileRedirectorChannel", /* [MS-RDPEPNP] */
  "Microsoft::Windows::RDS::AuthRedirection", /* [MS-RDPEAR] */
  "Microsoft::Windows::RDS::CoreInput", /* [MS-RDPECI] */
  "Microsoft::Windows::RDS::DisplayControl", /* [MS-RDPEDISP] */
  "Microsoft::Windows::RDS::Geometry::v08.01", /* [MS-RDPEGT] */
  "Microsoft::Windows::RDS::Graphics", /* [MS-RDPEGFX] */
  "Microsoft::Windows::RDS::Input", /* [MS-RDPEI] */
  "Microsoft::Windows::RDS::Location", /* [MS-RDPEL] */
  "Microsoft::Windows::RDS::MouseCursor", /* [MS-RDPEMSC] */
  "Microsoft::Windows::RDS::Telemetry", /* [MS-RDPET] */
  "Microsoft::Windows::RDS::Video::Control::v08.01", /* [MS-RDPEVOR] */
  "Microsoft::Windows::RDS::Video::Data::v08.01", /* [MS-RDPEVOR] */
  "PNPDR", /* [MS-RDPEPNP] */
  "RDCamera_Device_Enumerator", /* [MS-RDPECAM] */
  "TextInput_ServerToClientDVC", /* [MS-RDPETXT] */
  "TextInput_ClientToServerDVC", /* [MS-RDPETXT] */
  "TSMF", /* [MS-RDPEV] */
  "TSVCTKT", /* [MS-RDPEXPS] */
  "URBDRC", /* [MS-RDPEUSB] */
  "WebAuthN_Channel", /* [MS-RDPEWA] */
  "WMSAud", /* [MS-RDPADRV] */
  "WMSDL", /* [MS-RDPADRV] */
  "XPSRD", /* [MS-RDPEXPS] */
};

static gboolean
is_vc_name_reserved (const char *dvc_name)
{
  size_t i;

  for (i = 0; i < G_N_ELEMENTS (reserved_svc_names); ++i)
    {
      if (strcmp (dvc_name, reserved_svc_names[i]) == 0)
        return TRUE;
    }
  for (i = 0; i < G_N_ELEMENTS (reserved_dvc_names); ++i)
    {
      if (strcmp (dvc_name, reserved_dvc_names[i]) == 0)
        return TRUE;
    }

  return FALSE;
}

static uint32_t
enumerator_device_added_notification (CamDevEnumServerContext             *enumerator_context,
                                      const CAM_DEVICE_ADDED_NOTIFICATION *device_added_notification)
{
  GrdRdpDvcCameraEnumerator *enumerator = enumerator_context->userdata;
  g_autofree char *device_name = NULL;
  DeviceInfo *info;

  if (!enumerator->initialized)
    {
      g_warning ("[RDP.CAM_ENUMERATOR] Protocol violation: Received device "
                 "added notification, but no protocol version was negotiated");
      grd_rdp_dvc_queue_channel_tear_down (GRD_RDP_DVC (enumerator));
      return CHANNEL_RC_INITIALIZATION_ERROR;
    }

  /*
   * The name of a device channel MUST be a null-terminated ANSI encoded
   * character string containing a maximum of 256 characters.
   */
  if (strlen (device_added_notification->VirtualChannelName) > MAX_DVC_NAME_LEN)
    {
      g_warning ("[RDP.CAM_ENUMERATOR] Protocol violation: Client tried to use "
                 "too long DVC name. Ignoring announced camera device");
      return CHANNEL_RC_OK;
    }
  if (is_vc_name_reserved (device_added_notification->VirtualChannelName))
    {
      g_warning ("[RDP.CAM_ENUMERATOR] Client tried to use reserved DVC name "
                 "\"%s\". Ignoring announced camera device",
                 device_added_notification->VirtualChannelName);
      return CHANNEL_RC_OK;
    }

  device_name = ConvertWCharToUtf8Alloc (device_added_notification->DeviceName,
                                         NULL);
  if (!device_name)
    {
      g_warning ("[RDP.CAM_ENUMERATOR] Failed to convert device name. "
                 "Ignoring announced camera device");
      return CHANNEL_RC_OK;
    }

  g_debug ("[RDP.CAM_ENUMERATOR] Client announced camera device: "
           "DeviceName: \"%s\", DVC name: \"%s\"",
           device_name, device_added_notification->VirtualChannelName);

  info = g_new0 (DeviceInfo, 1);
  info->device_name = g_strdup (device_name);
  info->dvc_name = g_strdup (device_added_notification->VirtualChannelName);

  g_mutex_lock (&enumerator->device_queue_mutex);
  g_hash_table_remove (enumerator->devices_to_remove, info->dvc_name);
  g_hash_table_insert (enumerator->devices_to_add, info->dvc_name, info);
  g_mutex_unlock (&enumerator->device_queue_mutex);

  g_source_set_ready_time (enumerator->device_source, 0);

  return CHANNEL_RC_OK;
}

static uint32_t
enumerator_device_removed_notification (CamDevEnumServerContext               *enumerator_context,
                                        const CAM_DEVICE_REMOVED_NOTIFICATION *device_removed_notification)
{
  GrdRdpDvcCameraEnumerator *enumerator = enumerator_context->userdata;
  DeviceInfo *info;

  if (!enumerator->initialized)
    {
      g_warning ("[RDP.CAM_ENUMERATOR] Protocol violation: Received device "
                 "removed notification, but no protocol version was negotiated");
      grd_rdp_dvc_queue_channel_tear_down (GRD_RDP_DVC (enumerator));
      return CHANNEL_RC_INITIALIZATION_ERROR;
    }

  /*
   * The name of a device channel MUST be a null-terminated ANSI encoded
   * character string containing a maximum of 256 characters.
   */
  if (strlen (device_removed_notification->VirtualChannelName) > MAX_DVC_NAME_LEN)
    {
      g_warning ("[RDP.CAM_ENUMERATOR] Protocol violation: Client tried to use "
                 "too long DVC name. Ignoring announced camera device");
      return CHANNEL_RC_OK;
    }

  g_debug ("[RDP.CAM_ENUMERATOR] Client removed camera device "
           "(channel name: \"%s\")",
           device_removed_notification->VirtualChannelName);

  info = g_new0 (DeviceInfo, 1);
  info->dvc_name = g_strdup (device_removed_notification->VirtualChannelName);

  g_mutex_lock (&enumerator->device_queue_mutex);
  g_hash_table_remove (enumerator->devices_to_add, info->dvc_name);
  g_hash_table_insert (enumerator->devices_to_remove, info->dvc_name, info);
  g_mutex_unlock (&enumerator->device_queue_mutex);

  g_source_set_ready_time (enumerator->device_source, 0);

  return CHANNEL_RC_OK;
}

GrdRdpDvcCameraEnumerator *
grd_rdp_dvc_camera_enumerator_new (GrdSessionRdp    *session_rdp,
                                   GrdRdpDvcHandler *dvc_handler,
                                   HANDLE            vcm,
                                   rdpContext       *rdp_context)
{
  GrdRdpDvcCameraEnumerator *enumerator;
  CamDevEnumServerContext *enumerator_context;

  enumerator = g_object_new (GRD_TYPE_RDP_DVC_CAMERA_ENUMERATOR, NULL);
  enumerator_context = cam_dev_enum_server_context_new (vcm);
  if (!enumerator_context)
    g_error ("[RDP.CAM_ENUMERATOR] Failed to allocate server context (OOM)");

  enumerator->enumerator_context = enumerator_context;
  enumerator->dvc_handler = dvc_handler;

  grd_rdp_dvc_initialize_base (GRD_RDP_DVC (enumerator),
                               dvc_handler, session_rdp,
                               GRD_RDP_CHANNEL_CAMERA_ENUMERATOR);

  enumerator_context->ChannelIdAssigned = enumerator_channel_id_assigned;
  enumerator_context->SelectVersionRequest = enumerator_select_version_request;
  enumerator_context->DeviceAddedNotification = enumerator_device_added_notification;
  enumerator_context->DeviceRemovedNotification = enumerator_device_removed_notification;
  enumerator_context->rdpcontext = rdp_context;
  enumerator_context->userdata = enumerator;

  return enumerator;
}

static void
grd_rdp_dvc_camera_enumerator_dispose (GObject *object)
{
  GrdRdpDvcCameraEnumerator *enumerator =
    GRD_RDP_DVC_CAMERA_ENUMERATOR (object);
  GrdRdpDvc *dvc = GRD_RDP_DVC (enumerator);

  g_hash_table_remove_all (enumerator->device_table);

  if (enumerator->channel_opened)
    {
      enumerator->enumerator_context->Close (enumerator->enumerator_context);
      enumerator->channel_opened = FALSE;
    }
  grd_rdp_dvc_maybe_unsubscribe_creation_status (dvc);

  if (enumerator->device_source)
    {
      g_source_destroy (enumerator->device_source);
      g_clear_pointer (&enumerator->device_source,  g_source_unref);
    }

  g_clear_pointer (&enumerator->enumerator_context,
                   cam_dev_enum_server_context_free);

  G_OBJECT_CLASS (grd_rdp_dvc_camera_enumerator_parent_class)->dispose (object);
}

static void
grd_rdp_dvc_camera_enumerator_finalize (GObject *object)
{
  GrdRdpDvcCameraEnumerator *enumerator =
    GRD_RDP_DVC_CAMERA_ENUMERATOR (object);

  g_mutex_clear (&enumerator->device_queue_mutex);

  g_clear_pointer (&enumerator->devices_to_remove, g_hash_table_unref);
  g_clear_pointer (&enumerator->devices_to_add, g_hash_table_unref);

  g_assert (g_hash_table_size (enumerator->device_table) == 0);
  g_clear_pointer (&enumerator->device_table, g_hash_table_unref);

  G_OBJECT_CLASS (grd_rdp_dvc_camera_enumerator_parent_class)->finalize (object);
}

static void
device_info_free (gpointer data)
{
  DeviceInfo *info = data;

  g_clear_pointer (&info->dvc_name, g_free);
  g_clear_pointer (&info->device_name, g_free);

  g_free (info);
}

static gboolean
remove_camera_device (gpointer key,
                      gpointer value,
                      gpointer user_data)
{
  GrdRdpDvcCameraEnumerator *enumerator = user_data;
  DeviceInfo *info = value;

  g_hash_table_remove (enumerator->device_table, info->dvc_name);

  return TRUE;
}

static void
on_device_error (GrdRdpDvcCameraDevice     *device,
                 GrdRdpDvcCameraEnumerator *enumerator)
{
  DeviceInfo *info;

  /* Fake a Device Removed Notification on error */
  info = g_new0 (DeviceInfo, 1);
  info->dvc_name = g_strdup (grd_rdp_dvc_camera_device_get_dvc_name (device));

  g_mutex_lock (&enumerator->device_queue_mutex);
  g_hash_table_remove (enumerator->devices_to_add, info->dvc_name);
  g_hash_table_insert (enumerator->devices_to_remove, info->dvc_name, info);
  g_mutex_unlock (&enumerator->device_queue_mutex);

  g_source_set_ready_time (enumerator->device_source, 0);
}

static gboolean
add_camera_device (gpointer key,
                   gpointer value,
                   gpointer user_data)
{
  GrdRdpDvcCameraEnumerator *enumerator = user_data;
  CamDevEnumServerContext *enumerator_context = enumerator->enumerator_context;
  DeviceInfo *info = value;
  GrdRdpDvcCameraDevice *device;

  /* Camera may be re-added by client */
  g_hash_table_remove (enumerator->device_table, info->dvc_name);

  device = grd_rdp_dvc_camera_device_new (enumerator->dvc_handler,
                                          enumerator_context->vcm,
                                          enumerator_context->rdpcontext,
                                          enumerator->protocol_version,
                                          info->dvc_name,
                                          info->device_name);
  if (!device)
    return TRUE;

  g_signal_connect (device, "error",
                    G_CALLBACK (on_device_error),
                    enumerator);

  g_hash_table_insert (enumerator->device_table,
                       g_steal_pointer (&info->dvc_name), device);

  return TRUE;
}

static gboolean
manage_devices (gpointer user_data)
{
  GrdRdpDvcCameraEnumerator *enumerator = user_data;

  g_mutex_lock (&enumerator->device_queue_mutex);
  g_hash_table_foreach_remove (enumerator->devices_to_remove,
                               remove_camera_device, enumerator);
  g_hash_table_foreach_remove (enumerator->devices_to_add,
                               add_camera_device, enumerator);
  g_mutex_unlock (&enumerator->device_queue_mutex);

  return G_SOURCE_CONTINUE;
}

static gboolean
device_source_dispatch (GSource     *source,
                        GSourceFunc  callback,
                        gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs device_source_funcs =
{
  .dispatch = device_source_dispatch,
};

static void
grd_rdp_dvc_camera_enumerator_init (GrdRdpDvcCameraEnumerator *enumerator)
{
  GSource *device_source;

  enumerator->device_table =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free, g_object_unref);
  enumerator->devices_to_add =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           NULL, device_info_free);
  enumerator->devices_to_remove =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           NULL, device_info_free);

  g_mutex_init (&enumerator->device_queue_mutex);

  device_source = g_source_new (&device_source_funcs, sizeof (GSource));
  g_source_set_callback (device_source, manage_devices, enumerator, NULL);
  g_source_set_ready_time (device_source, -1);
  g_source_attach (device_source, NULL);
  enumerator->device_source = device_source;
}

static void
grd_rdp_dvc_camera_enumerator_class_init (GrdRdpDvcCameraEnumeratorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdRdpDvcClass *dvc_class = GRD_RDP_DVC_CLASS (klass);

  object_class->dispose = grd_rdp_dvc_camera_enumerator_dispose;
  object_class->finalize = grd_rdp_dvc_camera_enumerator_finalize;

  dvc_class->maybe_init = grd_rdp_dvc_camera_enumerator_maybe_init;
}
