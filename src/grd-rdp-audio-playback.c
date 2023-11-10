/*
 * Copyright (C) 2021 Pascal Nowack
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

#include "grd-rdp-audio-playback.h"

#include "grd-pipewire-utils.h"
#include "grd-rdp-audio-output-stream.h"
#include "grd-rdp-dsp.h"
#include "grd-rdp-dvc.h"
#include "grd-session-rdp.h"

#define PROTOCOL_TIMEOUT_MS (10 * 1000)

#define PCM_FRAMES_PER_PACKET 1024
#define MAX_IDLING_TIME_US_DEFAULT (5 * G_USEC_PER_SEC)
#define MAX_IDLING_TIME_US_OPUS (10 * G_USEC_PER_SEC)
#define MAX_LOCAL_FRAMES_LIFETIME_US (50 * 1000)
#define MAX_RENDER_LATENCY_MS 300

#define N_CHANNELS 2
#define N_SAMPLES_PER_SEC_DEFAULT 44100
#define N_SAMPLES_PER_SEC_OPUS 48000
#define N_BYTES_PER_SAMPLE_PCM sizeof (int16_t)
#define N_BLOCK_ALIGN_PCM (N_CHANNELS * N_BYTES_PER_SAMPLE_PCM)

#define TRAINING_PACKSIZE 1024
#define TRAINING_TIMESTAMP 0

typedef struct _AudioData
{
  int16_t *data;
  uint32_t size;
  int64_t timestamp_us;
} AudioData;

typedef struct _BlockInfo
{
  uint16_t render_latency_ms;
  int64_t render_latency_set_us;
} BlockInfo;

struct _GrdRdpAudioPlayback
{
  GObject parent;

  RdpsndServerContext *rdpsnd_context;
  gboolean channel_opened;
  gboolean channel_unavailable;

  uint32_t channel_id;
  uint32_t dvc_subscription_id;
  gboolean subscribed_status;

  GrdSessionRdp *session_rdp;
  GrdRdpDvc *rdp_dvc;

  gboolean prevent_dvc_initialization;
  GSource *svc_setup_source;

  GMutex protocol_timeout_mutex;
  GSource *channel_teardown_source;
  GSource *protocol_timeout_source;

  gboolean pending_client_formats;
  gboolean pending_training_confirm;
  gboolean protocol_stopped;

  int32_t aac_client_format_idx;
  int32_t opus_client_format_idx;
  int32_t pcm_client_format_idx;
  int32_t client_format_idx;
  uint32_t n_samples_per_sec;
  GrdRdpDspCodec codec;

  GThread *encode_thread;
  GMainContext *encode_context;
  GSource *encode_source;

  GrdRdpDsp *rdp_dsp;
  uint32_t sample_buffer_size;

  GMutex block_mutex;
  BlockInfo block_infos[256];

  GSource *pipewire_setup_source;
  GSource *pipewire_source;

  struct pw_context *pipewire_context;
  struct pw_core *pipewire_core;
  struct spa_hook pipewire_core_listener;

  struct pw_registry *pipewire_registry;
  struct spa_hook pipewire_registry_listener;

  GMutex streams_mutex;
  GHashTable *audio_streams;

  GMutex stream_lock_mutex;
  uint32_t locked_node_id;
  gboolean has_stream_lock;

  int64_t first_empty_audio_data_us;
  gboolean has_empty_audio_timestamp;

  GMutex pending_frames_mutex;
  GQueue *pending_frames;
};

G_DEFINE_TYPE (GrdRdpAudioPlayback, grd_rdp_audio_playback, G_TYPE_OBJECT)

static gboolean
initiate_channel_teardown (gpointer user_data)
{
  GrdRdpAudioPlayback *audio_playback = user_data;

  g_warning ("[RDP.AUDIO_PLAYBACK] Client did not respond to protocol "
             "initiation. Terminating protocol");

  g_mutex_lock (&audio_playback->protocol_timeout_mutex);
  g_clear_pointer (&audio_playback->protocol_timeout_source, g_source_unref);
  g_mutex_unlock (&audio_playback->protocol_timeout_mutex);

  g_source_set_ready_time (audio_playback->channel_teardown_source, 0);

  return G_SOURCE_REMOVE;
}

void
grd_rdp_audio_playback_maybe_init (GrdRdpAudioPlayback *audio_playback)
{
  RdpsndServerContext *rdpsnd_context;

  if (audio_playback->channel_opened || audio_playback->channel_unavailable)
    return;

  if (audio_playback->prevent_dvc_initialization)
    return;

  rdpsnd_context = audio_playback->rdpsnd_context;
  if (rdpsnd_context->Start (rdpsnd_context))
    {
      g_warning ("[RDP.AUDIO_PLAYBACK] Failed to open AUDIO_PLAYBACK_DVC "
                 "channel. Terminating protocol");
      audio_playback->channel_unavailable = TRUE;
      g_source_set_ready_time (audio_playback->channel_teardown_source, 0);
      return;
    }
  audio_playback->channel_opened = TRUE;

  g_assert (!audio_playback->protocol_timeout_source);

  audio_playback->protocol_timeout_source =
    g_timeout_source_new (PROTOCOL_TIMEOUT_MS);
  g_source_set_callback (audio_playback->protocol_timeout_source,
                         initiate_channel_teardown, audio_playback, NULL);
  g_source_attach (audio_playback->protocol_timeout_source, NULL);
}

static void
prepare_volume_data (GrdRdpAudioVolumeData *volume_data)
{
  uint32_t i;

  g_assert (N_CHANNELS <= SPA_AUDIO_MAX_CHANNELS);

  if (volume_data->audio_muted)
    {
      for (i = 0; i < volume_data->n_volumes; ++i)
        volume_data->volumes[i] = 0.0f;
    }
  else if (volume_data->n_volumes == 0)
    {
      for (i = 0; i < N_CHANNELS; ++i)
        volume_data->volumes[i] = 0.0f;
    }
  else if (N_CHANNELS > volume_data->n_volumes)
    {
      g_assert (N_CHANNELS == 2);
      g_assert (volume_data->n_volumes == 1);

      for (i = 1; i < N_CHANNELS; ++i)
        volume_data->volumes[i] = volume_data->volumes[0];
    }
}

static gboolean
does_volume_mute_audio (const GrdRdpAudioVolumeData *volume_data)
{
  uint32_t i;

  if (volume_data->audio_muted)
    return TRUE;

  for (i = 0; i < N_CHANNELS; ++i)
    {
      if (!G_APPROX_VALUE (volume_data->volumes[i], 0.0f, FLT_EPSILON))
        return FALSE;
    }

  return TRUE;
}

static gboolean
is_audio_data_empty (int16_t  *data,
                     uint32_t  size)
{
  uint32_t i;

  for (i = 0; i < size / sizeof (int16_t); ++i)
    {
      if (data[i] != 0)
        return FALSE;
    }

  return TRUE;
}

static void
set_other_streams_inactive (GrdRdpAudioPlayback *audio_playback)
{
  GrdRdpAudioOutputStream *audio_output_stream;
  gpointer key;
  GHashTableIter iter;

  g_assert (audio_playback->has_stream_lock);
  g_mutex_unlock (&audio_playback->stream_lock_mutex);

  g_mutex_lock (&audio_playback->streams_mutex);
  g_hash_table_iter_init (&iter, audio_playback->audio_streams);
  while (g_hash_table_iter_next (&iter, &key, (gpointer *) &audio_output_stream))
    {
      uint32_t node_id;

      node_id = GPOINTER_TO_UINT (key);
      if (audio_playback->locked_node_id != node_id)
        grd_rdp_audio_output_stream_set_active (audio_output_stream, FALSE);
    }
  g_mutex_unlock (&audio_playback->streams_mutex);

  g_mutex_lock (&audio_playback->stream_lock_mutex);
}

static void
acquire_stream_lock (GrdRdpAudioPlayback *audio_playback,
                     uint32_t             node_id)
{
  g_assert (!audio_playback->has_stream_lock);

  g_debug ("[RDP.AUDIO_PLAYBACK] Locking audio stream to node id %u", node_id);

  audio_playback->locked_node_id = node_id;
  audio_playback->has_stream_lock = TRUE;

  set_other_streams_inactive (audio_playback);
}

static void
audio_data_free (gpointer data)
{
  AudioData *audio_data = data;

  g_free (audio_data->data);
  g_free (audio_data);
}

static void
set_all_streams_active (GrdRdpAudioPlayback *audio_playback)
{
  GrdRdpAudioOutputStream *audio_output_stream;
  GHashTableIter iter;

  g_assert (!audio_playback->has_stream_lock);

  g_mutex_lock (&audio_playback->streams_mutex);
  g_hash_table_iter_init (&iter, audio_playback->audio_streams);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &audio_output_stream))
    grd_rdp_audio_output_stream_set_active (audio_output_stream, TRUE);
  g_mutex_unlock (&audio_playback->streams_mutex);
}

static void
release_stream_lock (GrdRdpAudioPlayback *audio_playback)
{
  uint16_t i;

  g_assert (audio_playback->has_stream_lock);

  g_debug ("[RDP.AUDIO_PLAYBACK] Unlocking audio stream (was locked to "
           "node id %u)", audio_playback->locked_node_id);

  g_mutex_lock (&audio_playback->pending_frames_mutex);
  g_queue_clear_full (audio_playback->pending_frames, audio_data_free);
  g_mutex_unlock (&audio_playback->pending_frames_mutex);

  g_mutex_lock (&audio_playback->block_mutex);
  for (i = 0; i < 256; ++i)
    {
      BlockInfo *block_info = &audio_playback->block_infos[i];

      block_info->render_latency_ms = 0;
    }
  g_mutex_unlock (&audio_playback->block_mutex);

  audio_playback->has_stream_lock = FALSE;

  set_all_streams_active (audio_playback);
}

static gboolean
is_locked_stream_idling_too_long (GrdRdpAudioPlayback *audio_playback)
{
  int64_t first_empty_audio_data_us = audio_playback->first_empty_audio_data_us;
  int64_t max_idling_time_us;
  int64_t current_time_us;

  if (!audio_playback->has_empty_audio_timestamp)
    return FALSE;

  if (audio_playback->codec == GRD_RDP_DSP_CODEC_OPUS)
    max_idling_time_us = MAX_IDLING_TIME_US_OPUS;
  else
    max_idling_time_us = MAX_IDLING_TIME_US_DEFAULT;

  current_time_us = g_get_monotonic_time ();
  if (current_time_us - first_empty_audio_data_us > max_idling_time_us)
    return TRUE;

  return FALSE;
}

static uint32_t
get_pending_audio_data_size (GrdRdpAudioPlayback *audio_playback)
{
  AudioData *audio_data;
  uint32_t pending_size = 0;
  GQueue *tmp;

  tmp = g_queue_copy (audio_playback->pending_frames);
  while ((audio_data = g_queue_pop_head (tmp)))
    pending_size += audio_data->size;

  g_queue_free (tmp);

  return pending_size;
}

void
grd_rdp_audio_playback_maybe_submit_samples (GrdRdpAudioPlayback   *audio_playback,
                                             uint32_t               node_id,
                                             GrdRdpAudioVolumeData *volume_data,
                                             int16_t               *data,
                                             uint32_t               size)
{
  g_autoptr (GMutexLocker) locker = NULL;
  gboolean audio_muted;
  gboolean data_is_empty;
  uint32_t pending_size = 0;
  gboolean pending_encode = FALSE;

  g_assert (N_BLOCK_ALIGN_PCM > 0);

  prepare_volume_data (volume_data);
  audio_muted = does_volume_mute_audio (volume_data);

  data_is_empty = audio_muted || is_audio_data_empty (data, size);

  locker = g_mutex_locker_new (&audio_playback->stream_lock_mutex);
  if (audio_playback->has_stream_lock &&
      audio_playback->locked_node_id != node_id)
    return;

  if (!audio_playback->has_stream_lock && data_is_empty)
    return;

  if (!audio_playback->has_stream_lock)
    acquire_stream_lock (audio_playback, node_id);

  if (size < N_BLOCK_ALIGN_PCM ||
      size % N_BLOCK_ALIGN_PCM != 0)
    {
      release_stream_lock (audio_playback);
      return;
    }

  if (data_is_empty && is_locked_stream_idling_too_long (audio_playback))
    {
      release_stream_lock (audio_playback);
      return;
    }
  else if (data_is_empty && !audio_playback->has_empty_audio_timestamp)
    {
      audio_playback->first_empty_audio_data_us = g_get_monotonic_time ();
      audio_playback->has_empty_audio_timestamp = TRUE;
    }

  if (!data_is_empty)
    audio_playback->has_empty_audio_timestamp = FALSE;

  g_mutex_lock (&audio_playback->pending_frames_mutex);
  pending_size = get_pending_audio_data_size (audio_playback);

  if (!data_is_empty ||
      (pending_size > 0 && pending_size < audio_playback->sample_buffer_size))
    {
      AudioData *audio_data;

      audio_data = g_new0 (AudioData, 1);
      audio_data->data = g_memdup2 (data, size);
      audio_data->size = size;
      audio_data->timestamp_us = g_get_monotonic_time ();

      g_queue_push_tail (audio_playback->pending_frames, audio_data);

      if (pending_size + size >= audio_playback->sample_buffer_size)
        pending_encode = TRUE;
    }
  g_mutex_unlock (&audio_playback->pending_frames_mutex);

  if (pending_encode)
    g_source_set_ready_time (audio_playback->encode_source, 0);
}

static void
dvc_creation_status (gpointer user_data,
                     int32_t  creation_status)
{
  GrdRdpAudioPlayback *audio_playback = user_data;

  if (creation_status < 0)
    {
      g_message ("[RDP.AUDIO_PLAYBACK] Failed to open AUDIO_PLAYBACK_DVC "
                 "channel (CreationStatus %i). Trying SVC fallback",
                 creation_status);
      g_source_set_ready_time (audio_playback->svc_setup_source, 0);
    }
}

static BOOL
rdpsnd_channel_id_assigned (RdpsndServerContext *rdpsnd_context,
                            uint32_t             channel_id)
{
  GrdRdpAudioPlayback *audio_playback = rdpsnd_context->data;

  g_debug ("[RDP.AUDIO_PLAYBACK] DVC channel id assigned to id %u", channel_id);
  audio_playback->channel_id = channel_id;

  audio_playback->dvc_subscription_id =
    grd_rdp_dvc_subscribe_dvc_creation_status (audio_playback->rdp_dvc,
                                               channel_id,
                                               dvc_creation_status,
                                               audio_playback);
  audio_playback->subscribed_status = TRUE;

  return TRUE;
}

static gboolean
is_audio_format_data_equal (const AUDIO_FORMAT *audio_format_1,
                            const AUDIO_FORMAT *audio_format_2)
{
  uint32_t i;

  if (audio_format_1->cbSize != audio_format_2->cbSize)
    return FALSE;

  for (i = 0; i < audio_format_1->cbSize; ++i)
    {
      if (audio_format_1->data[i] != audio_format_2->data[i])
        return FALSE;
    }

  return TRUE;
}

static gboolean
are_audio_formats_equal (const AUDIO_FORMAT *audio_format_1,
                         const AUDIO_FORMAT *audio_format_2)
{
  if (audio_format_1->wFormatTag == audio_format_2->wFormatTag &&
      audio_format_1->nChannels == audio_format_2->nChannels &&
      audio_format_1->nSamplesPerSec == audio_format_2->nSamplesPerSec &&
      audio_format_1->nAvgBytesPerSec == audio_format_2->nAvgBytesPerSec &&
      audio_format_1->nBlockAlign == audio_format_2->nBlockAlign &&
      audio_format_1->wBitsPerSample == audio_format_2->wBitsPerSample &&
      is_audio_format_data_equal (audio_format_1, audio_format_2))
    return TRUE;

  return FALSE;
}

static const AUDIO_FORMAT audio_format_aac =
{
  .wFormatTag = WAVE_FORMAT_AAC_MS,
  .nChannels = N_CHANNELS,
  .nSamplesPerSec = N_SAMPLES_PER_SEC_DEFAULT,
  .nAvgBytesPerSec = 12000,
  .nBlockAlign = 4,
  .wBitsPerSample = 16,
  .cbSize = 0,
};

static const AUDIO_FORMAT audio_format_opus =
{
  .wFormatTag = WAVE_FORMAT_OPUS,
  .nChannels = N_CHANNELS,
  .nSamplesPerSec = N_SAMPLES_PER_SEC_OPUS,
  .nAvgBytesPerSec = 12000,
  .nBlockAlign = 4,
  .wBitsPerSample = 16,
  .cbSize = 0,
};

static const AUDIO_FORMAT audio_format_pcm =
{
  .wFormatTag = WAVE_FORMAT_PCM,
  .nChannels = N_CHANNELS,
  .nSamplesPerSec = N_SAMPLES_PER_SEC_DEFAULT,
  .nAvgBytesPerSec = N_SAMPLES_PER_SEC_DEFAULT * N_BLOCK_ALIGN_PCM,
  .nBlockAlign = N_BLOCK_ALIGN_PCM,
  .wBitsPerSample = N_BYTES_PER_SAMPLE_PCM * 8,
  .cbSize = 0,
};

static AUDIO_FORMAT server_formats[] =
{
  audio_format_aac,
  audio_format_opus,
  audio_format_pcm,
};

static uint32_t
get_sample_rate (GrdRdpAudioPlayback *audio_playback)
{
  switch (audio_playback->codec)
    {
    case GRD_RDP_DSP_CODEC_NONE:
    case GRD_RDP_DSP_CODEC_AAC:
    case GRD_RDP_DSP_CODEC_ALAW:
      return N_SAMPLES_PER_SEC_DEFAULT;
    case GRD_RDP_DSP_CODEC_OPUS:
      return N_SAMPLES_PER_SEC_OPUS;
    }

  g_assert_not_reached ();
}

static void
prepare_client_format (GrdRdpAudioPlayback *audio_playback)
{
  uint32_t frames_per_packet;

  if (audio_playback->aac_client_format_idx >= 0)
    {
      audio_playback->client_format_idx = audio_playback->aac_client_format_idx;
      audio_playback->codec = GRD_RDP_DSP_CODEC_AAC;
    }
  else if (audio_playback->opus_client_format_idx >= 0)
    {
      audio_playback->client_format_idx = audio_playback->opus_client_format_idx;
      audio_playback->codec = GRD_RDP_DSP_CODEC_OPUS;
    }
  else if (audio_playback->pcm_client_format_idx >= 0)
    {
      audio_playback->client_format_idx = audio_playback->pcm_client_format_idx;
      audio_playback->codec = GRD_RDP_DSP_CODEC_NONE;
    }
  else
    {
      g_assert_not_reached ();
    }

  audio_playback->n_samples_per_sec = get_sample_rate (audio_playback);

  switch (audio_playback->codec)
    {
    case GRD_RDP_DSP_CODEC_NONE:
      frames_per_packet = PCM_FRAMES_PER_PACKET;
      break;
    case GRD_RDP_DSP_CODEC_AAC:
    case GRD_RDP_DSP_CODEC_ALAW:
    case GRD_RDP_DSP_CODEC_OPUS:
      frames_per_packet =
        grd_rdp_dsp_get_frames_per_packet (audio_playback->rdp_dsp,
                                           audio_playback->codec);
      break;
    }
  g_assert (frames_per_packet > 0);

  audio_playback->sample_buffer_size = N_BLOCK_ALIGN_PCM * frames_per_packet;

  g_debug ("[RDP.AUDIO_PLAYBACK] Using codec %s with sample buffer size %u",
           grd_rdp_dsp_codec_to_string (audio_playback->codec),
           audio_playback->sample_buffer_size);
}

static void
rdpsnd_activated (RdpsndServerContext *rdpsnd_context)
{
  GrdRdpAudioPlayback *audio_playback = rdpsnd_context->data;
  uint8_t training_data[TRAINING_PACKSIZE] = {};
  uint16_t i;

  if (!audio_playback->pending_client_formats)
    {
      g_warning ("[RDP.AUDIO_PLAYBACK] Protocol violation: Received stray "
                 "Client Audio Formats or Quality Mode PDU. "
                 "Terminating protocol");
      g_source_set_ready_time (audio_playback->channel_teardown_source, 0);
      return;
    }

  if (rdpsnd_context->clientVersion < 8)
    {
      g_message ("[RDP.AUDIO_PLAYBACK] Client protocol version (%u) too old. "
                 "Terminating protocol", rdpsnd_context->clientVersion);
      g_source_set_ready_time (audio_playback->channel_teardown_source, 0);
      return;
    }

  for (i = 0; i < rdpsnd_context->num_client_formats; ++i)
    {
      AUDIO_FORMAT *audio_format = &rdpsnd_context->client_formats[i];

      if (audio_playback->aac_client_format_idx < 0 &&
          are_audio_formats_equal (audio_format, &audio_format_aac))
        audio_playback->aac_client_format_idx = i;
      if (audio_playback->opus_client_format_idx < 0 &&
          are_audio_formats_equal (audio_format, &audio_format_opus))
        audio_playback->opus_client_format_idx = i;
      if (audio_playback->pcm_client_format_idx < 0 &&
          are_audio_formats_equal (audio_format, &audio_format_pcm))
        audio_playback->pcm_client_format_idx = i;
    }

  if (audio_playback->aac_client_format_idx < 0 &&
      audio_playback->opus_client_format_idx < 0 &&
      audio_playback->pcm_client_format_idx < 0)
    {
      g_warning ("[RDP.AUDIO_PLAYBACK] Audio Format negotiation with client "
                 "failed. Terminating protocol");
      g_source_set_ready_time (audio_playback->channel_teardown_source, 0);
      return;
    }
  audio_playback->pending_client_formats = FALSE;

  g_message ("[RDP.AUDIO_PLAYBACK] Client Formats: [AAC: %s, Opus: %s, PCM: %s]",
             audio_playback->aac_client_format_idx >= 0 ? "true" : "false",
             audio_playback->opus_client_format_idx >= 0 ? "true" : "false",
             audio_playback->pcm_client_format_idx >= 0 ? "true" : "false");

  prepare_client_format (audio_playback);

  rdpsnd_context->Training (rdpsnd_context, TRAINING_TIMESTAMP,
                            TRAINING_PACKSIZE, training_data);
}

static uint32_t
rdpsnd_training_confirm (RdpsndServerContext *rdpsnd_context,
                         uint16_t             timestamp,
                         uint16_t             packsize)
{
  GrdRdpAudioPlayback *audio_playback = rdpsnd_context->data;

  if (audio_playback->pending_client_formats ||
      !audio_playback->pending_training_confirm)
    {
      g_warning ("[RDP.AUDIO_PLAYBACK] Protocol violation: Received unexpected "
                 "Training Confirm PDU. Terminating protocol");
      g_source_set_ready_time (audio_playback->channel_teardown_source, 0);
      return CHANNEL_RC_OK;
    }

  if (timestamp != TRAINING_TIMESTAMP || packsize != TRAINING_PACKSIZE)
    {
      g_warning ("[RDP.AUDIO_PLAYBACK] Received invalid Training Confirm PDU. "
                 "Ignoring...");
      return CHANNEL_RC_OK;
    }

  g_debug ("[RDP.AUDIO_PLAYBACK] Received Training Confirm PDU");

  g_mutex_lock (&audio_playback->protocol_timeout_mutex);
  if (audio_playback->protocol_timeout_source)
    {
      g_source_destroy (audio_playback->protocol_timeout_source);
      g_clear_pointer (&audio_playback->protocol_timeout_source, g_source_unref);
    }
  g_mutex_unlock (&audio_playback->protocol_timeout_mutex);

  if (audio_playback->svc_setup_source)
    {
      g_source_destroy (audio_playback->svc_setup_source);
      g_clear_pointer (&audio_playback->svc_setup_source, g_source_unref);
    }

  audio_playback->pending_training_confirm = FALSE;

  g_source_set_ready_time (audio_playback->pipewire_setup_source, 0);

  return CHANNEL_RC_OK;
}

static uint32_t
rdpsnd_confirm_block (RdpsndServerContext *rdpsnd_context,
                      uint8_t              confirm_block_num,
                      uint16_t             wtimestamp)
{
  GrdRdpAudioPlayback *audio_playback = rdpsnd_context->data;
  BlockInfo *block_info;

  g_mutex_lock (&audio_playback->block_mutex);
  block_info = &audio_playback->block_infos[confirm_block_num];

  if (wtimestamp > 0)
    {
      block_info->render_latency_ms = wtimestamp;
      block_info->render_latency_set_us = g_get_monotonic_time ();
    }
  g_mutex_unlock (&audio_playback->block_mutex);

  return CHANNEL_RC_OK;
}

static gpointer
encode_thread_func (gpointer data)
{
  GrdRdpAudioPlayback *audio_playback = data;

  while (!audio_playback->protocol_stopped)
    g_main_context_iteration (audio_playback->encode_context, TRUE);

  return NULL;
}

GrdRdpAudioPlayback *
grd_rdp_audio_playback_new (GrdSessionRdp *session_rdp,
                            GrdRdpDvc     *rdp_dvc,
                            HANDLE         vcm,
                            rdpContext    *rdp_context)
{
  g_autoptr (GrdRdpAudioPlayback) audio_playback = NULL;
  RdpsndServerContext *rdpsnd_context;
  GrdRdpDspDescriptor dsp_descriptor = {};
  g_autoptr (GError) error = NULL;

  audio_playback = g_object_new (GRD_TYPE_RDP_AUDIO_PLAYBACK, NULL);
  rdpsnd_context = rdpsnd_server_context_new (vcm);
  if (!rdpsnd_context)
    g_error ("[RDP.AUDIO_PLAYBACK] Failed to create server context");

  audio_playback->rdpsnd_context = rdpsnd_context;
  audio_playback->session_rdp = session_rdp;
  audio_playback->rdp_dvc = rdp_dvc;

  rdpsnd_context->use_dynamic_virtual_channel = TRUE;
  rdpsnd_context->server_formats = server_formats;
  rdpsnd_context->num_server_formats = G_N_ELEMENTS (server_formats);

  rdpsnd_context->ChannelIdAssigned = rdpsnd_channel_id_assigned;
  rdpsnd_context->Activated = rdpsnd_activated;
  rdpsnd_context->TrainingConfirm = rdpsnd_training_confirm;
  rdpsnd_context->ConfirmBlock = rdpsnd_confirm_block;
  rdpsnd_context->rdpcontext = rdp_context;
  rdpsnd_context->data = audio_playback;

  pw_init (NULL, NULL);

  dsp_descriptor.create_flags = GRD_RDP_DSP_CREATE_FLAG_ENCODER;
  dsp_descriptor.n_samples_per_sec_aac = N_SAMPLES_PER_SEC_DEFAULT;
  dsp_descriptor.n_samples_per_sec_opus = N_SAMPLES_PER_SEC_OPUS;
  dsp_descriptor.n_channels = N_CHANNELS;
  dsp_descriptor.bitrate_aac = audio_format_aac.nAvgBytesPerSec * 8;
  dsp_descriptor.bitrate_opus = audio_format_opus.nAvgBytesPerSec * 8;

  audio_playback->rdp_dsp = grd_rdp_dsp_new (&dsp_descriptor, &error);
  if (!audio_playback->rdp_dsp)
    {
      g_warning ("[RDP.AUDIO_PLAYBACK] Failed to create DSP instance: %s",
                 error->message);
      return NULL;
    }

  audio_playback->encode_thread = g_thread_new ("RDP audio encode thread",
                                                encode_thread_func,
                                                audio_playback);

  return g_steal_pointer (&audio_playback);
}

static void
grd_rdp_audio_playback_dispose (GObject *object)
{
  GrdRdpAudioPlayback *audio_playback = GRD_RDP_AUDIO_PLAYBACK (object);

  audio_playback->protocol_stopped = TRUE;
  if (audio_playback->encode_thread)
    {
      g_main_context_wakeup (audio_playback->encode_context);
      g_clear_pointer (&audio_playback->encode_thread, g_thread_join);
    }

  if (audio_playback->channel_opened)
    {
      audio_playback->rdpsnd_context->Stop (audio_playback->rdpsnd_context);
      audio_playback->channel_opened = FALSE;
    }
  if (audio_playback->subscribed_status)
    {
      grd_rdp_dvc_unsubscribe_dvc_creation_status (audio_playback->rdp_dvc,
                                                   audio_playback->channel_id,
                                                   audio_playback->dvc_subscription_id);
      audio_playback->subscribed_status = FALSE;
    }

  if (audio_playback->rdpsnd_context)
    audio_playback->rdpsnd_context->server_formats = NULL;

  g_mutex_lock (&audio_playback->streams_mutex);
  g_hash_table_remove_all (audio_playback->audio_streams);
  g_mutex_unlock (&audio_playback->streams_mutex);

  if (audio_playback->pipewire_registry)
    {
      spa_hook_remove (&audio_playback->pipewire_registry_listener);
      pw_proxy_destroy ((struct pw_proxy *) audio_playback->pipewire_registry);
      audio_playback->pipewire_registry = NULL;
    }
  if (audio_playback->pipewire_core)
    {
      spa_hook_remove (&audio_playback->pipewire_core_listener);
      g_clear_pointer (&audio_playback->pipewire_core, pw_core_disconnect);
    }
  g_clear_pointer (&audio_playback->pipewire_context, pw_context_destroy);

  if (audio_playback->pipewire_source)
    {
      g_source_destroy (audio_playback->pipewire_source);
      g_clear_pointer (&audio_playback->pipewire_source, g_source_unref);
    }

  g_clear_object (&audio_playback->rdp_dsp);

  if (audio_playback->protocol_timeout_source)
    {
      g_source_destroy (audio_playback->protocol_timeout_source);
      g_clear_pointer (&audio_playback->protocol_timeout_source, g_source_unref);
    }
  if (audio_playback->pipewire_setup_source)
    {
      g_source_destroy (audio_playback->pipewire_setup_source);
      g_clear_pointer (&audio_playback->pipewire_setup_source, g_source_unref);
    }
  if (audio_playback->encode_source)
    {
      g_source_destroy (audio_playback->encode_source);
      g_clear_pointer (&audio_playback->encode_source, g_source_unref);
    }
  if (audio_playback->channel_teardown_source)
    {
      g_source_destroy (audio_playback->channel_teardown_source);
      g_clear_pointer (&audio_playback->channel_teardown_source, g_source_unref);
    }
  if (audio_playback->svc_setup_source)
    {
      g_source_destroy (audio_playback->svc_setup_source);
      g_clear_pointer (&audio_playback->svc_setup_source, g_source_unref);
    }

  g_clear_pointer (&audio_playback->encode_context, g_main_context_unref);

  if (audio_playback->pending_frames)
    {
      g_queue_free_full (audio_playback->pending_frames, audio_data_free);
      audio_playback->pending_frames = NULL;
    }

  g_clear_pointer (&audio_playback->rdpsnd_context, rdpsnd_server_context_free);

  G_OBJECT_CLASS (grd_rdp_audio_playback_parent_class)->dispose (object);
}

static void
grd_rdp_audio_playback_finalize (GObject *object)
{
  GrdRdpAudioPlayback *audio_playback = GRD_RDP_AUDIO_PLAYBACK (object);

  g_mutex_clear (&audio_playback->pending_frames_mutex);
  g_mutex_clear (&audio_playback->stream_lock_mutex);
  g_mutex_clear (&audio_playback->streams_mutex);
  g_mutex_clear (&audio_playback->block_mutex);
  g_mutex_clear (&audio_playback->protocol_timeout_mutex);

  g_assert (g_hash_table_size (audio_playback->audio_streams) == 0);
  g_clear_pointer (&audio_playback->audio_streams, g_hash_table_unref);

  pw_deinit ();

  G_OBJECT_CLASS (grd_rdp_audio_playback_parent_class)->finalize (object);
}

static gboolean
set_up_static_virtual_channel (gpointer user_data)
{
  GrdRdpAudioPlayback *audio_playback = user_data;
  RdpsndServerContext *rdpsnd_context = audio_playback->rdpsnd_context;

  g_clear_pointer (&audio_playback->svc_setup_source, g_source_unref);

  g_assert (audio_playback->channel_opened);
  audio_playback->prevent_dvc_initialization = TRUE;

  rdpsnd_context->Stop (rdpsnd_context);
  audio_playback->channel_opened = FALSE;

  if (audio_playback->subscribed_status)
    {
      grd_rdp_dvc_unsubscribe_dvc_creation_status (audio_playback->rdp_dvc,
                                                   audio_playback->channel_id,
                                                   audio_playback->dvc_subscription_id);
      audio_playback->subscribed_status = FALSE;
    }

  rdpsnd_context->use_dynamic_virtual_channel = FALSE;

  if (rdpsnd_context->Start (rdpsnd_context))
    {
      g_warning ("[RDP.AUDIO_PLAYBACK] Failed to open RDPSND channel. "
                 "Terminating protocol");
      audio_playback->channel_unavailable = TRUE;
      g_source_set_ready_time (audio_playback->channel_teardown_source, 0);
      return G_SOURCE_REMOVE;
    }
  audio_playback->channel_opened = TRUE;

  return G_SOURCE_REMOVE;
}

static gboolean
tear_down_channel (gpointer user_data)
{
  GrdRdpAudioPlayback *audio_playback = user_data;

  g_debug ("[RDP.AUDIO_PLAYBACK] Tearing down channel");

  g_clear_pointer (&audio_playback->channel_teardown_source, g_source_unref);

  grd_session_rdp_tear_down_channel (audio_playback->session_rdp,
                                     GRD_RDP_CHANNEL_AUDIO_PLAYBACK);

  return G_SOURCE_REMOVE;
}

static void
clear_old_frames (GrdRdpAudioPlayback *audio_playback)
{
  int64_t current_time_us;
  AudioData *audio_data;
  GQueue *tmp;

  current_time_us = g_get_monotonic_time ();
  tmp = g_queue_copy (audio_playback->pending_frames);

  while ((audio_data = g_queue_pop_head (tmp)))
    {
      if (current_time_us - audio_data->timestamp_us > MAX_LOCAL_FRAMES_LIFETIME_US)
        audio_data_free (g_queue_pop_head (audio_playback->pending_frames));
      else
        break;
    }

  g_queue_free (tmp);
}

static uint32_t
get_current_render_latency (GrdRdpAudioPlayback *audio_playback)
{
  uint32_t accumulated_render_latency_ms = 0;
  uint32_t n_latencies = 0;
  int64_t current_time_us;
  uint16_t i;

  current_time_us = g_get_monotonic_time ();

  g_mutex_lock (&audio_playback->block_mutex);
  for (i = 0; i < 256; ++i)
    {
      BlockInfo *block_info = &audio_playback->block_infos[i];

      if (block_info->render_latency_ms > 0 &&
          current_time_us - block_info->render_latency_set_us < G_USEC_PER_SEC)
        {
          accumulated_render_latency_ms += block_info->render_latency_ms;
          ++n_latencies;
        }
    }
  g_mutex_unlock (&audio_playback->block_mutex);

  if (n_latencies == 0)
    return 0;

  return accumulated_render_latency_ms / n_latencies;
}

static void
maybe_drop_pending_frames (GrdRdpAudioPlayback *audio_playback)
{
  uint32_t render_latency_ms;
  uint16_t i;

  render_latency_ms = get_current_render_latency (audio_playback);
  if (render_latency_ms <= MAX_RENDER_LATENCY_MS)
    return;

  g_queue_clear_full (audio_playback->pending_frames, audio_data_free);

  g_mutex_lock (&audio_playback->block_mutex);
  for (i = 0; i < 256; ++i)
    {
      BlockInfo *block_info = &audio_playback->block_infos[i];

      block_info->render_latency_ms = 0;
    }
  g_mutex_unlock (&audio_playback->block_mutex);
}

static void
apply_volume_to_audio_data (GrdRdpAudioPlayback         *audio_playback,
                            const GrdRdpAudioVolumeData *volume_data,
                            int16_t                     *data,
                            uint32_t                     frames)
{
  uint32_t i, j;

  g_assert (N_CHANNELS <= SPA_AUDIO_MAX_CHANNELS);

  for (i = 0; i < frames; ++i)
    {
      for (j = 0; j < N_CHANNELS; ++j)
        {
          float pcm;

          pcm = volume_data->volumes[j] * data[i * N_CHANNELS + j];
          data[i * N_CHANNELS + j] = pcm;
        }
    }
}

static void
maybe_send_frames (GrdRdpAudioPlayback         *audio_playback,
                   const GrdRdpAudioVolumeData *volume_data)
{
  RdpsndServerContext *rdpsnd_context = audio_playback->rdpsnd_context;
  int32_t client_format_idx = audio_playback->client_format_idx;
  GrdRdpDspCodec codec = audio_playback->codec;
  g_autofree uint8_t *raw_data = NULL;
  uint32_t raw_data_size = 0;
  uint32_t copied_data = 0;
  g_autofree uint8_t *out_data = NULL;
  uint32_t out_size = 0;
  gboolean success = FALSE;
  BlockInfo *block_info;

  g_assert (N_BLOCK_ALIGN_PCM > 0);
  g_assert (audio_playback->sample_buffer_size > 0);
  g_assert (audio_playback->sample_buffer_size % N_BLOCK_ALIGN_PCM == 0);
  g_assert (!audio_playback->pending_training_confirm);

  raw_data_size = get_pending_audio_data_size (audio_playback);
  if (raw_data_size < audio_playback->sample_buffer_size)
    return;

  g_assert (raw_data_size > 0);
  raw_data = g_malloc0 (audio_playback->sample_buffer_size);

  while (copied_data < audio_playback->sample_buffer_size)
    {
      AudioData *audio_data;
      uint32_t sample_size;
      uint32_t frames_taken;

      audio_data = g_queue_pop_head (audio_playback->pending_frames);

      g_assert (audio_data);
      g_assert (audio_data->size % N_BLOCK_ALIGN_PCM == 0);

      sample_size = N_BLOCK_ALIGN_PCM;
      frames_taken = audio_data->size / sample_size;

      g_assert (audio_data->size >= N_BLOCK_ALIGN_PCM);

      if (copied_data + audio_data->size > audio_playback->sample_buffer_size)
        {
          AudioData *remaining_audio_data;
          uint32_t processed_size;
          uint32_t remaining_size;

          processed_size = audio_playback->sample_buffer_size - copied_data;
          g_assert (processed_size % N_BLOCK_ALIGN_PCM == 0);

          frames_taken = processed_size / sample_size;

          remaining_size = audio_data->size - processed_size;
          if (remaining_size >= N_BLOCK_ALIGN_PCM)
            {
              remaining_audio_data = g_new0 (AudioData, 1);
              remaining_audio_data->data =
                g_memdup2 (((uint8_t *) audio_data->data) + processed_size,
                           remaining_size);
              remaining_audio_data->size = remaining_size;
              remaining_audio_data->timestamp_us = audio_data->timestamp_us;

              g_queue_push_head (audio_playback->pending_frames, remaining_audio_data);
            }
        }

      apply_volume_to_audio_data (audio_playback, volume_data,
                                  (int16_t *) audio_data->data,
                                  frames_taken);

      memcpy (raw_data + copied_data, audio_data->data,
              frames_taken * sample_size);
      copied_data += frames_taken * sample_size;

      audio_data_free (audio_data);
    }

  switch (codec)
    {
    case GRD_RDP_DSP_CODEC_NONE:
      out_data = g_steal_pointer (&raw_data);
      out_size = copied_data;
      success = TRUE;
      break;
    case GRD_RDP_DSP_CODEC_AAC:
    case GRD_RDP_DSP_CODEC_ALAW:
    case GRD_RDP_DSP_CODEC_OPUS:
      success = grd_rdp_dsp_encode (audio_playback->rdp_dsp, codec,
                                    (int16_t *) raw_data, copied_data,
                                    sizeof (int16_t), &out_data, &out_size);
      break;
    }

  if (!success)
    return;

  g_mutex_lock (&audio_playback->block_mutex);
  block_info = &audio_playback->block_infos[rdpsnd_context->block_no];

  block_info->render_latency_ms = 0;
  g_mutex_unlock (&audio_playback->block_mutex);

  rdpsnd_context->SendSamples2 (rdpsnd_context, client_format_idx,
                                out_data, out_size, 0, 0);
}

static gboolean
maybe_encode_frames (gpointer user_data)
{
  GrdRdpAudioPlayback *audio_playback = user_data;
  g_autoptr (GMutexLocker) streams_locker = NULL;
  g_autoptr (GMutexLocker) stream_lock_locker = NULL;
  GrdRdpAudioOutputStream *audio_output_stream;
  GrdRdpAudioVolumeData volume_data = {};
  uint32_t node_id;

  if (audio_playback->pending_training_confirm)
    return G_SOURCE_CONTINUE;

  stream_lock_locker = g_mutex_locker_new (&audio_playback->stream_lock_mutex);
  if (!audio_playback->has_stream_lock)
    return G_SOURCE_CONTINUE;

  node_id = audio_playback->locked_node_id;

  streams_locker = g_mutex_locker_new (&audio_playback->streams_mutex);
  if (!g_hash_table_lookup_extended (audio_playback->audio_streams,
                                     GUINT_TO_POINTER (node_id),
                                     NULL, (gpointer *) &audio_output_stream))
    return G_SOURCE_CONTINUE;

  grd_rdp_audio_output_stream_get_volume_data (audio_output_stream,
                                               &volume_data);
  g_clear_pointer (&streams_locker, g_mutex_locker_free);
  g_clear_pointer (&stream_lock_locker, g_mutex_locker_free);

  g_return_val_if_fail (volume_data.n_volumes <= SPA_AUDIO_MAX_CHANNELS,
                        G_SOURCE_CONTINUE);

  prepare_volume_data (&volume_data);

  g_mutex_lock (&audio_playback->pending_frames_mutex);
  clear_old_frames (audio_playback);
  maybe_drop_pending_frames (audio_playback);

  if (g_queue_get_length (audio_playback->pending_frames) > 0)
    maybe_send_frames (audio_playback, &volume_data);
  g_mutex_unlock (&audio_playback->pending_frames_mutex);

  return G_SOURCE_CONTINUE;
}

static void
pipewire_core_error (void       *user_data,
                     uint32_t    id,
                     int         seq,
                     int         res,
                     const char *message)
{
  GrdRdpAudioPlayback *audio_playback = user_data;

  g_warning ("[RDP.AUDIO_PLAYBACK] PipeWire core error: "
             "id: %u, seq: %i, res: %i, %s", id, seq, res, message);

  if (id == PW_ID_CORE && res == -EPIPE)
    g_source_set_ready_time (audio_playback->channel_teardown_source, 0);
}

static const struct pw_core_events pipewire_core_events =
{
  .version = PW_VERSION_CORE_EVENTS,
  .error = pipewire_core_error,
};

static void
registry_event_global (void                  *user_data,
                       uint32_t               id,
                       uint32_t               permissions,
                       const char            *type,
                       uint32_t               version,
                       const struct spa_dict *props)
{
  GrdRdpAudioPlayback *audio_playback = user_data;
  GrdRdpAudioOutputStream *audio_output_stream;
  g_autoptr (GMutexLocker) locker = NULL;
  const struct spa_dict_item *item;
  gboolean found_audio_sink = FALSE;
  uint32_t position[SPA_AUDIO_MAX_CHANNELS] = {};
  g_autofree char *node_name = NULL;
  g_autoptr (GError) error = NULL;

  if (strcmp (type, PW_TYPE_INTERFACE_Node) != 0)
    return;

  spa_dict_for_each (item, props)
    {
      if (strcmp (item->key, "node.name") == 0)
        {
          g_clear_pointer (&node_name, g_free);
          node_name = g_strdup (item->value);
        }
      if (strcmp (item->key, "media.class") == 0 &&
          strcmp (item->value, "Audio/Sink") == 0)
        found_audio_sink = TRUE;
    }

  if (!found_audio_sink)
    return;

  g_debug ("[RDP.AUDIO_PLAYBACK] Found audio sink: id: %u, type: %s/%u, "
           "name: %s", id, type, version, node_name);

  locker = g_mutex_locker_new (&audio_playback->streams_mutex);
  if (g_hash_table_contains (audio_playback->audio_streams,
                             GUINT_TO_POINTER (id)))
    return;

  g_clear_pointer (&locker, g_mutex_locker_free);

  g_assert (N_CHANNELS == 2);
  position[0] = SPA_AUDIO_CHANNEL_FL;
  position[1] = SPA_AUDIO_CHANNEL_FR;

  audio_output_stream =
    grd_rdp_audio_output_stream_new (audio_playback,
                                     audio_playback->pipewire_core,
                                     audio_playback->pipewire_registry,
                                     id, audio_playback->n_samples_per_sec,
                                     N_CHANNELS, position, &error);
  if (!audio_output_stream)
    {
      g_warning ("[RDP.AUDIO_PLAYBACK] Failed to establish PipeWire stream: "
                 "%s", error->message);
      return;
    }

  locker = g_mutex_locker_new (&audio_playback->streams_mutex);
  g_hash_table_insert (audio_playback->audio_streams,
                       GUINT_TO_POINTER (id), audio_output_stream);
  g_clear_pointer (&locker, g_mutex_locker_free);

  g_mutex_lock (&audio_playback->stream_lock_mutex);
  if (!audio_playback->has_stream_lock)
    grd_rdp_audio_output_stream_set_active (audio_output_stream, TRUE);
  g_mutex_unlock (&audio_playback->stream_lock_mutex);
}

static void
registry_event_global_remove (void     *user_data,
                              uint32_t  id)
{
  GrdRdpAudioPlayback *audio_playback = user_data;

  g_mutex_lock (&audio_playback->streams_mutex);
  g_hash_table_remove (audio_playback->audio_streams, GUINT_TO_POINTER (id));
  g_mutex_unlock (&audio_playback->streams_mutex);

  g_mutex_lock (&audio_playback->stream_lock_mutex);
  if (audio_playback->has_stream_lock &&
      audio_playback->locked_node_id == id)
    release_stream_lock (audio_playback);
  g_mutex_unlock (&audio_playback->stream_lock_mutex);
}

static const struct pw_registry_events registry_events =
{
  .version = PW_VERSION_REGISTRY_EVENTS,
  .global = registry_event_global,
  .global_remove = registry_event_global_remove,
};

static gboolean
set_up_registry_listener (GrdRdpAudioPlayback  *audio_playback,
                          GError              **error)
{
  GrdPipeWireSource *pipewire_source;

  pipewire_source = grd_attached_pipewire_source_new ("RDP.AUDIO_PLAYBACK",
                                                      error);
  if (!pipewire_source)
    return FALSE;

  audio_playback->pipewire_source = (GSource *) pipewire_source;

  audio_playback->pipewire_context =
    pw_context_new (pipewire_source->pipewire_loop, NULL, 0);
  if (!audio_playback->pipewire_context)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire context");
      return FALSE;
    }

  audio_playback->pipewire_core =
    pw_context_connect (audio_playback->pipewire_context, NULL, 0);
  if (!audio_playback->pipewire_core)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire core");
      return FALSE;
    }

  pw_core_add_listener (audio_playback->pipewire_core,
                        &audio_playback->pipewire_core_listener,
                        &pipewire_core_events, audio_playback);

  audio_playback->pipewire_registry =
    pw_core_get_registry (audio_playback->pipewire_core,
                          PW_VERSION_REGISTRY, 0);
  if (!audio_playback->pipewire_registry)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to retrieve PipeWire registry");
      return FALSE;
    }

  pw_registry_add_listener (audio_playback->pipewire_registry,
                            &audio_playback->pipewire_registry_listener,
                            &registry_events, audio_playback);

  return TRUE;
}

static gboolean
set_up_pipewire (gpointer user_data)
{
  GrdRdpAudioPlayback *audio_playback = user_data;
  g_autoptr (GError) error = NULL;

  g_clear_pointer (&audio_playback->pipewire_setup_source, g_source_unref);

  if (!set_up_registry_listener (audio_playback, &error))
    {
      g_warning ("[RDP.AUDIO_PLAYBACK] Failed to setup registry listener: %s",
                 error->message);
      g_source_set_ready_time (audio_playback->channel_teardown_source, 0);
    }

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
grd_rdp_audio_playback_init (GrdRdpAudioPlayback *audio_playback)
{
  GSource *svc_setup_source;
  GSource *channel_teardown_source;
  GSource *encode_source;
  GSource *pipewire_setup_source;

  audio_playback->pending_client_formats = TRUE;
  audio_playback->pending_training_confirm = TRUE;

  audio_playback->aac_client_format_idx = -1;
  audio_playback->opus_client_format_idx = -1;
  audio_playback->pcm_client_format_idx = -1;

  audio_playback->audio_streams = g_hash_table_new_full (NULL, NULL,
                                                         NULL, g_object_unref);
  audio_playback->pending_frames = g_queue_new ();

  g_mutex_init (&audio_playback->protocol_timeout_mutex);
  g_mutex_init (&audio_playback->block_mutex);
  g_mutex_init (&audio_playback->streams_mutex);
  g_mutex_init (&audio_playback->stream_lock_mutex);
  g_mutex_init (&audio_playback->pending_frames_mutex);

  audio_playback->encode_context = g_main_context_new ();

  svc_setup_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (svc_setup_source, set_up_static_virtual_channel,
                         audio_playback, NULL);
  g_source_set_ready_time (svc_setup_source, -1);
  g_source_attach (svc_setup_source, NULL);
  audio_playback->svc_setup_source = svc_setup_source;

  channel_teardown_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (channel_teardown_source, tear_down_channel,
                         audio_playback, NULL);
  g_source_set_ready_time (channel_teardown_source, -1);
  g_source_attach (channel_teardown_source, NULL);
  audio_playback->channel_teardown_source = channel_teardown_source;

  encode_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (encode_source, maybe_encode_frames,
                         audio_playback, NULL);
  g_source_set_ready_time (encode_source, -1);
  g_source_attach (encode_source, audio_playback->encode_context);
  audio_playback->encode_source = encode_source;

  pipewire_setup_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (pipewire_setup_source, set_up_pipewire,
                         audio_playback, NULL);
  g_source_set_ready_time (pipewire_setup_source, -1);
  g_source_attach (pipewire_setup_source, NULL);
  audio_playback->pipewire_setup_source = pipewire_setup_source;
}

static void
grd_rdp_audio_playback_class_init (GrdRdpAudioPlaybackClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_audio_playback_dispose;
  object_class->finalize = grd_rdp_audio_playback_finalize;
}
