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

#include "grd-rdp-camera-stream.h"

#include <spa/pod/dynamic.h>

#include "grd-decode-session-sw-avc.h"
#include "grd-frame-clock.h"
#include "grd-pipewire-utils.h"
#include "grd-rdp-dvc-camera-device.h"

#define MAX_N_PENDING_FRAMES 2

#define PARAMS_BUFFER_SIZE 1024

typedef struct
{
  GMutex buffer_mutex;
} BufferContext;

struct _GrdRdpCameraStream
{
  GObject parent;

  GrdRdpDvcCameraDevice *device;
  const char *device_name;
  uint8_t stream_index;
  GList *media_type_descriptions;

  GrdFrameClock *frame_clock;

  struct pw_stream *pipewire_stream;
  struct spa_hook pipewire_stream_listener;

  GHashTable *buffer_contexts;

  struct spa_video_info_raw video_format;
  CAM_MEDIA_TYPE_DESCRIPTION *current_media_type_description;
  gboolean pending_decode_session_reset;

  gboolean is_enabled;

  uint32_t current_run_sequence;
  uint32_t last_acked_run_sequence;

  gboolean camera_loop_inhibited;

  GrdDecodeSession *sw_decode_session;
  GrdDecodeSession *decode_session_in_use;

  GMutex dequeued_buffers_mutex;
  GHashTable *dequeued_buffers;
  GHashTable *buffers_in_decoding;

  uint64_t buffer_sequence;
};

G_DEFINE_TYPE (GrdRdpCameraStream, grd_rdp_camera_stream,
               G_TYPE_OBJECT)

static BufferContext *
buffer_context_new (void)
{
  BufferContext *buffer_context;

  buffer_context = g_new0 (BufferContext, 1);
  g_mutex_init (&buffer_context->buffer_mutex);

  return buffer_context;
}

static void
buffer_context_free (BufferContext *buffer_context)
{
  g_mutex_clear (&buffer_context->buffer_mutex);

  g_free (buffer_context);
}

static void
acquire_pipewire_buffer_lock (GrdRdpCameraStream *camera_stream,
                              struct pw_buffer   *pw_buffer)
{
  BufferContext *buffer_context = NULL;

  if (!g_hash_table_lookup_extended (camera_stream->buffer_contexts, pw_buffer,
                                     NULL, (gpointer *) &buffer_context))
    g_assert_not_reached ();

  g_mutex_lock (&buffer_context->buffer_mutex);
}

static void
release_pipewire_buffer_lock (GrdRdpCameraStream *camera_stream,
                              struct pw_buffer   *pw_buffer)
{
  BufferContext *buffer_context = NULL;

  if (!g_hash_table_lookup_extended (camera_stream->buffer_contexts, pw_buffer,
                                     NULL, (gpointer *) &buffer_context))
    g_assert_not_reached ();

  g_mutex_unlock (&buffer_context->buffer_mutex);
}

static void
ensure_unlocked_pipewire_buffer (GrdRdpCameraStream *camera_stream,
                                 struct pw_buffer   *pw_buffer)
{
  BufferContext *buffer_context = NULL;

  if (!g_hash_table_lookup_extended (camera_stream->buffer_contexts, pw_buffer,
                                     NULL, (gpointer *) &buffer_context))
    g_assert_not_reached ();

  g_mutex_lock (&buffer_context->buffer_mutex);
  g_mutex_unlock (&buffer_context->buffer_mutex);
}

uint8_t
grd_rdp_camera_stream_get_stream_index (GrdRdpCameraStream *camera_stream)
{
  return camera_stream->stream_index;
}

static gboolean
can_serve_frames (GrdRdpCameraStream *camera_stream)
{
  return camera_stream->is_enabled &&
         camera_stream->current_run_sequence == camera_stream->last_acked_run_sequence &&
         !camera_stream->camera_loop_inhibited;
}

static void
maybe_start_camera_loop (GrdRdpCameraStream *camera_stream)
{
  const char *device_name = camera_stream->device_name;
  uint8_t stream_index = camera_stream->stream_index;
  struct spa_video_info_raw *video_format = &camera_stream->video_format;

  if (!can_serve_frames (camera_stream))
    return;

  if (grd_frame_clock_is_armed (camera_stream->frame_clock))
    return;

  g_debug ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: Starting camera stream",
           device_name, stream_index);

  grd_frame_clock_arm_timer (camera_stream->frame_clock,
                             video_format->framerate.num,
                             video_format->framerate.denom);
}

