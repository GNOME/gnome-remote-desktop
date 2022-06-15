/*
 * Copyright (C) 2025 Pascal Nowack
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

#include "grd-rdp-dvc-camera-device.h"

#include "grd-pipewire-utils.h"
#include "grd-rdp-camera-stream.h"
#include "grd-sample-buffer.h"
#include "grd-utils.h"

#define SAMPLE_TIMEOUT_MS (2 * 1000)

enum
{
  ERROR,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef enum
{
  DEVICE_STATE_FATAL_ERROR,
  DEVICE_STATE_PENDING_ACTIVATION,
  DEVICE_STATE_PENDING_ACTIVATION_RESPONSE,
  DEVICE_STATE_PENDING_STREAM_LIST_RESPONSE,
  DEVICE_STATE_PENDING_MEDIA_TYPE_LIST_RESPONSE,
  DEVICE_STATE_PENDING_STREAM_PREPARATION,
  DEVICE_STATE_INITIALIZATION_DONE,
  DEVICE_STATE_IN_SHUTDOWN,
} DeviceState;

typedef enum
{
  DEVICE_EVENT_ACK_STARTED_STREAMS,
  DEVICE_EVENT_STOP_STREAMS,
  DEVICE_EVENT_ACK_STOPPED_STREAMS,
} DeviceEvent;

typedef enum
{
  CLIENT_REQUEST_TYPE_NONE,
  CLIENT_REQUEST_TYPE_START_STREAMS,
  CLIENT_REQUEST_TYPE_STOP_STREAMS,
} ClientRequestType;

typedef struct
{
  CAM_MEDIA_TYPE_DESCRIPTION *media_type_description;
  uint32_t run_sequence;
} StreamRunContext;

typedef struct
{
  GrdRdpDvcCameraDevice *device;

  GList *media_type_descriptions;
  GrdRdpCameraStream *camera_stream;

  GAsyncQueue *pending_samples;
} StreamContext;

struct _GrdRdpDvcCameraDevice
{
  GrdRdpDvc parent;

  CameraDeviceServerContext *device_context;
  gboolean channel_opened;

  char *dvc_name;
  char *device_name;

  GThread *camera_thread;
  GMainContext *camera_context;
  gboolean in_shutdown;

  GrdSyncPoint sync_point;

  GSource *initialization_source;
  GMutex state_mutex;
  DeviceState state;
  GQueue *pending_media_type_lists;

  GHashTable *stream_contexts;

  GSource *pipewire_source;

  struct pw_context *pipewire_context;
  struct pw_core *pipewire_core;
  struct spa_hook pipewire_core_listener;

  GHashTable *queued_stream_starts;
  GHashTable *pending_stream_starts;
  GHashTable *running_streams;

  GSource *event_source;

  GMutex event_mutex;
  GHashTable *pending_events;

  GMutex client_request_mutex;
  gboolean pending_client_request;
  ClientRequestType pending_client_request_type;

  GMutex sample_request_mutex;
  GHashTable *pending_samples;

  GSource *sample_timeout_source;
};

G_DEFINE_TYPE (GrdRdpDvcCameraDevice, grd_rdp_dvc_camera_device,
               GRD_TYPE_RDP_DVC)

static StreamRunContext *
stream_run_context_new (CAM_MEDIA_TYPE_DESCRIPTION *media_type_description,
                        uint32_t                    run_sequence)
{
  StreamRunContext *stream_run_context;

  stream_run_context = g_new0 (StreamRunContext, 1);
  stream_run_context->media_type_description = media_type_description;
  stream_run_context->run_sequence = run_sequence;

  return stream_run_context;
}

static void
stream_run_context_free (StreamRunContext *stream_run_context)
{
  g_free (stream_run_context);
}

static StreamContext *
stream_context_new (GrdRdpDvcCameraDevice *device)
{
  StreamContext *stream_context;

  stream_context = g_new0 (StreamContext, 1);
  stream_context->device = device;

  stream_context->pending_samples = g_async_queue_new ();

  return stream_context;
}

static void
discard_pending_sample_requests (StreamContext *stream_context)
{
  GrdRdpDvcCameraDevice *device = stream_context->device;
  g_autoptr (GMutexLocker) locker = NULL;
  GrdSampleBuffer *sample_buffer;

  locker = g_mutex_locker_new (&device->state_mutex);
  while ((sample_buffer = g_async_queue_try_pop (stream_context->pending_samples)))
    {
      g_mutex_lock (&device->sample_request_mutex);
      g_hash_table_remove (device->pending_samples, sample_buffer);
      g_mutex_unlock (&device->sample_request_mutex);

      grd_rdp_camera_stream_submit_sample (stream_context->camera_stream,
                                           sample_buffer, FALSE);
    }
}

static void
stream_context_free (StreamContext *stream_context)
{
  if (stream_context->camera_stream)
    {
      GrdRdpDvcCameraDevice *device = stream_context->device;
      uint8_t stream_index =
        grd_rdp_camera_stream_get_stream_index (stream_context->camera_stream);

      discard_pending_sample_requests (stream_context);

      g_hash_table_remove (device->queued_stream_starts,
                           GUINT_TO_POINTER (stream_index));
      g_hash_table_remove (device->pending_stream_starts,
                           GUINT_TO_POINTER (stream_index));
      g_hash_table_remove (device->running_streams,
                           GUINT_TO_POINTER (stream_index));
    }

  g_clear_object (&stream_context->camera_stream);
  g_clear_list (&stream_context->media_type_descriptions, g_free);

  g_clear_pointer (&stream_context->pending_samples, g_async_queue_unref);

  g_free (stream_context);
}

const char *
grd_rdp_dvc_camera_device_get_dvc_name (GrdRdpDvcCameraDevice *device)
{
  return device->dvc_name;
}

void
grd_rdp_dvc_camera_device_start_stream (GrdRdpDvcCameraDevice      *device,
                                        GrdRdpCameraStream         *camera_stream,
                                        CAM_MEDIA_TYPE_DESCRIPTION *media_type_description,
                                        uint32_t                    run_sequence)
{
  uint8_t stream_index = grd_rdp_camera_stream_get_stream_index (camera_stream);

  g_hash_table_insert (device->queued_stream_starts,
                       GUINT_TO_POINTER (stream_index),
                       stream_run_context_new (media_type_description,
                                               run_sequence));

  g_source_set_ready_time (device->event_source, 0);
}

static gboolean
queue_stream_restart (gpointer key,
                      gpointer value,
                      gpointer user_data)
{
  GrdRdpDvcCameraDevice *device = user_data;
  uint8_t stream_index = GPOINTER_TO_UINT (key);
  StreamRunContext *stream_run_context = value;
  StreamContext *stream_context = NULL;

  if (!g_hash_table_lookup_extended (device->stream_contexts,
                                     GUINT_TO_POINTER (stream_index),
                                     NULL, (gpointer *) &stream_context))
    g_assert_not_reached ();

  grd_rdp_camera_stream_inhibit_camera_loop (stream_context->camera_stream);

  if (!g_hash_table_contains (device->queued_stream_starts,
                              GUINT_TO_POINTER (stream_index)))
    {
      g_hash_table_insert (device->queued_stream_starts,
                           GUINT_TO_POINTER (stream_index),
                           g_memdup2 (stream_run_context,
                                      sizeof (StreamRunContext)));
    }

  return TRUE;
}

void
grd_rdp_dvc_camera_device_stop_stream (GrdRdpDvcCameraDevice *device,
                                       GrdRdpCameraStream    *camera_stream)
{
  uint8_t stream_index = grd_rdp_camera_stream_get_stream_index (camera_stream);

  g_hash_table_remove (device->running_streams,
                       GUINT_TO_POINTER (stream_index));
  g_hash_table_remove (device->queued_stream_starts,
                       GUINT_TO_POINTER (stream_index));

  /*
   * Stopping a stream can mean one of the two things:
   * 1) The stream is stopped, because there is no consumer left
   * 2) The stream format was changed
   *
   * For case 2, all streams need to be stopped, because the [MS-RDPECAM]
   * protocol only supports stopping all streams and not stopping one individual
   * stream.
   */
  g_hash_table_foreach_remove (device->running_streams,
                               queue_stream_restart, device);

  g_mutex_lock (&device->event_mutex);
  g_hash_table_add (device->pending_events,
                    GUINT_TO_POINTER (DEVICE_EVENT_STOP_STREAMS));
  g_mutex_unlock (&device->event_mutex);

  g_source_set_ready_time (device->event_source, 0);
}

