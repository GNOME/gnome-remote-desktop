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

#include "grd-rdp-audio-output-stream.h"

#include <gio/gio.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include "grd-rdp-audio-playback.h"

struct _GrdRdpAudioOutputStream
{
  GObject parent;

  uint32_t target_node_id;

  GrdRdpAudioPlayback *audio_playback;

  struct pw_node *pipewire_node;
  struct spa_hook pipewire_node_listener;

  struct pw_stream *pipewire_stream;
  struct spa_hook pipewire_stream_listener;

  GMutex volume_mutex;
  gboolean audio_muted;
  float volumes[SPA_AUDIO_MAX_CHANNELS];
  uint32_t n_volumes;
};

G_DEFINE_TYPE (GrdRdpAudioOutputStream, grd_rdp_audio_output_stream, G_TYPE_OBJECT)

void
grd_rdp_audio_output_stream_set_active (GrdRdpAudioOutputStream *audio_output_stream,
                                        gboolean                 active)
{
  pw_stream_set_active (audio_output_stream->pipewire_stream, active);
}

void
grd_rdp_audio_output_stream_get_volume_data (GrdRdpAudioOutputStream *audio_output_stream,
                                             GrdRdpAudioVolumeData   *volume_data)
{
  uint32_t i;

  g_mutex_lock (&audio_output_stream->volume_mutex);
  volume_data->audio_muted = audio_output_stream->audio_muted;
  volume_data->n_volumes = audio_output_stream->n_volumes;

  for (i = 0; i < audio_output_stream->n_volumes; ++i)
    volume_data->volumes[i] = audio_output_stream->volumes[i];
  g_mutex_unlock (&audio_output_stream->volume_mutex);
}

static void
pipewire_node_info (void                      *user_data,
                    const struct pw_node_info *info)
{
  GrdRdpAudioOutputStream *audio_output_stream = user_data;
  uint32_t i;

  if (!(info->change_mask & PW_NODE_CHANGE_MASK_PARAMS))
    return;

  for (i = 0; i < info->n_params; ++i)
    {
      if (info->params[i].id == SPA_PARAM_Props &&
          info->params[i].flags & SPA_PARAM_INFO_READ)
        {
          pw_node_enum_params (audio_output_stream->pipewire_node,
                               0, SPA_PARAM_Props, 0, -1, NULL);
        }
    }
}

static void
pipewire_node_param (void                 *user_data,
                     int                   seq,
                     uint32_t              id,
                     uint32_t              index,
                     uint32_t              next,
                     const struct spa_pod *param)
{
  GrdRdpAudioOutputStream *audio_output_stream = user_data;
  const struct spa_pod_object *object = (const struct spa_pod_object *) param;
  struct spa_pod_prop *prop;

  if (id != SPA_PARAM_Props)
    return;

  SPA_POD_OBJECT_FOREACH (object, prop)
    {
      if (prop->key == SPA_PROP_mute)
        {
          bool mute;

          if (spa_pod_get_bool (&prop->value, &mute) < 0)
            continue;

          g_mutex_lock (&audio_output_stream->volume_mutex);
          audio_output_stream->audio_muted = !!mute;
          g_mutex_unlock (&audio_output_stream->volume_mutex);
        }
      else if (prop->key == SPA_PROP_channelVolumes)
        {
          g_mutex_lock (&audio_output_stream->volume_mutex);
          audio_output_stream->n_volumes =
            spa_pod_copy_array (&prop->value, SPA_TYPE_Float,
                                audio_output_stream->volumes,
                                SPA_AUDIO_MAX_CHANNELS);
          g_mutex_unlock (&audio_output_stream->volume_mutex);
        }
    }
}

static const struct pw_node_events pipewire_node_events =
{
  .version = PW_VERSION_NODE_EVENTS,
  .info = pipewire_node_info,
  .param = pipewire_node_param,
};