gboolean
grd_rdp_camera_stream_notify_stream_started (GrdRdpCameraStream *camera_stream,
                                             uint32_t            run_sequence)
{
  if (run_sequence != camera_stream->current_run_sequence)
    return FALSE;

  if (run_sequence == camera_stream->last_acked_run_sequence)
    return TRUE;

  camera_stream->last_acked_run_sequence = run_sequence;

  maybe_start_camera_loop (camera_stream);

  return TRUE;
}

static void
stop_camera_loop (GrdRdpCameraStream *camera_stream)
{
  grd_frame_clock_disarm_timer (camera_stream->frame_clock);
}

void
grd_rdp_camera_stream_inhibit_camera_loop (GrdRdpCameraStream *camera_stream)
{
  camera_stream->camera_loop_inhibited = TRUE;

  stop_camera_loop (camera_stream);
}

void
grd_rdp_camera_stream_uninhibit_camera_loop (GrdRdpCameraStream *camera_stream)
{
  camera_stream->camera_loop_inhibited = FALSE;

  maybe_start_camera_loop (camera_stream);
}

gboolean
grd_rdp_camera_stream_announce_new_sample (GrdRdpCameraStream *camera_stream,
                                           GrdSampleBuffer    *sample_buffer)
{
  g_autoptr (GMutexLocker) locker = NULL;
  struct pw_buffer *pw_buffer = NULL;
  g_autoptr (GError) error = NULL;

  locker = g_mutex_locker_new (&camera_stream->dequeued_buffers_mutex);
  if (!g_hash_table_lookup_extended (camera_stream->dequeued_buffers,
                                     sample_buffer,
                                     NULL, (gpointer *) &pw_buffer))
    return FALSE;

  g_hash_table_insert (camera_stream->buffers_in_decoding,
                       sample_buffer, pw_buffer);

  return TRUE;
}

static void
queue_corrupted_pw_buffer (GrdRdpCameraStream *camera_stream,
                           struct pw_buffer   *pw_buffer)
{
  struct spa_buffer *spa_buffer;
  struct spa_data *spa_data;
  struct spa_meta_header *spa_header;

  spa_buffer = pw_buffer->buffer;
  spa_data = &spa_buffer->datas[0];

  spa_data->chunk->size = 0;
  spa_data->chunk->flags = SPA_CHUNK_FLAG_CORRUPTED;

  spa_header = spa_buffer_find_meta_data (spa_buffer, SPA_META_Header,
                                          sizeof (struct spa_meta_header));
  if (spa_header)
    {
      spa_header->flags = 0;
      spa_header->pts = pw_stream_get_nsec (camera_stream->pipewire_stream);
      spa_header->dts_offset = 0;
      spa_header->seq = ++camera_stream->buffer_sequence;
    }

  pw_stream_queue_buffer (camera_stream->pipewire_stream, pw_buffer);
  release_pipewire_buffer_lock (camera_stream, pw_buffer);
}

static void
discard_failed_sample_request (GrdRdpCameraStream *camera_stream,
                               GrdSampleBuffer    *sample_buffer,
                               struct pw_buffer   *pw_buffer)
{
  g_mutex_lock (&camera_stream->dequeued_buffers_mutex);
  g_hash_table_remove (camera_stream->buffers_in_decoding, sample_buffer);
  g_hash_table_remove (camera_stream->dequeued_buffers, sample_buffer);
  g_mutex_unlock (&camera_stream->dequeued_buffers_mutex);

  queue_corrupted_pw_buffer (camera_stream, pw_buffer);
}