void
grd_rdp_dvc_camera_device_request_sample (GrdRdpDvcCameraDevice *device,
                                          GrdRdpCameraStream    *camera_stream,
                                          GrdSampleBuffer       *sample_buffer)
{
  CameraDeviceServerContext *device_context = device->device_context;
  uint8_t stream_index = grd_rdp_camera_stream_get_stream_index (camera_stream);
  StreamContext *stream_context = NULL;
  CAM_SAMPLE_REQUEST sample_request = {};

  g_assert (device->pending_client_request_type !=
            CLIENT_REQUEST_TYPE_STOP_STREAMS);

  if (!g_hash_table_lookup_extended (device->stream_contexts,
                                     GUINT_TO_POINTER (stream_index),
                                     NULL, (gpointer *) &stream_context))
    g_assert_not_reached ();

  g_mutex_lock (&device->sample_request_mutex);
  g_hash_table_add (device->pending_samples, sample_buffer);
  g_mutex_unlock (&device->sample_request_mutex);

  g_async_queue_push (stream_context->pending_samples, sample_buffer);

  sample_request.StreamIndex = stream_index;

  device_context->SampleRequest (device_context, &sample_request);
}

static void
dvc_creation_status_cb (gpointer user_data,
                        int32_t  creation_status)
{
  GrdRdpDvcCameraDevice *device = user_data;

  if (creation_status < 0)
    {
      g_warning ("[RDP.CAM_DEVICE] Failed to open %s channel "
                 "(CreationStatus %i, device name: \"%s\"). "
                 "Terminating protocol",
                 device->dvc_name, creation_status, device->device_name);
      g_signal_emit (device, signals[ERROR], 0);
      return;
    }

  g_message ("[RDP.CAM_DEVICE] Successfully opened %s channel "
             "(CreationStatus %i, device name: \"%s\"). ",
             device->dvc_name, creation_status, device->device_name);

  g_source_set_ready_time (device->initialization_source, 0);
}

static BOOL
device_channel_id_assigned (CameraDeviceServerContext *device_context,
                            uint32_t                   channel_id)
{
  GrdRdpDvcCameraDevice *device = device_context->userdata;
  GrdRdpDvc *dvc = GRD_RDP_DVC (device);

  g_debug ("[RDP.CAM_DEVICE] DVC channel id for channel %s assigned to id %u",
           device->dvc_name, channel_id);

  grd_rdp_dvc_subscribe_creation_status (dvc, channel_id,
                                         dvc_creation_status_cb,
                                         device);

  return TRUE;
}

static const char *
device_state_to_string (DeviceState state)
{
  switch (state)
    {
    case DEVICE_STATE_FATAL_ERROR:
      return "FATAL_ERROR";
    case DEVICE_STATE_PENDING_ACTIVATION:
      return "PENDING_ACTIVATION";
    case DEVICE_STATE_PENDING_ACTIVATION_RESPONSE:
      return "PENDING_ACTIVATION_RESPONSE";
    case DEVICE_STATE_PENDING_STREAM_LIST_RESPONSE:
      return "PENDING_STREAM_LIST_RESPONSE";
    case DEVICE_STATE_PENDING_MEDIA_TYPE_LIST_RESPONSE:
      return "PENDING_MEDIA_TYPE_LIST_RESPONSE";
    case DEVICE_STATE_PENDING_STREAM_PREPARATION:
      return "PENDING_STREAM_PREPARATION";
    case DEVICE_STATE_INITIALIZATION_DONE:
      return "INITIALIZATION_DONE";
    case DEVICE_STATE_IN_SHUTDOWN:
      return "IN_SHUTDOWN";
    }

  g_assert_not_reached ();
}

static void
transition_into_fatal_error_state (GrdRdpDvcCameraDevice *device)
{
  device->state = DEVICE_STATE_FATAL_ERROR;
  g_signal_emit (device, signals[ERROR], 0);
}

static void
request_stream_list (GrdRdpDvcCameraDevice *device)
{
  CameraDeviceServerContext *device_context = device->device_context;
  CAM_STREAM_LIST_REQUEST stream_list_request = {};

  device->state = DEVICE_STATE_PENDING_STREAM_LIST_RESPONSE;
  device_context->StreamListRequest (device_context, &stream_list_request);
}

static gboolean
try_handle_runtime_success_response (GrdRdpDvcCameraDevice  *device,
                                     GError                **error)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&device->client_request_mutex);
  if (!device->pending_client_request)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Protocol violation: "
                   "Received stray success response in state %s for device "
                   "\"%s\". There was no runtime request. Removing device...",
                   device_state_to_string (device->state), device->device_name);
      return FALSE;
    }

  g_mutex_lock (&device->event_mutex);
  switch (device->pending_client_request_type)
    {
    case CLIENT_REQUEST_TYPE_NONE:
      g_assert_not_reached ();
      break;
    case CLIENT_REQUEST_TYPE_START_STREAMS:
      g_hash_table_add (device->pending_events,
                        GUINT_TO_POINTER (DEVICE_EVENT_ACK_STARTED_STREAMS));
      break;
    case CLIENT_REQUEST_TYPE_STOP_STREAMS:
      g_hash_table_add (device->pending_events,
                        GUINT_TO_POINTER (DEVICE_EVENT_ACK_STOPPED_STREAMS));
      break;
    }
  g_mutex_unlock (&device->event_mutex);

  g_source_set_ready_time (device->event_source, 0);

  return TRUE;
}