static void
pipewire_stream_state_changed (void                 *user_data,
                               enum pw_stream_state  old,
                               enum pw_stream_state  state,
                               const char           *error)
{
  GrdRdpAudioOutputStream *audio_output_stream = user_data;

  g_debug ("[RDP.AUDIO_PLAYBACK] Node %u: PipeWire stream state changed from "
           "%s to %s", audio_output_stream->target_node_id,
           pw_stream_state_as_string (old),
           pw_stream_state_as_string (state));

  switch (state)
    {
    case PW_STREAM_STATE_ERROR:
      g_warning ("[RDP.AUDIO_PLAYBACK] Node %u: PipeWire stream error: %s",
                 audio_output_stream->target_node_id, error);
      break;
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_STREAMING:
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_CONNECTING:
      break;
    }
}

static void
pipewire_stream_param_changed (void                 *user_data,
                               uint32_t              id,
                               const struct spa_pod *param)
{
  GrdRdpAudioOutputStream *audio_output_stream = user_data;
  struct spa_pod_builder pod_builder;
  const struct spa_pod *params[1];
  uint8_t params_buffer[1024];

  if (!param || id != SPA_PARAM_Format)
    return;

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));

  params[0] = spa_pod_builder_add_object (
    &pod_builder,
    SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
    SPA_PARAM_BUFFERS_dataType, SPA_POD_Int (1 << SPA_DATA_MemPtr));

  pw_stream_update_params (audio_output_stream->pipewire_stream, params, 1);
}

static void
pipewire_stream_process (void *user_data)
{
  GrdRdpAudioOutputStream *audio_output_stream = user_data;
  struct pw_buffer *next_buffer;
  struct pw_buffer *buffer = NULL;
  int16_t *data;
  uint32_t offset;
  uint32_t size;

  next_buffer = pw_stream_dequeue_buffer (audio_output_stream->pipewire_stream);
  while (next_buffer)
    {
      buffer = next_buffer;
      next_buffer = pw_stream_dequeue_buffer (audio_output_stream->pipewire_stream);

      if (next_buffer)
        pw_stream_queue_buffer (audio_output_stream->pipewire_stream, buffer);
    }
  if (!buffer)
    return;

  offset = buffer->buffer->datas[0].chunk->offset;
  size = buffer->buffer->datas[0].chunk->size;
  data = buffer->buffer->datas[0].data + offset;

  if (size > 0)
    {
      GrdRdpAudioPlayback *audio_playback = audio_output_stream->audio_playback;
      uint32_t node_id = audio_output_stream->target_node_id;
      GrdRdpAudioVolumeData volume_data = {};

      grd_rdp_audio_output_stream_get_volume_data (audio_output_stream,
                                                   &volume_data);
      grd_rdp_audio_playback_maybe_submit_samples (audio_playback, node_id,
                                                   &volume_data, data, size);
    }

  pw_stream_queue_buffer (audio_output_stream->pipewire_stream, buffer);
}

static const struct pw_stream_events pipewire_stream_events =
{
  .version = PW_VERSION_STREAM_EVENTS,
  .state_changed = pipewire_stream_state_changed,
  .param_changed = pipewire_stream_param_changed,
  .process = pipewire_stream_process,
};

static gboolean
connect_to_stream (GrdRdpAudioOutputStream  *audio_output_stream,
                   struct pw_core           *pipewire_core,
                   uint32_t                  target_node_id,
                   uint32_t                  n_samples_per_sec,
                   uint32_t                  n_channels,
                   uint32_t                 *position,
                   GError                  **error)
{
  struct spa_pod_builder pod_builder;
  const struct spa_pod *params[1];
  uint8_t params_buffer[1024];
  int result;

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));
  params[0] = spa_pod_builder_add_object (
    &pod_builder,
    SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
    SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_audio),
    SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_AUDIO_format, SPA_POD_Id (SPA_AUDIO_FORMAT_S16),
    SPA_FORMAT_AUDIO_rate, SPA_POD_Int (n_samples_per_sec),
    SPA_FORMAT_AUDIO_channels, SPA_POD_Int (n_channels),
    SPA_FORMAT_AUDIO_position, SPA_POD_Array (sizeof (uint32_t), SPA_TYPE_Id,
                                              n_channels, position),
    0);

  audio_output_stream->pipewire_stream =
    pw_stream_new (pipewire_core,
                   "GRD::RDP::AUDIO_PLAYBACK",
                   pw_properties_new (PW_KEY_MEDIA_TYPE, "Audio",
                                      PW_KEY_MEDIA_CATEGORY, "Capture",
                                      PW_KEY_NODE_FORCE_QUANTUM, "256",
                                      NULL));
  if (!audio_output_stream->pipewire_stream)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire stream");
      return FALSE;
    }

  pw_stream_add_listener (audio_output_stream->pipewire_stream,
                          &audio_output_stream->pipewire_stream_listener,
                          &pipewire_stream_events, audio_output_stream);

  result = pw_stream_connect (audio_output_stream->pipewire_stream,
                              PW_DIRECTION_INPUT, target_node_id,
                              PW_STREAM_FLAG_RT_PROCESS |
                              PW_STREAM_FLAG_AUTOCONNECT |
                              PW_STREAM_FLAG_INACTIVE |
                              PW_STREAM_FLAG_MAP_BUFFERS,
                              params, 1);
  if (result < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (-result),
                           strerror (-result));
      return FALSE;
    }

  return TRUE;
}