static void
on_frame_ready (GrdDecodeSession *decode_session,
                GrdSampleBuffer  *sample_buffer,
                gpointer          user_data,
                GError           *error)
{
  GrdRdpCameraStream *camera_stream = user_data;
  const char *device_name = camera_stream->device_name;
  uint8_t stream_index = camera_stream->stream_index;
  struct pw_buffer *pw_buffer = NULL;
  struct spa_buffer *spa_buffer;
  struct spa_data *spa_data;
  struct spa_meta_header *spa_header;

  g_mutex_lock (&camera_stream->dequeued_buffers_mutex);
  if (!g_hash_table_steal_extended (camera_stream->dequeued_buffers,
                                    sample_buffer,
                                    NULL, (gpointer *) &pw_buffer))
    g_assert_not_reached ();

  g_hash_table_remove (camera_stream->buffers_in_decoding, sample_buffer);
  g_mutex_unlock (&camera_stream->dequeued_buffers_mutex);

  spa_buffer = pw_buffer->buffer;
  spa_data = &spa_buffer->datas[0];

  if (error)
    {
      g_warning ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: Failed to decode "
                 "frame: %s", device_name, stream_index, error->message);
      spa_data->chunk->size = 0;
      spa_data->chunk->flags = SPA_CHUNK_FLAG_CORRUPTED;
    }
  else
    {
      spa_data->chunk->offset = 0;
      spa_data->chunk->size = spa_data->maxsize;
      spa_data->chunk->flags = SPA_CHUNK_FLAG_NONE;
    }

  spa_header = spa_buffer_find_meta_data (spa_buffer, SPA_META_Header,
                                          sizeof (struct spa_meta_header));
  if (spa_header)
    {
      spa_header->flags = 0;
      spa_header->pts = pw_stream_get_nsec (camera_stream->pipewire_stream);
      spa_header->dts_offset = 0;
      spa_header->seq = ++camera_stream->buffer_sequence;
    }

  pw_stream_queue_buffer (camera_stream->pipewire_stream, pw_buffer);
  release_pipewire_buffer_lock (camera_stream, pw_buffer);
}

void
grd_rdp_camera_stream_submit_sample (GrdRdpCameraStream *camera_stream,
                                     GrdSampleBuffer    *sample_buffer,
                                     gboolean            success)
{
  GrdDecodeSession *decode_session = camera_stream->decode_session_in_use;
  const char *device_name = camera_stream->device_name;
  uint8_t stream_index = camera_stream->stream_index;
  g_autoptr (GMutexLocker) locker = NULL;
  struct pw_buffer *pw_buffer = NULL;
  g_autoptr (GError) error = NULL;

  locker = g_mutex_locker_new (&camera_stream->dequeued_buffers_mutex);
  if (!g_hash_table_lookup_extended (camera_stream->dequeued_buffers,
                                     sample_buffer,
                                     NULL, (gpointer *) &pw_buffer))
    return;

  g_hash_table_insert (camera_stream->buffers_in_decoding,
                       sample_buffer, pw_buffer);
  g_clear_pointer (&locker, g_mutex_locker_free);

  if (!success || !can_serve_frames (camera_stream))
    {
      discard_failed_sample_request (camera_stream, sample_buffer, pw_buffer);
      return;
    }

  if (grd_decode_session_decode_frame (decode_session, sample_buffer,
                                       on_frame_ready, camera_stream,
                                       &error))
    return;

  g_warning ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: Failed to start "
             "decoding frame: %s", device_name, stream_index, error->message);

  discard_failed_sample_request (camera_stream, sample_buffer, pw_buffer);
}

static void
on_frame_clock_trigger (gpointer user_data)
{
  GrdRdpCameraStream *camera_stream = user_data;
  GrdDecodeSession *decode_session = camera_stream->decode_session_in_use;
  struct pw_buffer *pw_buffer;
  GrdSampleBuffer *sample_buffer;
  uint32_t n_pending_frames;

  if (!can_serve_frames (camera_stream))
    return;

  n_pending_frames = grd_decode_session_get_n_pending_frames (decode_session);
  if (n_pending_frames > MAX_N_PENDING_FRAMES)
    return;

  /*
   * If there is no PipeWire buffer available, then the consumer side is too
   * slow. Don't emit a warning for this, as this can spam up the whole log.
   */
  pw_buffer = pw_stream_dequeue_buffer (camera_stream->pipewire_stream);
  if (!pw_buffer)
    return;

  acquire_pipewire_buffer_lock (camera_stream, pw_buffer);

  sample_buffer = grd_decode_session_get_sample_buffer (decode_session,
                                                        pw_buffer);

  g_mutex_lock (&camera_stream->dequeued_buffers_mutex);
  g_hash_table_insert (camera_stream->dequeued_buffers,
                       sample_buffer, pw_buffer);

  g_assert (!g_hash_table_contains (camera_stream->buffers_in_decoding,
                                    sample_buffer));
  g_mutex_unlock (&camera_stream->dequeued_buffers_mutex);

  grd_rdp_dvc_camera_device_request_sample (camera_stream->device,
                                            camera_stream,
                                            sample_buffer);
}