/*
 * Actions for success:
 *
 * - Activate Device Request
 * - Deactivate Device Request
 * - Start Streams Request
 * - Stop Streams Request
 * - Set Property Value Request
 */
static uint32_t
device_success_response (CameraDeviceServerContext  *device_context,
                         const CAM_SUCCESS_RESPONSE *success_response)
{
  GrdRdpDvcCameraDevice *device = device_context->userdata;
  g_autoptr (GMutexLocker) locker = NULL;
  g_autoptr (GError) error = NULL;

  locker = g_mutex_locker_new (&device->state_mutex);
  switch (device->state)
    {
    case DEVICE_STATE_FATAL_ERROR:
    case DEVICE_STATE_IN_SHUTDOWN:
      break;
    case DEVICE_STATE_PENDING_ACTIVATION:
    case DEVICE_STATE_PENDING_STREAM_LIST_RESPONSE:
    case DEVICE_STATE_PENDING_MEDIA_TYPE_LIST_RESPONSE:
    case DEVICE_STATE_PENDING_STREAM_PREPARATION:
      g_warning ("[RDP.CAM_DEVICE] Protocol violation: Received stray success "
                 "response in state %s for device \"%s\". Removing device...",
                 device_state_to_string (device->state), device->device_name);
      transition_into_fatal_error_state (device);
      break;
    case DEVICE_STATE_PENDING_ACTIVATION_RESPONSE:
      g_debug ("[RDP.CAM_DEVICE] Activated device \"%s\"", device->device_name);
      request_stream_list (device);
      break;
    case DEVICE_STATE_INITIALIZATION_DONE:
      if (!try_handle_runtime_success_response (device, &error))
        {
          g_warning ("[RDP.CAM_DEVICE] %s", error->message);
          transition_into_fatal_error_state (device);
        }
      break;
    }

  return CHANNEL_RC_OK;
}

static const char *
error_code_to_string (CAM_ERROR_CODE error_code)
{
  switch (error_code)
    {
      case CAM_ERROR_CODE_UnexpectedError:
        return "Unexpected Error";
      case CAM_ERROR_CODE_InvalidMessage:
        return "Invalid Message";
      case CAM_ERROR_CODE_NotInitialized:
        return "Not Initialized";
      case CAM_ERROR_CODE_InvalidRequest:
        return "Invalid Request";
      case CAM_ERROR_CODE_InvalidStreamNumber:
        return "Invalid Stream Number";
      case CAM_ERROR_CODE_InvalidMediaType:
        return "Invalid MediaType";
      case CAM_ERROR_CODE_OutOfMemory:
        return "Out Of Memory";
      case CAM_ERROR_CODE_ItemNotFound:
        return "Item Not Found";
      case CAM_ERROR_CODE_SetNotFound:
        return "Set Not Found";
      case CAM_ERROR_CODE_OperationNotSupported:
        return "Operation Not Supported";
      default:
        return "Unknown Error";
    }

  g_assert_not_reached ();
}

static const char *
device_error_to_string (const CAM_ERROR_RESPONSE *error_response)
{
  return error_code_to_string (error_response->ErrorCode);
}

static const char *
sample_error_to_string (const CAM_SAMPLE_ERROR_RESPONSE *sample_error_response)
{
  return error_code_to_string (sample_error_response->ErrorCode);
}

static void
fetch_runtime_error_response (GrdRdpDvcCameraDevice     *device,
                              const CAM_ERROR_RESPONSE  *error_response,
                              GError                   **error)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&device->client_request_mutex);
  if (!device->pending_client_request)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Protocol violation: "
                   "Received stray error response \"%s\" in state %s for "
                   "device \"%s\". There was no runtime request. Removing "
                   "device...", device_error_to_string (error_response),
                   device_state_to_string (device->state), device->device_name);
      return;
    }

  switch (device->pending_client_request_type)
    {
    case CLIENT_REQUEST_TYPE_NONE:
      g_assert_not_reached ();
      break;
    case CLIENT_REQUEST_TYPE_START_STREAMS:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to start "
                   "streams on device \"%s\": %s. Removing device...",
                   device->device_name, device_error_to_string (error_response));
      break;
    case CLIENT_REQUEST_TYPE_STOP_STREAMS:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to stop "
                   "streams on device \"%s\": %s. Removing device...",
                   device->device_name, device_error_to_string (error_response));
      break;
    }
}

/*
 * Actions for failure:
 *
 * - Activate Device Request
 * - Deactivate Device Request
 * - Start Streams Request
 * - Stop Streams Request
 * - Set Property Value Request
 *
 * - Stream List Request
 * - Media Type List Request
 * - Current Media Type Request
 * - Property List Request
 * - Property Value Request
 */
static uint32_t
device_error_response (CameraDeviceServerContext *device_context,
                       const CAM_ERROR_RESPONSE  *error_response)
{
  GrdRdpDvcCameraDevice *device = device_context->userdata;
  g_autoptr (GError) error = NULL;

  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&device->state_mutex);
  switch (device->state)
    {
    case DEVICE_STATE_FATAL_ERROR:
    case DEVICE_STATE_IN_SHUTDOWN:
      break;
    case DEVICE_STATE_PENDING_ACTIVATION:
    case DEVICE_STATE_PENDING_STREAM_PREPARATION:
      g_warning ("[RDP.CAM_DEVICE] Protocol violation: Received stray error "
                 "response \"%s\" in state %s for device \"%s\". "
                 "Removing device...", device_error_to_string (error_response),
                 device_state_to_string (device->state), device->device_name);
      break;
    case DEVICE_STATE_PENDING_ACTIVATION_RESPONSE:
      g_warning ("[RDP.CAM_DEVICE] Failed to activate device \"%s\": %s."
                 "Removing device...",
                 device->device_name, device_error_to_string (error_response));
      break;
    case DEVICE_STATE_PENDING_STREAM_LIST_RESPONSE:
      g_warning ("[RDP.CAM_DEVICE] Failed to fetch stream list from device "
                 "\"%s\": %s. Removing device...",
                 device->device_name, device_error_to_string (error_response));
      break;
    case DEVICE_STATE_PENDING_MEDIA_TYPE_LIST_RESPONSE:
      g_warning ("[RDP.CAM_DEVICE] Failed to fetch media type list from device "
                 "\"%s\": %s. Removing device...",
                 device->device_name, device_error_to_string (error_response));
      break;
    case DEVICE_STATE_INITIALIZATION_DONE:
      fetch_runtime_error_response (device, error_response, &error);
      g_warning ("[RDP.CAM_DEVICE] %s", error->message);
      break;
    }

  transition_into_fatal_error_state (device);

  return CHANNEL_RC_OK;
}