GrdRdpAudioOutputStream *
grd_rdp_audio_output_stream_new (GrdRdpAudioPlayback  *audio_playback,
                                 struct pw_core       *pipewire_core,
                                 struct pw_registry   *pipewire_registry,
                                 uint32_t              target_node_id,
                                 uint32_t              n_samples_per_sec,
                                 uint32_t              n_channels,
                                 uint32_t             *position,
                                 GError              **error)
{
  g_autoptr (GrdRdpAudioOutputStream) audio_output_stream = NULL;

  audio_output_stream = g_object_new (GRD_TYPE_RDP_AUDIO_OUTPUT_STREAM, NULL);
  audio_output_stream->audio_playback = audio_playback;
  audio_output_stream->target_node_id = target_node_id;

  audio_output_stream->pipewire_node = pw_registry_bind (pipewire_registry,
                                                         target_node_id,
                                                         PW_TYPE_INTERFACE_Node,
                                                         PW_VERSION_NODE, 0);
  if (!audio_output_stream->pipewire_node)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to bind PipeWire node");
      return NULL;
    }

  pw_node_add_listener (audio_output_stream->pipewire_node,
                        &audio_output_stream->pipewire_node_listener,
                        &pipewire_node_events, audio_output_stream);

  if (!connect_to_stream (audio_output_stream, pipewire_core, target_node_id,
                          n_samples_per_sec, n_channels, position, error))
    return NULL;

  return g_steal_pointer (&audio_output_stream);
}

static void
grd_rdp_audio_output_stream_dispose (GObject *object)
{
  GrdRdpAudioOutputStream *audio_output_stream =
    GRD_RDP_AUDIO_OUTPUT_STREAM (object);

  if (audio_output_stream->pipewire_stream)
    {
      spa_hook_remove (&audio_output_stream->pipewire_stream_listener);
      g_clear_pointer (&audio_output_stream->pipewire_stream, pw_stream_destroy);
    }
  if (audio_output_stream->pipewire_node)
    {
      spa_hook_remove (&audio_output_stream->pipewire_node_listener);
      pw_proxy_destroy ((struct pw_proxy *) audio_output_stream->pipewire_node);
      audio_output_stream->pipewire_node = NULL;
    }

  G_OBJECT_CLASS (grd_rdp_audio_output_stream_parent_class)->dispose (object);
}

static void
grd_rdp_audio_output_stream_finalize (GObject *object)
{
  GrdRdpAudioOutputStream *audio_output_stream =
    GRD_RDP_AUDIO_OUTPUT_STREAM (object);

  g_mutex_clear (&audio_output_stream->volume_mutex);

  G_OBJECT_CLASS (grd_rdp_audio_output_stream_parent_class)->finalize (object);
}

static void
grd_rdp_audio_output_stream_init (GrdRdpAudioOutputStream *audio_output_stream)
{
  g_mutex_init (&audio_output_stream->volume_mutex);
}

static void
grd_rdp_audio_output_stream_class_init (GrdRdpAudioOutputStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_audio_output_stream_dispose;
  object_class->finalize = grd_rdp_audio_output_stream_finalize;
}