static void
push_format_object (GArray                 *pod_offsets,
                    struct spa_pod_builder *pod_builder,
                    enum spa_video_format   format,
                    uint64_t               *modifiers,
                    uint32_t                n_modifiers,
                    gboolean                fixate_modifier,
                    ...)
{
  struct spa_pod_frame pod_frame;
  va_list args;

  grd_append_pod_offset (pod_offsets, pod_builder);

  spa_pod_builder_push_object (pod_builder,
                               &pod_frame,
                               SPA_TYPE_OBJECT_Format,
                               SPA_PARAM_EnumFormat);
  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_mediaType,
                       SPA_POD_Id (SPA_MEDIA_TYPE_video),
                       0);
  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_mediaSubtype,
                       SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
                       0);
  spa_pod_builder_add (pod_builder,
                       SPA_FORMAT_VIDEO_format,
                       SPA_POD_Id (format),
                       0);
  if (n_modifiers > 0)
    {
      if (fixate_modifier)
        {
          spa_pod_builder_prop (pod_builder,
                                SPA_FORMAT_VIDEO_modifier,
                                SPA_POD_PROP_FLAG_MANDATORY);
          spa_pod_builder_long (pod_builder, modifiers[0]);
        }
      else
        {
          struct spa_pod_frame pod_frame_mods;
          uint32_t i;

          spa_pod_builder_prop (pod_builder,
                                SPA_FORMAT_VIDEO_modifier,
                                (SPA_POD_PROP_FLAG_MANDATORY |
                                 SPA_POD_PROP_FLAG_DONT_FIXATE));
          spa_pod_builder_push_choice (pod_builder,
                                       &pod_frame_mods,
                                       SPA_CHOICE_Enum,
                                       0);
          spa_pod_builder_long (pod_builder, modifiers[0]);
          for (i = 0; i < n_modifiers; i++)
            spa_pod_builder_long (pod_builder, modifiers[i]);
          spa_pod_builder_pop (pod_builder, &pod_frame_mods);
        }
    }

  va_start (args, fixate_modifier);
  spa_pod_builder_addv (pod_builder, args);
  va_end (args);

  spa_pod_builder_pop (pod_builder, &pod_frame);
}

static void
push_format_objects (GrdRdpCameraStream     *camera_stream,
                     GrdDecodeSession       *decode_session,
                     enum spa_video_format   format,
                     struct spa_pod_builder *pod_builder,
                     GArray                 *pod_offsets)
{
  g_autofree uint64_t *modifiers = NULL;
  uint32_t n_modifiers = 0;
  uint32_t drm_format;
  GList *l;

  grd_get_spa_format_details (format, &drm_format, NULL);

  grd_decode_session_get_drm_format_modifiers (decode_session, drm_format,
                                               &n_modifiers, &modifiers);

  for (l = camera_stream->media_type_descriptions; l; l = l->next)
    {
      CAM_MEDIA_TYPE_DESCRIPTION *description = l->data;

      /* Only add sanitized formats */
      if (description->FrameRateNumerator == 0 ||
          description->FrameRateDenominator == 0 ||
          description->PixelAspectRatioDenominator == 0)
        continue;

      push_format_object (
        pod_offsets, pod_builder,
        format, modifiers, n_modifiers, FALSE,
        SPA_FORMAT_VIDEO_size,
        SPA_POD_Rectangle (&SPA_RECTANGLE (description->Width,
                                           description->Height)),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_Fraction (&SPA_FRACTION (description->FrameRateNumerator,
                                         description->FrameRateDenominator)),
        SPA_FORMAT_VIDEO_pixelAspectRatio,
        SPA_POD_Rectangle (&SPA_RECTANGLE (description->PixelAspectRatioNumerator,
                                           description->PixelAspectRatioDenominator)),
        0);
    }
}

static void
build_format_params (GrdRdpCameraStream     *camera_stream,
                     struct spa_pod_builder *pod_builder,
                     GArray                 *pod_offsets)
{
  GrdDecodeSession *sw_decode_session = camera_stream->sw_decode_session;

  if (sw_decode_session)
    {
      push_format_objects (camera_stream, sw_decode_session,
                           SPA_VIDEO_FORMAT_BGRA, pod_builder, pod_offsets);
    }
}

static void
stop_camera_stream (GrdRdpCameraStream *camera_stream,
                    gboolean            was_connecting)
{
  const char *device_name = camera_stream->device_name;
  uint8_t stream_index = camera_stream->stream_index;

  camera_stream->is_enabled = FALSE;
  stop_camera_loop (camera_stream);

  if (was_connecting)
    return;

  g_debug ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: Stopping camera stream",
           device_name, stream_index);

  grd_rdp_dvc_camera_device_stop_stream (camera_stream->device, camera_stream);
}