static void
request_next_media_type_list (GrdRdpDvcCameraDevice *device)
{
  CameraDeviceServerContext *device_context = device->device_context;
  CAM_MEDIA_TYPE_LIST_REQUEST media_type_list_request = {};

  g_assert (g_queue_get_length (device->pending_media_type_lists) > 0);

  media_type_list_request.StreamIndex =
    GPOINTER_TO_UINT (g_queue_peek_head (device->pending_media_type_lists));

  device->state = DEVICE_STATE_PENDING_MEDIA_TYPE_LIST_RESPONSE;
  device_context->MediaTypeListRequest (device_context,
                                        &media_type_list_request);
}

static uint32_t
device_stream_list_response (CameraDeviceServerContext      *device_context,
                             const CAM_STREAM_LIST_RESPONSE *stream_list_response)
{
  GrdRdpDvcCameraDevice *device = device_context->userdata;
  g_autoptr (GMutexLocker) locker = NULL;
  uint16_t i;

  locker = g_mutex_locker_new (&device->state_mutex);
  if (device->state == DEVICE_STATE_FATAL_ERROR ||
      device->state == DEVICE_STATE_IN_SHUTDOWN)
    return CHANNEL_RC_OK;

  if (device->state != DEVICE_STATE_PENDING_STREAM_LIST_RESPONSE)
    {
      g_warning ("[RDP.CAM_DEVICE] Protocol violation: Received stray stream "
                 "list response in state %s for device \"%s\". "
                 "Removing device...",
                 device_state_to_string (device->state), device->device_name);
      transition_into_fatal_error_state (device);
      return CHANNEL_RC_OK;
    }
  if (stream_list_response->N_Descriptions < 1)
    {
      g_warning ("[RDP.CAM_DEVICE] Device \"%s\": Received invalid stream list "
                 "response. Removing device...", device->device_name);
      transition_into_fatal_error_state (device);
      return CHANNEL_RC_OK;
    }

  for (i = 0; i < stream_list_response->N_Descriptions; ++i)
    {
      g_debug ("[RDP.CAM_DEVICE] Device \"%s\": Stream %u: FrameSourceTypes: %u, "
               "StreamCategory, %u, Selected: %u, CanBeShared: %u",
               device->device_name, i,
               stream_list_response->StreamDescriptions[i].FrameSourceTypes,
               stream_list_response->StreamDescriptions[i].StreamCategory,
               stream_list_response->StreamDescriptions[i].Selected,
               stream_list_response->StreamDescriptions[i].CanBeShared);

      g_hash_table_insert (device->stream_contexts,
                           GUINT_TO_POINTER (i), stream_context_new (device));
      g_queue_push_tail (device->pending_media_type_lists,
                         GUINT_TO_POINTER (i));
    }

  request_next_media_type_list (device);

  return CHANNEL_RC_OK;
}

static void
start_preparing_streams (GrdRdpDvcCameraDevice *device)
{
  device->state = DEVICE_STATE_PENDING_STREAM_PREPARATION;
  g_source_set_ready_time (device->initialization_source, 0);
}

static uint32_t
device_media_type_list_response (CameraDeviceServerContext          *device_context,
                                 const CAM_MEDIA_TYPE_LIST_RESPONSE *media_type_list_response)
{
  GrdRdpDvcCameraDevice *device = device_context->userdata;
  g_autoptr (GMutexLocker) locker = NULL;
  StreamContext *stream_context = NULL;
  uint8_t stream_index;
  size_t i;

  locker = g_mutex_locker_new (&device->state_mutex);
  if (device->state == DEVICE_STATE_FATAL_ERROR ||
      device->state == DEVICE_STATE_IN_SHUTDOWN)
    return CHANNEL_RC_OK;

  if (device->state != DEVICE_STATE_PENDING_MEDIA_TYPE_LIST_RESPONSE)
    {
      g_warning ("[RDP.CAM_DEVICE] Protocol violation: Received stray media "
                 "type list response in state %s for device \"%s\". "
                 "Removing device...",
                 device_state_to_string (device->state), device->device_name);
      transition_into_fatal_error_state (device);
      return CHANNEL_RC_OK;
    }
  if (media_type_list_response->N_Descriptions < 1)
    {
      g_warning ("[RDP.CAM_DEVICE] Device \"%s\": Received invalid media type "
                 "list response. Removing device...", device->device_name);
      transition_into_fatal_error_state (device);
      return CHANNEL_RC_OK;
    }

  stream_index =
    GPOINTER_TO_UINT (g_queue_pop_head (device->pending_media_type_lists));
  if (!g_hash_table_lookup_extended (device->stream_contexts,
                                     GUINT_TO_POINTER (stream_index),
                                     NULL, (gpointer *) &stream_context))
    g_assert_not_reached ();

  for (i = 0; i < media_type_list_response->N_Descriptions; ++i)
    {
      CAM_MEDIA_TYPE_DESCRIPTION *media_type_description =
        &media_type_list_response->MediaTypeDescriptions[i];

      g_debug ("[RDP.CAM_DEVICE] Device \"%s\": Stream %u: MediaTypeList %zu: "
               "Format: %u, Width: %u, Height: %u, FrameRateNumerator: %u, "
               "FrameRateDenominator: %u, PixelAspectRatioNumerator: %u, "
               "PixelAspectRatioDenominator: %u, Flags: 0x%08X",
               device->device_name, stream_index, i,
               media_type_description->Format,
               media_type_description->Width,
               media_type_description->Height,
               media_type_description->FrameRateNumerator,
               media_type_description->FrameRateDenominator,
               media_type_description->PixelAspectRatioNumerator,
               media_type_description->PixelAspectRatioDenominator,
               media_type_description->Flags);

      stream_context->media_type_descriptions =
        g_list_append (stream_context->media_type_descriptions,
                       g_memdup2 (media_type_description,
                                  sizeof (CAM_MEDIA_TYPE_DESCRIPTION)));
    }

  if (g_queue_get_length (device->pending_media_type_lists) > 0)
    request_next_media_type_list (device);
  else
    start_preparing_streams (device);

  return CHANNEL_RC_OK;
}

static uint32_t
device_current_media_type_response (CameraDeviceServerContext             *device_context,
                                    const CAM_CURRENT_MEDIA_TYPE_RESPONSE *current_media_type_response)
{
  return CHANNEL_RC_OK;
}

static gboolean
has_pending_event_unlocked (GrdRdpDvcCameraDevice *device,
                            DeviceEvent            event)
{
  return g_hash_table_contains (device->pending_events,
                                GUINT_TO_POINTER (event));
}

static gboolean
has_pending_event (GrdRdpDvcCameraDevice *device,
                   DeviceEvent            event)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&device->event_mutex);
  return has_pending_event_unlocked (device, event);
}