static void
start_camera_stream (GrdRdpCameraStream *camera_stream)
{
  CAM_MEDIA_TYPE_DESCRIPTION *media_type_description =
    camera_stream->current_media_type_description;

  while (G_UNLIKELY (camera_stream->current_run_sequence + 1 ==
                     camera_stream->last_acked_run_sequence))
    ++camera_stream->current_run_sequence;

  grd_rdp_dvc_camera_device_start_stream (camera_stream->device, camera_stream,
                                          media_type_description,
                                          ++camera_stream->current_run_sequence);
  camera_stream->is_enabled = TRUE;
}

static void
on_stream_state_changed (void                 *user_data,
                         enum pw_stream_state  old,
                         enum pw_stream_state  state,
                         const char           *error)
{
  GrdRdpCameraStream *camera_stream = user_data;
  const char *device_name = camera_stream->device_name;
  uint8_t stream_index = camera_stream->stream_index;

  g_debug ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: PipeWire stream state "
           "changed from %s to %s", device_name, stream_index,
           pw_stream_state_as_string (old),
           pw_stream_state_as_string (state));

  switch (state)
    {
    case PW_STREAM_STATE_ERROR:
      g_warning ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: PipeWire stream "
                 "error: %s", device_name, stream_index, error);
      stop_camera_stream (camera_stream, old == PW_STREAM_STATE_CONNECTING);
      break;
    case PW_STREAM_STATE_PAUSED:
      stop_camera_stream (camera_stream, old == PW_STREAM_STATE_CONNECTING);
      break;
    case PW_STREAM_STATE_STREAMING:
      start_camera_stream (camera_stream);
      break;
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_CONNECTING:
      break;
    }
}

static uint32_t
gcd (uint32_t num,
     uint32_t denom)
{
  g_assert (num > 0);
  g_assert (denom > 0);

  while (denom != 0)
    {
      uint32_t remainder;

      remainder = num % denom;
      num = denom;
      denom = remainder;
    }

  return num;
}

static CAM_MEDIA_TYPE_DESCRIPTION *
get_media_type_description_for_video_format (GrdRdpCameraStream  *camera_stream,
                                             GError             **error)
{
  struct spa_video_info_raw *video_format = &camera_stream->video_format;
  GList *l;

  for (l = camera_stream->media_type_descriptions; l; l = l->next)
    {
      CAM_MEDIA_TYPE_DESCRIPTION *description = l->data;
      uint32_t sanitized_ecam_num;
      uint32_t sanitized_ecam_denom;
      uint32_t gcd_pw;
      uint32_t gcd_ecam;

      /* Only check added formats */
      if (description->FrameRateNumerator == 0 ||
          description->FrameRateDenominator == 0 ||
          description->PixelAspectRatioDenominator == 0)
        continue;

      if (video_format->size.width != description->Width ||
          video_format->size.height != description->Height)
        continue;

      gcd_pw = gcd (video_format->framerate.num,
                    video_format->framerate.denom);
      gcd_ecam = gcd (description->FrameRateNumerator,
                      description->FrameRateDenominator);

      sanitized_ecam_num = description->FrameRateNumerator / gcd_ecam;
      sanitized_ecam_denom = description->FrameRateDenominator / gcd_ecam;

      if (video_format->framerate.num / gcd_pw != sanitized_ecam_num ||
          video_format->framerate.denom / gcd_pw != sanitized_ecam_denom)
        continue;

      return description;
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Failed to find associated media type description for PipeWire "
               "video format");

  return NULL;
}

static void
pod_builder_add_object (struct spa_pod_builder *pod_builder,
                        GArray                 *pod_offsets,
                        uint32_t                type,
                        uint32_t                id,
                        ...)
{
  struct spa_pod_frame pod_frame;
  va_list args;

  grd_append_pod_offset (pod_offsets, pod_builder);

  spa_pod_builder_push_object (pod_builder, &pod_frame, type, id);

  va_start (args, id);
  spa_pod_builder_addv (pod_builder, args);
  va_end (args);

  spa_pod_builder_pop (pod_builder, &pod_frame);
}