static uint32_t
device_sample_response (CameraDeviceServerContext *device_context,
                        const CAM_SAMPLE_RESPONSE *sample_response)
{
  GrdRdpDvcCameraDevice *device = device_context->userdata;
  uint8_t stream_index = sample_response->StreamIndex;
  g_autoptr (GMutexLocker) locker = NULL;
  StreamContext *stream_context = NULL;
  g_autoptr (GError) error = NULL;
  GrdSampleBuffer *sample_buffer;
  uint32_t n_pending_samples;
  gboolean success = FALSE;

  locker = g_mutex_locker_new (&device->state_mutex);
  if (device->state == DEVICE_STATE_FATAL_ERROR ||
      device->state == DEVICE_STATE_IN_SHUTDOWN)
    return CHANNEL_RC_OK;

  if (!g_hash_table_lookup_extended (device->stream_contexts,
                                     GUINT_TO_POINTER (stream_index),
                                     NULL, (gpointer *) &stream_context))
    {
      g_warning ("[RDP.CAM_DEVICE] Protocol violation: Received sample "
                 "response for unknown stream in state %s for device \"%s\". "
                 "Removing device...",
                 device_state_to_string (device->state), device->device_name);
      transition_into_fatal_error_state (device);
      return CHANNEL_RC_OK;
    }

  sample_buffer = g_async_queue_try_pop (stream_context->pending_samples);
  if (!sample_buffer)
    {
      g_warning ("[RDP.CAM_DEVICE] Protocol violation: Received stray sample "
                 "response for stream %u on device \"%s\". Ignoring...",
                 stream_index, device->device_name);
      return CHANNEL_RC_OK;
    }

  g_mutex_lock (&device->sample_request_mutex);
  g_hash_table_remove (device->pending_samples, sample_buffer);

  n_pending_samples = g_hash_table_size (device->pending_samples);
  g_mutex_unlock (&device->sample_request_mutex);

  if (grd_rdp_camera_stream_announce_new_sample (stream_context->camera_stream,
                                                 sample_buffer))
    {
      success = grd_sample_buffer_load_sample (sample_buffer,
                                               sample_response->Sample,
                                               sample_response->SampleSize,
                                               &error);
      if (!success)
        {
          g_warning ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: Failed to "
                     "load sample: %s ", device->device_name, stream_index,
                     error->message);
        }
    }

  grd_rdp_camera_stream_submit_sample (stream_context->camera_stream,
                                       sample_buffer, success);

  if (n_pending_samples == 0 &&
      has_pending_event (device, DEVICE_EVENT_STOP_STREAMS))
    g_source_set_ready_time (device->event_source, 0);

  return CHANNEL_RC_OK;
}

static uint32_t
device_sample_error_response (CameraDeviceServerContext       *device_context,
                              const CAM_SAMPLE_ERROR_RESPONSE *sample_error_response)
{
  GrdRdpDvcCameraDevice *device = device_context->userdata;
  uint8_t stream_index = sample_error_response->StreamIndex;
  g_autoptr (GMutexLocker) locker = NULL;
  StreamContext *stream_context = NULL;
  GrdSampleBuffer *sample_buffer;
  uint32_t n_pending_samples;

  locker = g_mutex_locker_new (&device->state_mutex);
  if (device->state == DEVICE_STATE_FATAL_ERROR ||
      device->state == DEVICE_STATE_IN_SHUTDOWN)
    return CHANNEL_RC_OK;

  if (!g_hash_table_lookup_extended (device->stream_contexts,
                                     GUINT_TO_POINTER (stream_index),
                                     NULL, (gpointer *) &stream_context))
    {
      g_warning ("[RDP.CAM_DEVICE] Protocol violation: Received sample error "
                 "response \"%s\" for unknown stream in state %s for device "
                 "\"%s\". Removing device...",
                 sample_error_to_string (sample_error_response),
                 device_state_to_string (device->state), device->device_name);
      transition_into_fatal_error_state (device);
      return CHANNEL_RC_OK;
    }

  sample_buffer = g_async_queue_try_pop (stream_context->pending_samples);
  if (!sample_buffer)
    {
      g_warning ("[RDP.CAM_DEVICE] Protocol violation: Received stray sample "
                 "error response \"%s\" for stream %u on device \"%s\". "
                 "Ignoring...", sample_error_to_string (sample_error_response),
                 stream_index, device->device_name);
      return CHANNEL_RC_OK;
    }

  g_mutex_lock (&device->sample_request_mutex);
  g_hash_table_remove (device->pending_samples, sample_buffer);

  n_pending_samples = g_hash_table_size (device->pending_samples);
  g_mutex_unlock (&device->sample_request_mutex);

  g_warning ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: Failed to retrieve "
             "sample from client: %s ", device->device_name, stream_index,
             sample_error_to_string (sample_error_response));

  grd_rdp_camera_stream_submit_sample (stream_context->camera_stream,
                                       sample_buffer, FALSE);

  if (n_pending_samples == 0 &&
      has_pending_event (device, DEVICE_EVENT_STOP_STREAMS))
    g_source_set_ready_time (device->event_source, 0);

  return CHANNEL_RC_OK;
}

static uint32_t
device_property_list_response (CameraDeviceServerContext        *device_context,
                               const CAM_PROPERTY_LIST_RESPONSE *property_list_response)
{
  return CHANNEL_RC_OK;
}

static uint32_t
device_property_value_response (CameraDeviceServerContext         *device_context,
                                const CAM_PROPERTY_VALUE_RESPONSE *property_value_response)
{
  return CHANNEL_RC_OK;
}

GrdRdpDvcCameraDevice *
grd_rdp_dvc_camera_device_new (GrdRdpDvcHandler *dvc_handler,
                               HANDLE            vcm,
                               rdpContext       *rdp_context,
                               uint8_t           protocol_version,
                               const char       *dvc_name,
                               const char       *device_name)
{
  g_autoptr (GrdRdpDvcCameraDevice) device = NULL;
  CameraDeviceServerContext *device_context;

  device = g_object_new (GRD_TYPE_RDP_DVC_CAMERA_DEVICE, NULL);
  device_context = camera_device_server_context_new (vcm);
  if (!device_context)
    g_error ("[RDP.CAM_DEVICE] Failed to allocate server context (OOM)");

  device->device_context = device_context;
  device->dvc_name = g_strdup (dvc_name);
  device->device_name = g_strdup (device_name);

  grd_rdp_dvc_initialize_base (GRD_RDP_DVC (device),
                               dvc_handler, NULL,
                               GRD_RDP_CHANNEL_CAMERA);

  device_context->virtualChannelName = g_strdup (dvc_name);
  device_context->protocolVersion = protocol_version;

  device_context->ChannelIdAssigned = device_channel_id_assigned;
  device_context->SuccessResponse = device_success_response;
  device_context->ErrorResponse = device_error_response;
  device_context->StreamListResponse = device_stream_list_response;
  device_context->MediaTypeListResponse = device_media_type_list_response;
  device_context->CurrentMediaTypeResponse = device_current_media_type_response;
  device_context->SampleResponse = device_sample_response;
  device_context->SampleErrorResponse = device_sample_error_response;
  device_context->PropertyListResponse = device_property_list_response;
  device_context->PropertyValueResponse = device_property_value_response;
  device_context->rdpcontext = rdp_context;
  device_context->userdata = device;

  /* Check whether camera thread was able to successfully initialize PipeWire */
  if (!grd_sync_point_wait_for_completion (&device->sync_point))
    return NULL;

  /*
   * The DRDYNVC virtual channel is already initialized here,
   * as the enumerator channel also uses it
   */
  if (device_context->Open (device_context))
    {
      g_warning ("[RDP.CAM_DEVICE] Device \"%s\": Failed to open the %s "
                 "channel. Terminating protocol", device_name, dvc_name);
      return NULL;
    }
  device->channel_opened = TRUE;

  return g_steal_pointer (&device);
}