static void
on_stream_param_changed (void                 *user_data,
                         uint32_t              id,
                         const struct spa_pod *param)
{
  GrdRdpCameraStream *camera_stream = user_data;
  const char *device_name = camera_stream->device_name;
  uint8_t stream_index = camera_stream->stream_index;
  struct spa_video_info_raw *video_format = &camera_stream->video_format;
  g_autoptr (GArray) pod_offsets = NULL;
  g_autoptr (GPtrArray) params = NULL;
  g_autoptr (GError) error = NULL;
  struct spa_pod_dynamic_builder pod_builder;
  uint32_t buffer_type;

  if (!param || id != SPA_PARAM_Format)
    return;

  g_assert (!camera_stream->is_enabled);

  spa_format_video_raw_parse (param, video_format);
  g_debug ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: New format: "
           "Size: %ux%u, framerate: %u/%u", device_name, stream_index,
           video_format->size.width, video_format->size.height,
           video_format->framerate.num, video_format->framerate.denom);

  camera_stream->current_media_type_description =
    get_media_type_description_for_video_format (camera_stream,
                                                 &error);
  if (!camera_stream->current_media_type_description)
    {
      pw_stream_set_error (camera_stream->pipewire_stream, -EINVAL,
                           "%s", error->message);
      return;
    }
  camera_stream->pending_decode_session_reset = TRUE;

  pod_offsets = g_array_new (FALSE, FALSE, sizeof (uint32_t));
  spa_pod_dynamic_builder_init (&pod_builder, NULL, 0, PARAMS_BUFFER_SIZE);

  buffer_type = 1 << SPA_DATA_MemFd;

  pod_builder_add_object (
    &pod_builder.b, pod_offsets,
    SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int (8, 2, 8),
    SPA_PARAM_BUFFERS_blocks, SPA_POD_Int (1),
    SPA_PARAM_BUFFERS_align, SPA_POD_Int (16),
    SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int (buffer_type));

  pod_builder_add_object (
    &pod_builder.b, pod_offsets,
    SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
    SPA_PARAM_META_type, SPA_POD_Id (SPA_META_Header),
    SPA_PARAM_META_size, SPA_POD_Int (sizeof (struct spa_meta_header)));

  params = grd_finish_pipewire_params (&pod_builder.b, pod_offsets);

  pw_stream_update_params (camera_stream->pipewire_stream,
                           (const struct spa_pod **) params->pdata,
                           params->len);
  spa_pod_dynamic_builder_clean (&pod_builder);
}

static gboolean
has_negotiated_dma_bufs (GrdRdpCameraStream *camera_stream,
                         struct pw_buffer   *pw_buffer)
{
  struct spa_data *spa_data = &pw_buffer->buffer->datas[0];

  if (spa_data->type & 1 << SPA_DATA_DmaBuf)
    return TRUE;

  return FALSE;
}

static gboolean
maybe_reset_decode_session (GrdRdpCameraStream  *camera_stream,
                            gboolean             using_dma_bufs,
                            GError             **error)
{
  struct spa_video_info_raw *video_format = &camera_stream->video_format;
  const char *device_name = camera_stream->device_name;
  uint8_t stream_index = camera_stream->stream_index;

  if (!camera_stream->pending_decode_session_reset)
    return TRUE;

  g_debug ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: Resetting decode "
           "session", device_name, stream_index);

  g_assert (!using_dma_bufs);
  camera_stream->decode_session_in_use = camera_stream->sw_decode_session;

  if (!grd_decode_session_reset (camera_stream->decode_session_in_use,
                                 video_format->size.width,
                                 video_format->size.height,
                                 video_format->modifier,
                                 error))
    return FALSE;

  camera_stream->pending_decode_session_reset = FALSE;

  return TRUE;
}

static void
maybe_init_buffer_data (struct pw_buffer *pw_buffer)
{
  struct spa_data *spa_data = &pw_buffer->buffer->datas[0];

  if (spa_data->type != SPA_DATA_MemFd)
    return;

  /*
   * Some applications like Firefox or Chromium don't support BGRX images, but
   * only BGRA images. The alpha bit is however untouched by FreeRDPs H.264
   * decoder. So initialize the memory with 0xFF to ensure that the opacity is
   * always 100%.
   */
  memset (spa_data->data, 0xFF, spa_data->maxsize);
}

static void
on_stream_add_buffer (void             *user_data,
                      struct pw_buffer *pw_buffer)
{
  GrdRdpCameraStream *camera_stream = user_data;
  const char *device_name = camera_stream->device_name;
  uint8_t stream_index = camera_stream->stream_index;
  struct spa_data *spa_data = &pw_buffer->buffer->datas[0];
  g_autoptr (GError) error = NULL;
  gboolean using_dma_bufs;
  const char *buffer_type;