static gboolean
tear_down_streams (gpointer user_data)
{
  GrdRdpDvcCameraDevice *device = user_data;

  g_hash_table_remove_all (device->stream_contexts);
  if (device->sample_timeout_source)
    {
      g_source_destroy (device->sample_timeout_source);
      g_clear_pointer (&device->sample_timeout_source, g_source_unref);
    }

  grd_sync_point_complete (&device->sync_point, TRUE);

  return G_SOURCE_REMOVE;
}

static gboolean
source_dispatch (GSource     *source,
                 GSourceFunc  callback,
                 gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs source_funcs =
{
  .dispatch = source_dispatch,
};

static void
destroy_camera_streams_in_camera_thread (GrdRdpDvcCameraDevice *device)
{
  GSource *stream_teardown_source;

  g_mutex_lock (&device->state_mutex);
  device->state = DEVICE_STATE_IN_SHUTDOWN;
  g_mutex_unlock (&device->state_mutex);

  grd_sync_point_reset (&device->sync_point);

  stream_teardown_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (stream_teardown_source, tear_down_streams,
                         device, NULL);
  g_source_set_ready_time (stream_teardown_source, 0);
  g_source_attach (stream_teardown_source, device->camera_context);
  g_source_unref (stream_teardown_source);

  grd_sync_point_wait_for_completion (&device->sync_point);
}

static void
stop_camera_thread (GrdRdpDvcCameraDevice *device)
{
  destroy_camera_streams_in_camera_thread (device);

  device->in_shutdown = TRUE;

  g_main_context_wakeup (device->camera_context);
  g_clear_pointer (&device->camera_thread, g_thread_join);
}

static void
grd_rdp_dvc_camera_device_dispose (GObject *object)
{
  GrdRdpDvcCameraDevice *device = GRD_RDP_DVC_CAMERA_DEVICE (object);
  GrdRdpDvc *dvc = GRD_RDP_DVC (device);

  if (device->camera_thread)
    stop_camera_thread (device);

  g_assert (g_hash_table_size (device->stream_contexts) == 0);
  g_assert (!device->sample_timeout_source);

  if (device->channel_opened)
    {
      device->device_context->Close (device->device_context);
      device->channel_opened = FALSE;
    }
  grd_rdp_dvc_maybe_unsubscribe_creation_status (dvc);

  g_assert (!device->pipewire_core);
  g_assert (!device->pipewire_context);
  g_assert (!device->pipewire_source);

  if (device->event_source)
    {
      g_source_destroy (device->event_source);
      g_clear_pointer (&device->event_source, g_source_unref);
    }
  if (device->initialization_source)
    {
      g_source_destroy (device->initialization_source);
      g_clear_pointer (&device->initialization_source, g_source_unref);
    }

  g_clear_pointer (&device->camera_context, g_main_context_unref);

  g_clear_pointer (&device->pending_samples, g_hash_table_unref);
  g_clear_pointer (&device->pending_events, g_hash_table_unref);
  g_clear_pointer (&device->running_streams, g_hash_table_unref);
  g_clear_pointer (&device->pending_stream_starts, g_hash_table_unref);
  g_clear_pointer (&device->queued_stream_starts, g_hash_table_unref);
  g_clear_pointer (&device->pending_media_type_lists, g_queue_free);

  g_clear_pointer (&device->device_name, g_free);
  g_clear_pointer (&device->dvc_name, g_free);

  g_clear_pointer (&device->device_context, camera_device_server_context_free);

  G_OBJECT_CLASS (grd_rdp_dvc_camera_device_parent_class)->dispose (object);
}

static void
grd_rdp_dvc_camera_device_finalize (GObject *object)
{
  GrdRdpDvcCameraDevice *device = GRD_RDP_DVC_CAMERA_DEVICE (object);

  grd_sync_point_clear (&device->sync_point);

  g_mutex_clear (&device->sample_request_mutex);
  g_mutex_clear (&device->client_request_mutex);
  g_mutex_clear (&device->event_mutex);
  g_mutex_clear (&device->state_mutex);

  g_assert (g_hash_table_size (device->stream_contexts) == 0);
  g_clear_pointer (&device->stream_contexts, g_hash_table_unref);

  G_OBJECT_CLASS (grd_rdp_dvc_camera_device_parent_class)->finalize (object);
}

static void
pipewire_core_error (void       *user_data,
                     uint32_t    id,
                     int         seq,
                     int         res,
                     const char *message)
{
  GrdRdpDvcCameraDevice *device = user_data;

  g_warning ("[RDP.CAM_DEVICE] Device \"%s\": PipeWire core error: "
             "id: %u, seq: %i, res: %i, %s",
             device->device_name, id, seq, res, message);

  if (id == PW_ID_CORE && res == -EPIPE)
    g_signal_emit (device, signals[ERROR], 0);
}

static const struct pw_core_events pipewire_core_events =
{
  .version = PW_VERSION_CORE_EVENTS,
  .error = pipewire_core_error,
};

static gboolean
set_up_pipewire (GrdRdpDvcCameraDevice  *device,
                 GError                **error)
{
  GrdPipeWireSource *pipewire_source;

  pipewire_source = grd_pipewire_source_new ("RDP.CAM_DEVICE", error);
  if (!pipewire_source)
    return FALSE;

  device->pipewire_source = (GSource *) pipewire_source;

  device->pipewire_context =
    pw_context_new (pipewire_source->pipewire_loop, NULL, 0);
  if (!device->pipewire_context)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire context");
      return FALSE;
    }

  device->pipewire_core =
    pw_context_connect (device->pipewire_context, NULL, 0);
  if (!device->pipewire_core)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire core");
      return FALSE;
    }

  pw_core_add_listener (device->pipewire_core,
                        &device->pipewire_core_listener,
                        &pipewire_core_events, device);

  g_source_attach (device->pipewire_source, device->camera_context);

  return TRUE;
}

static void
stop_pipewire (GrdRdpDvcCameraDevice *device)
{
  if (device->pipewire_core)
    {
      spa_hook_remove (&device->pipewire_core_listener);
      g_clear_pointer (&device->pipewire_core, pw_core_disconnect);
    }
  g_clear_pointer (&device->pipewire_context, pw_context_destroy);

  if (device->pipewire_source)
    {
      g_source_destroy (device->pipewire_source);
      g_clear_pointer (&device->pipewire_source, g_source_unref);
    }
}

static gpointer
camera_thread_func (gpointer data)
{
  GrdRdpDvcCameraDevice *device = data;
  g_autoptr (GError) error = NULL;
  gboolean success;

  pw_init (NULL, NULL);

  success = set_up_pipewire (device, &error);
  if (!success)
    {
      g_warning ("[RDP.CAM_DEVICE] Device \"%s\": Failed to set up PipeWire: "
                 "%s", device->device_name, error->message);
    }
  grd_sync_point_complete (&device->sync_point, success);

  while (!device->in_shutdown)
    g_main_context_iteration (device->camera_context, TRUE);

  stop_pipewire (device);

  pw_deinit ();

  return NULL;
}

static void
activate_device (GrdRdpDvcCameraDevice *device)
{
  CameraDeviceServerContext *device_context = device->device_context;
  CAM_ACTIVATE_DEVICE_REQUEST activate_device_request = {};

  device->state = DEVICE_STATE_PENDING_ACTIVATION_RESPONSE;
  device_context->ActivateDeviceRequest (device_context,
                                         &activate_device_request);
}

static gboolean
has_supported_media_type (GList *media_type_descriptions)
{
  GList *l;

  for (l = media_type_descriptions; l; l = l->next)
    {
      CAM_MEDIA_TYPE_DESCRIPTION *media_type_description = l->data;

      /* Sanitize format */
      if (media_type_description->FrameRateNumerator == 0 ||
          media_type_description->FrameRateDenominator == 0 ||
          media_type_description->PixelAspectRatioDenominator == 0)
        continue;

      if (media_type_description->Format == CAM_MEDIA_FORMAT_H264)
        return TRUE;
    }

  return FALSE;
}

static void
create_streams (GrdRdpDvcCameraDevice *device)
{
  StreamContext *stream_context = NULL;
  gpointer key = NULL;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, device->stream_contexts);
  while (g_hash_table_iter_next (&iter, &key, (gpointer *) &stream_context))
    {
      uint8_t stream_index = GPOINTER_TO_UINT (key);
      g_autoptr (GError) error = NULL;

      if (!has_supported_media_type (stream_context->media_type_descriptions))
        continue;

      stream_context->camera_stream =
        grd_rdp_camera_stream_new (device,
                                   device->camera_context,
                                   device->pipewire_core,
                                   device->device_name,
                                   stream_index,
                                   stream_context->media_type_descriptions,
                                   &error);
      if (!stream_context->camera_stream)
        {
          g_warning ("[RDP.CAM_DEVICE] Device \"%s\": Failed to create camera "
                     "stream for stream %u: %s ", device->device_name,
                     stream_index, error->message);
        }
    }

  device->state = DEVICE_STATE_INITIALIZATION_DONE;
}

static gboolean
initialize_device (gpointer user_data)
{
  GrdRdpDvcCameraDevice *device = user_data;
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&device->state_mutex);
  switch (device->state)
    {
    case DEVICE_STATE_FATAL_ERROR:
    case DEVICE_STATE_PENDING_ACTIVATION_RESPONSE:
    case DEVICE_STATE_PENDING_STREAM_LIST_RESPONSE:
    case DEVICE_STATE_PENDING_MEDIA_TYPE_LIST_RESPONSE:
    case DEVICE_STATE_INITIALIZATION_DONE:
    case DEVICE_STATE_IN_SHUTDOWN:
      break;
    case DEVICE_STATE_PENDING_ACTIVATION:
      activate_device (device);
      break;
    case DEVICE_STATE_PENDING_STREAM_PREPARATION:
      create_streams (device);
      break;
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
has_pending_ack_event (GrdRdpDvcCameraDevice *device)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&device->event_mutex);
  return has_pending_event_unlocked (device, DEVICE_EVENT_ACK_STARTED_STREAMS) ||
         has_pending_event_unlocked (device, DEVICE_EVENT_ACK_STOPPED_STREAMS);
}

static gboolean
has_pending_client_success_response (GrdRdpDvcCameraDevice *device)
{
  return device->pending_client_request && !has_pending_ack_event (device);
}

static void
set_pending_client_request (GrdRdpDvcCameraDevice *device,
                            ClientRequestType      request_type)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&device->client_request_mutex);
  g_assert (!device->pending_client_request);
  g_assert (device->pending_client_request_type == CLIENT_REQUEST_TYPE_NONE);

  device->pending_client_request_type = request_type;
  device->pending_client_request = TRUE;
}

static void
reset_pending_client_request (GrdRdpDvcCameraDevice *device)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&device->client_request_mutex);
  device->pending_client_request = FALSE;
  device->pending_client_request_type = CLIENT_REQUEST_TYPE_NONE;
}

static gboolean
try_pop_event (GrdRdpDvcCameraDevice *device,
               DeviceEvent            event)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&device->event_mutex);
  return g_hash_table_remove (device->pending_events, GUINT_TO_POINTER (event));
}

static void
ack_started_streams (GrdRdpDvcCameraDevice *device)
{
  StreamRunContext *stream_run_context = NULL;
  gpointer key = NULL;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, device->pending_stream_starts);
  while (g_hash_table_iter_next (&iter, &key, (gpointer *) &stream_run_context))
    {
      uint8_t stream_index = GPOINTER_TO_UINT (key);
      StreamContext *stream_context = NULL;
      GrdRdpCameraStream *camera_stream;
      uint32_t run_sequence;

      if (!g_hash_table_lookup_extended (device->stream_contexts,
                                         GUINT_TO_POINTER (stream_index),
                                         NULL, (gpointer *) &stream_context))
        g_assert_not_reached ();

      camera_stream = stream_context->camera_stream;
      run_sequence = stream_run_context->run_sequence;

      if (grd_rdp_camera_stream_notify_stream_started (camera_stream,
                                                       run_sequence))
        {
          grd_rdp_camera_stream_uninhibit_camera_loop (camera_stream);

          g_hash_table_insert (device->running_streams,
                               GUINT_TO_POINTER (stream_index),
                               stream_run_context);
          g_hash_table_iter_steal (&iter);
        }
      else
        {
          g_hash_table_iter_remove (&iter);
        }
    }

  g_debug ("[RDP.CAM_DEVICE] Device \"%s\": Started streams",
           device->device_name);
}