  g_hash_table_insert (camera_stream->buffer_contexts,
                       pw_buffer, buffer_context_new ());

  if (!(spa_data->type & 1 << SPA_DATA_MemFd))
    {
      pw_stream_set_error (camera_stream->pipewire_stream, -EINVAL,
                           "No supported PipeWire stream buffer data type "
                           "could be negotiated");
      return;
    }

  using_dma_bufs = has_negotiated_dma_bufs (camera_stream, pw_buffer);

  buffer_type = using_dma_bufs ? "dma-buf" : "mem-fd";
  g_debug ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: Allocating %s buffer "
           "for PipeWire stream", device_name, stream_index, buffer_type);

  if (!maybe_reset_decode_session (camera_stream, using_dma_bufs, &error) ||
      !grd_decode_session_register_buffer (camera_stream->decode_session_in_use,
                                           pw_buffer, &error))
    {
      pw_stream_set_error (camera_stream->pipewire_stream, -EINVAL,
                           "%s", error->message);
      return;
    }

  maybe_init_buffer_data (pw_buffer);
}

static void
discard_unanswered_sample_requests (GrdRdpCameraStream *camera_stream)
{
  GrdSampleBuffer *sample_buffer = NULL;
  struct pw_buffer *pw_buffer = NULL;
  GHashTableIter iter;

  g_mutex_lock (&camera_stream->dequeued_buffers_mutex);
  g_hash_table_iter_init (&iter, camera_stream->dequeued_buffers);
  while (g_hash_table_iter_next (&iter,
                                 (gpointer *) &sample_buffer,
                                 (gpointer *) &pw_buffer))
    {
      if (g_hash_table_contains (camera_stream->buffers_in_decoding,
                                 sample_buffer))
        continue;

      queue_corrupted_pw_buffer (camera_stream, pw_buffer);
      g_hash_table_iter_remove (&iter);
    }
  g_mutex_unlock (&camera_stream->dequeued_buffers_mutex);
}

static void
on_stream_remove_buffer (void             *user_data,
                         struct pw_buffer *pw_buffer)
{
  GrdRdpCameraStream *camera_stream = user_data;
  const char *device_name = camera_stream->device_name;
  uint8_t stream_index = camera_stream->stream_index;

  discard_unanswered_sample_requests (camera_stream);
  ensure_unlocked_pipewire_buffer (camera_stream, pw_buffer);

  g_debug ("[RDP.CAM_DEVICE] Device \"%s\", stream %u: Removing buffer for "
           "PipeWire stream", device_name, stream_index);

  grd_decode_session_unregister_buffer (camera_stream->decode_session_in_use,
                                        pw_buffer);
  g_hash_table_remove (camera_stream->buffer_contexts, pw_buffer);
}

static const struct pw_stream_events stream_events =
{
  .version = PW_VERSION_STREAM_EVENTS,
  .state_changed = on_stream_state_changed,
  .param_changed = on_stream_param_changed,
  .add_buffer = on_stream_add_buffer,
  .remove_buffer = on_stream_remove_buffer,
};

static gboolean
set_up_video_source (GrdRdpCameraStream  *camera_stream,
                     struct pw_core      *pipewire_core,
                     GError             **error)
{
  const char *device_name = camera_stream->device_name;
  g_autofree char *stream_name = NULL;
  g_autoptr (GArray) pod_offsets = NULL;
  g_autoptr (GPtrArray) params = NULL;
  struct spa_pod_dynamic_builder pod_builder;
  int result;

  stream_name =
    g_strdup_printf ("%s::%u", device_name, camera_stream->stream_index);

  camera_stream->pipewire_stream =
    pw_stream_new (pipewire_core, stream_name,
                   pw_properties_new (PW_KEY_MEDIA_TYPE, "Video",
                                      PW_KEY_MEDIA_CATEGORY, "Capture",
                                      PW_KEY_MEDIA_ROLE, "Camera",
                                      PW_KEY_MEDIA_CLASS, "Video/Source",
                                      PW_KEY_NODE_NAME, "grd_remote_video_source",
                                      PW_KEY_NODE_NICK, device_name,
                                      PW_KEY_NODE_DESCRIPTION, device_name,
                                      NULL));
  if (!camera_stream->pipewire_stream)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire stream");
      return FALSE;
    }

  pw_stream_add_listener (camera_stream->pipewire_stream,
                          &camera_stream->pipewire_stream_listener,
                          &stream_events, camera_stream);

  pod_offsets = g_array_new (FALSE, FALSE, sizeof (uint32_t));
  spa_pod_dynamic_builder_init (&pod_builder, NULL, 0, PARAMS_BUFFER_SIZE);

  build_format_params (camera_stream, &pod_builder.b, pod_offsets);
  params = grd_finish_pipewire_params (&pod_builder.b, pod_offsets);

  result = pw_stream_connect (camera_stream->pipewire_stream,
                              PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              PW_STREAM_FLAG_DRIVER |
                              PW_STREAM_FLAG_ALLOC_BUFFERS,
                              (const struct spa_pod **) params->pdata,
                              params->len);
  spa_pod_dynamic_builder_clean (&pod_builder);

  if (result < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (-result),
                           strerror (-result));
      return FALSE;
    }

  return TRUE;
}

GrdRdpCameraStream *
grd_rdp_camera_stream_new (GrdRdpDvcCameraDevice  *device,
                           GMainContext           *camera_context,
                           struct pw_core         *pipewire_core,
                           const char             *device_name,
                           uint8_t                 stream_index,
                           GList                  *media_type_descriptions,
                           GError                **error)
{
  g_autoptr (GrdRdpCameraStream) camera_stream = NULL;

  camera_stream = g_object_new (GRD_TYPE_RDP_CAMERA_STREAM, NULL);
  camera_stream->device = device;
  camera_stream->device_name = device_name;
  camera_stream->stream_index = stream_index;
  camera_stream->media_type_descriptions = media_type_descriptions;

  camera_stream->sw_decode_session =
    (GrdDecodeSession *) grd_decode_session_sw_avc_new (error);
  if (!camera_stream->sw_decode_session)
    return NULL;

  camera_stream->frame_clock = grd_frame_clock_new (camera_context,
                                                    on_frame_clock_trigger,
                                                    camera_stream,
                                                    error);
  if (!camera_stream->frame_clock)
    return NULL;

  if (!set_up_video_source (camera_stream, pipewire_core, error))
    return NULL;

  return g_steal_pointer (&camera_stream);
}

static void
grd_rdp_camera_stream_dispose (GObject *object)
{
  GrdRdpCameraStream *camera_stream = GRD_RDP_CAMERA_STREAM (object);

  if (camera_stream->pipewire_stream)
    {
      pw_stream_destroy (camera_stream->pipewire_stream);
      camera_stream->pipewire_stream = NULL;
    }

  g_mutex_lock (&camera_stream->dequeued_buffers_mutex);
  g_assert (g_hash_table_size (camera_stream->dequeued_buffers) == 0);
  g_assert (g_hash_table_size (camera_stream->buffers_in_decoding) == 0);
  g_mutex_unlock (&camera_stream->dequeued_buffers_mutex);

  g_clear_object (&camera_stream->frame_clock);
  g_clear_object (&camera_stream->sw_decode_session);

  G_OBJECT_CLASS (grd_rdp_camera_stream_parent_class)->dispose (object);
}

static void
grd_rdp_camera_stream_finalize (GObject *object)
{
  GrdRdpCameraStream *camera_stream = GRD_RDP_CAMERA_STREAM (object);

  g_mutex_clear (&camera_stream->dequeued_buffers_mutex);

  g_clear_pointer (&camera_stream->dequeued_buffers, g_hash_table_unref);
  g_clear_pointer (&camera_stream->buffers_in_decoding, g_hash_table_unref);
  g_clear_pointer (&camera_stream->buffer_contexts, g_hash_table_unref);

  G_OBJECT_CLASS (grd_rdp_camera_stream_parent_class)->finalize (object);
}

static void
grd_rdp_camera_stream_init (GrdRdpCameraStream *camera_stream)
{
  camera_stream->buffer_contexts =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) buffer_context_free);
  camera_stream->dequeued_buffers = g_hash_table_new (NULL, NULL);
  camera_stream->buffers_in_decoding = g_hash_table_new (NULL, NULL);

  g_mutex_init (&camera_stream->dequeued_buffers_mutex);
}

static void
grd_rdp_camera_stream_class_init (GrdRdpCameraStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_camera_stream_dispose;
  object_class->finalize = grd_rdp_camera_stream_finalize;
}