static void
ack_stopped_streams (GrdRdpDvcCameraDevice *device)
{
  g_debug ("[RDP.CAM_DEVICE] Device \"%s\": Stopped streams",
           device->device_name);
}

static void
maybe_handle_client_success_response (GrdRdpDvcCameraDevice *device)
{
  if (!has_pending_ack_event (device))
    return;

  reset_pending_client_request (device);

  if (try_pop_event (device, DEVICE_EVENT_ACK_STARTED_STREAMS))
    ack_started_streams (device);
  if (try_pop_event (device, DEVICE_EVENT_ACK_STOPPED_STREAMS))
    ack_stopped_streams (device);
}

static gboolean
has_pending_sample_requests (GrdRdpDvcCameraDevice *device)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&device->sample_request_mutex);
  return g_hash_table_size (device->pending_samples) > 0;
}

static gboolean
discard_sample_requests (gpointer user_data)
{
  GrdRdpDvcCameraDevice *device = user_data;
  StreamContext *stream_context = NULL;
  GHashTableIter iter;

  g_warning ("[RDP.CAM_DEVICE] Device \"%s\": Protocol violation: Got no "
             "sample response after timeout. Discarding requests...",
             device->device_name);

  g_clear_pointer (&device->sample_timeout_source, g_source_unref);

  g_hash_table_iter_init (&iter, device->stream_contexts);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &stream_context))
    discard_pending_sample_requests (stream_context);

  g_source_set_ready_time (device->event_source, 0);

  return G_SOURCE_REMOVE;
}

static void
maybe_set_sample_timeout (GrdRdpDvcCameraDevice *device)
{
  if (device->sample_timeout_source)
    return;

  g_debug ("[RDP.CAM_DEVICE] Device \"%s\": Waiting for remaining samples "
           "before sending stop-streams-request", device->device_name);

  device->sample_timeout_source = g_timeout_source_new (SAMPLE_TIMEOUT_MS);
  g_source_set_callback (device->sample_timeout_source, discard_sample_requests,
                         device, NULL);
  g_source_attach (device->sample_timeout_source, device->camera_context);
}

static void
stop_camera_streams (GrdRdpDvcCameraDevice *device)
{
  CameraDeviceServerContext *device_context = device->device_context;
  CAM_STOP_STREAMS_REQUEST stop_streams_request = {};

  if (device->sample_timeout_source)
    {
      g_source_destroy (device->sample_timeout_source);
      g_clear_pointer (&device->sample_timeout_source, g_source_unref);
    }

  g_debug ("[RDP.CAM_DEVICE] Device \"%s\": Sending stop-streams-request",
           device->device_name);

  set_pending_client_request (device, CLIENT_REQUEST_TYPE_STOP_STREAMS);

  device_context->StopStreamsRequest (device_context, &stop_streams_request);
}

static void
start_queued_camera_streams (GrdRdpDvcCameraDevice *device)
{
  CameraDeviceServerContext *device_context = device->device_context;
  CAM_START_STREAMS_REQUEST start_streams_request = {};
  StreamRunContext *stream_run_context = NULL;
  gpointer key = NULL;
  GHashTableIter iter;
  uint16_t i = 0;

  g_assert (g_hash_table_size (device->pending_stream_starts) == 0);
  g_assert (g_hash_table_size (device->queued_stream_starts) > 0);

  start_streams_request.N_Infos =
    g_hash_table_size (device->queued_stream_starts);

  g_hash_table_iter_init (&iter, device->queued_stream_starts);
  while (g_hash_table_iter_next (&iter, &key, (gpointer *) &stream_run_context))
    {
      CAM_START_STREAM_INFO *start_stream_info;

      start_stream_info = &start_streams_request.StartStreamsInfo[i++];
      start_stream_info->StreamIndex = GPOINTER_TO_UINT (key);
      start_stream_info->MediaTypeDescription =
        *stream_run_context->media_type_description;

      g_hash_table_insert (device->pending_stream_starts,
                           key, stream_run_context);
      g_hash_table_iter_steal (&iter);
    }
  g_assert (start_streams_request.N_Infos == i);

  g_debug ("[RDP.CAM_DEVICE] Device \"%s\": Sending start-streams-request",
           device->device_name);

  set_pending_client_request (device, CLIENT_REQUEST_TYPE_START_STREAMS);

  device_context->StartStreamsRequest (device_context, &start_streams_request);
}

static gboolean
handle_device_events (gpointer user_data)
{
  GrdRdpDvcCameraDevice *device = user_data;

  if (has_pending_client_success_response (device))
    return G_SOURCE_CONTINUE;

  maybe_handle_client_success_response (device);

  if (has_pending_sample_requests (device) &&
      has_pending_event (device, DEVICE_EVENT_STOP_STREAMS))
    maybe_set_sample_timeout (device);

  if (!has_pending_sample_requests (device) &&
      try_pop_event (device, DEVICE_EVENT_STOP_STREAMS))
    stop_camera_streams (device);
  else if (g_hash_table_size (device->queued_stream_starts) > 0 &&
           !has_pending_event (device, DEVICE_EVENT_STOP_STREAMS))
    start_queued_camera_streams (device);

  return G_SOURCE_CONTINUE;
}

static void
grd_rdp_dvc_camera_device_init (GrdRdpDvcCameraDevice *device)
{
  GSource *initialization_source;
  GSource *event_source;

  device->state = DEVICE_STATE_PENDING_ACTIVATION;

  device->stream_contexts =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) stream_context_free);
  device->queued_stream_starts =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) stream_run_context_free);
  device->pending_stream_starts =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) stream_run_context_free);
  device->running_streams =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) stream_run_context_free);
  device->pending_events = g_hash_table_new (NULL, NULL);
  device->pending_samples = g_hash_table_new (NULL, NULL);

  device->pending_media_type_lists = g_queue_new ();

  g_mutex_init (&device->state_mutex);
  g_mutex_init (&device->event_mutex);
  g_mutex_init (&device->client_request_mutex);
  g_mutex_init (&device->sample_request_mutex);

  grd_sync_point_init (&device->sync_point);

  device->camera_context = g_main_context_new ();
  device->camera_thread = g_thread_new ("RDP camera device thread",
                                        camera_thread_func,
                                        device);

  initialization_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (initialization_source, initialize_device,
                         device, NULL);
  g_source_set_ready_time (initialization_source, -1);
  g_source_attach (initialization_source, device->camera_context);
  device->initialization_source = initialization_source;

  event_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (event_source, handle_device_events, device, NULL);
  g_source_set_ready_time (event_source, -1);
  g_source_attach (event_source, device->camera_context);
  device->event_source = event_source;
}

static void
grd_rdp_dvc_camera_device_class_init (GrdRdpDvcCameraDeviceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_dvc_camera_device_dispose;
  object_class->finalize = grd_rdp_dvc_camera_device_finalize;

  signals[ERROR] = g_signal_new ("error",
                                 G_TYPE_FROM_CLASS (klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);
}
