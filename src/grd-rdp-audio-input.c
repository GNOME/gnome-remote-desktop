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

#include "grd-rdp-audio-input.h"

#include <spa/param/audio/raw.h>

#include "grd-pipewire-utils.h"
#include "grd-rdp-dsp.h"
#include "grd-rdp-dvc.h"
#include "grd-session-rdp.h"

#define PROTOCOL_TIMEOUT_MS (10 * 1000)

#define MAX_LOCAL_FRAMES_LIFETIME_US (200 * 1000)

#define N_CHANNELS 2
#define N_SAMPLES_PER_SEC 44100
#define N_BYTES_PER_SAMPLE_PCM sizeof (int16_t)
#define N_BYTES_PER_SAMPLE_ALAW sizeof (uint8_t)
#define N_BLOCK_ALIGN_PCM (N_CHANNELS * N_BYTES_PER_SAMPLE_PCM)
#define N_BLOCK_ALIGN_ALAW (N_CHANNELS * N_BYTES_PER_SAMPLE_ALAW)

typedef enum
{
  NEGOTIATION_STATE_AWAIT_VERSION,
  NEGOTIATION_STATE_AWAIT_INCOMING_DATA,
  NEGOTIATION_STATE_AWAIT_FORMATS,
  NEGOTIATION_STATE_AWAIT_FORMAT_CHANGE,
  NEGOTIATION_STATE_AWAIT_OPEN_REPLY,
  NEGOTIATION_STATE_COMPLETE,
} NegotiationState;

typedef enum
{
  RT_STATE_AWAIT_INCOMING_DATA,
  RT_STATE_AWAIT_DATA,
} RuntimeState;

typedef struct
{
  int16_t *data;
  uint32_t n_frames;
  uint32_t sample_offset;
  int64_t timestamp_us;
} AudioData;

struct _GrdRdpAudioInput
{
  GObject parent;

  audin_server_context *audin_context;
  gboolean channel_opened;
  gboolean channel_unavailable;

  GMutex prevent_dvc_init_mutex;
  gboolean prevent_dvc_initialization;

  uint32_t channel_id;
  uint32_t dvc_subscription_id;
  gboolean subscribed_status;

  GrdSessionRdp *session_rdp;
  GrdRdpDvc *rdp_dvc;

  GMutex protocol_timeout_mutex;
  GSource *channel_teardown_source;
  GSource *protocol_timeout_source;

  NegotiationState negotiation_state;
  RuntimeState runtime_state;

  int64_t incoming_data_time_us;

  int64_t requested_format_idx;
  int64_t alaw_client_format_idx;
  int64_t pcm_client_format_idx;
  GrdRdpDspCodec codec;

  GrdRdpDsp *rdp_dsp;

  GSource *pipewire_source;

  struct pw_context *pipewire_context;
  struct pw_core *pipewire_core;
  struct spa_hook pipewire_core_listener;

  struct pw_stream *pipewire_stream;
  struct spa_hook pipewire_stream_listener;

  GMutex pending_frames_mutex;
  GQueue *pending_frames;
};

G_DEFINE_TYPE (GrdRdpAudioInput, grd_rdp_audio_input, G_TYPE_OBJECT)

static void
audio_data_free (gpointer data);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AudioData, audio_data_free)

static void
audio_data_free (gpointer data)
{
  AudioData *audio_data = data;

  g_free (audio_data->data);
  g_free (audio_data);
}

static gboolean
initiate_channel_teardown (gpointer user_data)
{
  GrdRdpAudioInput *audio_input = user_data;

  g_warning ("[RDP.AUDIO_INPUT] Client did not respond to protocol initiation. "
             "Terminating protocol");

  g_mutex_lock (&audio_input->protocol_timeout_mutex);
  g_clear_pointer (&audio_input->protocol_timeout_source, g_source_unref);
  g_mutex_unlock (&audio_input->protocol_timeout_mutex);

  g_source_set_ready_time (audio_input->channel_teardown_source, 0);

  return G_SOURCE_REMOVE;
}

void
grd_rdp_audio_input_maybe_init (GrdRdpAudioInput *audio_input)
{
  audin_server_context *audin_context;
  g_autoptr (GMutexLocker) locker = NULL;

  if (audio_input->channel_opened || audio_input->channel_unavailable)
    return;

  locker = g_mutex_locker_new (&audio_input->prevent_dvc_init_mutex);
  if (audio_input->prevent_dvc_initialization)
    return;

  g_debug ("[RDP.AUDIO_INPUT] Trying to open DVC");

  audin_context = audio_input->audin_context;
  if (!audin_context->Open (audin_context))
    {
      g_warning ("[RDP.AUDIO_INPUT] Failed to open channel. "
                 "Terminating protocol");
      audio_input->channel_unavailable = TRUE;
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);
      return;
    }
  audio_input->channel_opened = TRUE;

  g_assert (!audio_input->protocol_timeout_source);

  audio_input->protocol_timeout_source =
    g_timeout_source_new (PROTOCOL_TIMEOUT_MS);
  g_source_set_callback (audio_input->protocol_timeout_source,
                         initiate_channel_teardown, audio_input, NULL);
  g_source_attach (audio_input->protocol_timeout_source, NULL);
}

static void
dvc_creation_status (gpointer user_data,
                     int32_t  creation_status)
{
  GrdRdpAudioInput *audio_input = user_data;

  if (creation_status < 0)
    {
      g_warning ("[RDP.AUDIO_INPUT] Failed to open channel "
                 "(CreationStatus %i). Terminating protocol", creation_status);
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);
    }
}

static BOOL
audin_channel_id_assigned (audin_server_context *audin_context,
                           uint32_t              channel_id)
{
  GrdRdpAudioInput *audio_input = audin_context->userdata;

  g_debug ("[RDP.AUDIO_INPUT] DVC channel id assigned to id %u", channel_id);
  audio_input->channel_id = channel_id;

  audio_input->dvc_subscription_id =
    grd_rdp_dvc_subscribe_dvc_creation_status (audio_input->rdp_dvc,
                                               channel_id,
                                               dvc_creation_status,
                                               audio_input);
  audio_input->subscribed_status = TRUE;

  return TRUE;
}

static const char *
negotiation_state_to_string (NegotiationState state)
{
  switch (state)
    {
    case NEGOTIATION_STATE_AWAIT_VERSION:
      return "AWAIT_VERSION";
    case NEGOTIATION_STATE_AWAIT_INCOMING_DATA:
      return "AWAIT_INCOMING_DATA";
    case NEGOTIATION_STATE_AWAIT_FORMATS:
      return "AWAIT_FORMATS";
    case NEGOTIATION_STATE_AWAIT_FORMAT_CHANGE:
      return "AWAIT_FORMAT_CHANGE";
    case NEGOTIATION_STATE_AWAIT_OPEN_REPLY:
      return "AWAIT_OPEN_REPLY";
    case NEGOTIATION_STATE_COMPLETE:
      return "COMPLETE";
    }

  g_assert_not_reached ();
}

static const AUDIO_FORMAT audio_format_alaw =
{
  .wFormatTag = WAVE_FORMAT_ALAW,
  .nChannels = N_CHANNELS,
  .nSamplesPerSec = N_SAMPLES_PER_SEC,
  .nAvgBytesPerSec = N_SAMPLES_PER_SEC * N_BLOCK_ALIGN_ALAW,
  .nBlockAlign = N_BLOCK_ALIGN_ALAW,
  .wBitsPerSample = N_BYTES_PER_SAMPLE_ALAW * 8,
  .cbSize = 0,
};

static const AUDIO_FORMAT audio_format_pcm =
{
  .wFormatTag = WAVE_FORMAT_PCM,
  .nChannels = N_CHANNELS,
  .nSamplesPerSec = N_SAMPLES_PER_SEC,
  .nAvgBytesPerSec = N_SAMPLES_PER_SEC * N_BLOCK_ALIGN_PCM,
  .nBlockAlign = N_BLOCK_ALIGN_PCM,
  .wBitsPerSample = N_BYTES_PER_SAMPLE_PCM * 8,
  .cbSize = 0,
};

static AUDIO_FORMAT server_formats[] =
{
  audio_format_alaw,
  audio_format_pcm,
};

static uint32_t
audin_receive_version (audin_server_context *audin_context,
                       const SNDIN_VERSION  *version)
{
  GrdRdpAudioInput *audio_input = audin_context->userdata;
  SNDIN_FORMATS formats = {};

  if (audio_input->negotiation_state != NEGOTIATION_STATE_AWAIT_VERSION)
    {
      g_warning ("[RDP.AUDIO_INPUT] Protocol violation: Received stray Version "
                 "PDU (Negotiation state: %s). Terminating protocol",
                 negotiation_state_to_string (audio_input->negotiation_state));
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);

      return CHANNEL_RC_INITIALIZATION_ERROR;
    }

  if (version->Version < SNDIN_VERSION_Version_1)
    {
      g_warning ("[RDP.AUDIO_INPUT] Protocol violation: Received invalid "
                 "Version PDU. Terminating protocol");
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);

      return CHANNEL_RC_INITIALIZATION_ERROR;
    }

  g_debug ("[RDP.AUDIO_INPUT] Client supports version %u", version->Version);

  audio_input->negotiation_state = NEGOTIATION_STATE_AWAIT_INCOMING_DATA;

  formats.NumFormats = G_N_ELEMENTS (server_formats);
  formats.SoundFormats = server_formats;

  return audin_context->SendFormats (audin_context, &formats);
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

/* KSDATAFORMAT_SUBTYPE_PCM ({00000001-0000-0010-8000-00aa00389b71}) */
static GUID KSDATAFORMAT_SUBTYPE_PCM =
{
  .Data1 = 0x00000001,
  .Data2 = 0x0000,
  .Data3 = 0x0010,
  .Data4[0] = 0x80,
  .Data4[1] = 0x00,
  .Data4[2] = 0x00,
  .Data4[3] = 0xaa,
  .Data4[4] = 0x00,
  .Data4[5] = 0x38,
  .Data4[6] = 0x9b,
  .Data4[7] = 0x71,
};

static uint32_t
audin_receive_formats (audin_server_context *audin_context,
                       const SNDIN_FORMATS  *formats)
{
  GrdRdpAudioInput *audio_input = audin_context->userdata;
  int64_t current_time_us;
  int64_t time_delta_us;
  size_t byte_count;
  SNDIN_OPEN open = {};
  WAVEFORMAT_EXTENSIBLE waveformat_extensible = {};
  uint32_t i;

  current_time_us = g_get_monotonic_time ();

  if (audio_input->negotiation_state != NEGOTIATION_STATE_AWAIT_FORMATS)
    {
      g_warning ("[RDP.AUDIO_INPUT] Protocol violation: Received stray Sound "
                 "Formats PDU (Negotiation state: %s). Terminating protocol",
                 negotiation_state_to_string (audio_input->negotiation_state));
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);

      return CHANNEL_RC_INITIALIZATION_ERROR;
    }

  for (i = 0; i < formats->NumFormats; ++i)
    {
      AUDIO_FORMAT *audio_format = &formats->SoundFormats[i];

      if (audio_input->alaw_client_format_idx < 0 &&
          are_audio_formats_equal (audio_format, &audio_format_alaw))
        audio_input->alaw_client_format_idx = i;
      if (audio_input->pcm_client_format_idx < 0 &&
          are_audio_formats_equal (audio_format, &audio_format_pcm))
        audio_input->pcm_client_format_idx = i;
    }

  if (audio_input->alaw_client_format_idx < 0 &&
      audio_input->pcm_client_format_idx < 0)
    {
      g_warning ("[RDP.AUDIO_INPUT] Audio Format negotiation with client "
                 "failed. Terminating protocol");
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);

      return CHANNEL_RC_INITIALIZATION_ERROR;
    }

  g_debug ("[RDP.AUDIO_INPUT] Client Formats: [A-law: %s, PCM: %s]",
           audio_input->alaw_client_format_idx >= 0 ? "true" : "false",
           audio_input->pcm_client_format_idx >= 0 ? "true" : "false");

  if (audio_input->alaw_client_format_idx >= 0)
    audio_input->requested_format_idx = audio_input->alaw_client_format_idx;
  else if (audio_input->pcm_client_format_idx >= 0)
    audio_input->requested_format_idx = audio_input->pcm_client_format_idx;
  else
    g_assert_not_reached ();

  time_delta_us = current_time_us - audio_input->incoming_data_time_us;
  byte_count = formats->cbSizeFormatsPacket + formats->ExtraDataSize;
  g_debug ("[RDP.AUDIO_INPUT] Measured clients uplink: >= %zuKB/s "
           "(cbSizeFormatsPacket: %u, ExtraDataSize, %zu)",
           byte_count * 1000 / MAX (time_delta_us, 1),
           formats->cbSizeFormatsPacket, formats->ExtraDataSize);

  audio_input->negotiation_state = NEGOTIATION_STATE_AWAIT_FORMAT_CHANGE;

  waveformat_extensible.Samples.wValidBitsPerSample = N_BYTES_PER_SAMPLE_PCM * 8;
  waveformat_extensible.dwChannelMask = SPEAKER_FRONT_LEFT |
                                        SPEAKER_FRONT_RIGHT;
  waveformat_extensible.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

  open.FramesPerPacket = N_SAMPLES_PER_SEC / 100;
  open.initialFormat = audio_input->requested_format_idx;
  open.captureFormat.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  open.captureFormat.nChannels = N_CHANNELS;
  open.captureFormat.nSamplesPerSec = N_SAMPLES_PER_SEC;
  open.captureFormat.nAvgBytesPerSec = N_SAMPLES_PER_SEC * N_BLOCK_ALIGN_PCM;
  open.captureFormat.nBlockAlign = N_BLOCK_ALIGN_PCM;
  open.captureFormat.wBitsPerSample = N_BYTES_PER_SAMPLE_PCM * 8;
  open.ExtraFormatData = &waveformat_extensible;

  return audin_context->SendOpen (audin_context, &open);
}

static uint32_t
audin_open_reply (audin_server_context   *audin_context,
                  const SNDIN_OPEN_REPLY *open_reply)
{
  GrdRdpAudioInput *audio_input = audin_context->userdata;
  int32_t signed_result = open_reply->Result;

  if (audio_input->negotiation_state != NEGOTIATION_STATE_AWAIT_OPEN_REPLY)
    {
      g_warning ("[RDP.AUDIO_INPUT] Protocol violation: Received stray Open "
                 "Reply PDU (Negotiation state: %s). Terminating protocol",
                 negotiation_state_to_string (audio_input->negotiation_state));
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);

      return CHANNEL_RC_INITIALIZATION_ERROR;
    }

  g_mutex_lock (&audio_input->protocol_timeout_mutex);
  if (audio_input->protocol_timeout_source)
    {
      g_source_destroy (audio_input->protocol_timeout_source);
      g_clear_pointer (&audio_input->protocol_timeout_source, g_source_unref);
    }
  g_mutex_unlock (&audio_input->protocol_timeout_mutex);

  if (signed_result < 0)
    {
      g_warning ("[RDP.AUDIO_INPUT] Failed to open audio capture device. "
                 "Terminating protocol");
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);

      return CHANNEL_RC_INITIALIZATION_ERROR;
    }

  g_debug ("[RDP.AUDIO_INPUT] Received Open Reply PDU, HRESULT: %i",
           signed_result);

  audio_input->negotiation_state = NEGOTIATION_STATE_COMPLETE;
  audio_input->runtime_state = RT_STATE_AWAIT_INCOMING_DATA;

  return CHANNEL_RC_OK;
}

static uint32_t
audin_incoming_data (audin_server_context      *audin_context,
                     const SNDIN_DATA_INCOMING *data_incoming)
{
  GrdRdpAudioInput *audio_input = audin_context->userdata;

  audio_input->incoming_data_time_us = g_get_monotonic_time ();

  if (audio_input->negotiation_state < NEGOTIATION_STATE_COMPLETE &&
      audio_input->negotiation_state != NEGOTIATION_STATE_AWAIT_INCOMING_DATA)
    {
      g_warning ("[RDP.AUDIO_INPUT] Protocol violation: Received stray "
                 "Incoming Data PDU (Negotiation state: %s)",
                 negotiation_state_to_string (audio_input->negotiation_state));
      return CHANNEL_RC_OK;
    }

  if (audio_input->negotiation_state == NEGOTIATION_STATE_COMPLETE &&
      audio_input->runtime_state != RT_STATE_AWAIT_INCOMING_DATA)
    {
      g_warning ("[RDP.AUDIO_INPUT] Protocol violation: Received duplicated "
                 "Incoming Data PDU");
    }

  if (audio_input->negotiation_state == NEGOTIATION_STATE_AWAIT_INCOMING_DATA)
    audio_input->negotiation_state = NEGOTIATION_STATE_AWAIT_FORMATS;
  else if (audio_input->negotiation_state == NEGOTIATION_STATE_COMPLETE)
    audio_input->runtime_state = RT_STATE_AWAIT_DATA;
  else
    g_assert_not_reached ();

  return CHANNEL_RC_OK;
}

static const char *
runtime_state_to_string (RuntimeState state)
{
  switch (state)
    {
    case RT_STATE_AWAIT_INCOMING_DATA:
      return "AWAIT_INCOMING_DATA";
    case RT_STATE_AWAIT_DATA:
      return "AWAIT_DATA";
    }

  g_assert_not_reached ();
}

static uint32_t
audin_data (audin_server_context *audin_context,
            const SNDIN_DATA     *data)
{
  GrdRdpAudioInput *audio_input = audin_context->userdata;
  g_autoptr (AudioData) audio_data = NULL;
  gboolean success = FALSE;
  uint32_t dst_size = 0;
  uint8_t *src_data;
  uint32_t src_size;

  if (audio_input->negotiation_state < NEGOTIATION_STATE_COMPLETE ||
      audio_input->runtime_state != RT_STATE_AWAIT_DATA)
    {
      g_warning ("[RDP.AUDIO_INPUT] Protocol violation: Received stray Data "
                 "PDU (Negotiation state: %s, Runtime state: %s). "
                 "Terminating protocol",
                 negotiation_state_to_string (audio_input->negotiation_state),
                 runtime_state_to_string (audio_input->runtime_state));
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);

      return CHANNEL_RC_UNSUPPORTED_VERSION;
    }

  audio_input->runtime_state = RT_STATE_AWAIT_INCOMING_DATA;

  src_data = Stream_Pointer (data->Data);
  src_size = Stream_Length (data->Data);

  if (!src_data || src_size == 0)
    return CHANNEL_RC_OK;

  audio_data = g_new0 (AudioData, 1);
  audio_data->timestamp_us = g_get_monotonic_time ();

  switch (audio_input->codec)
    {
    case GRD_RDP_DSP_CODEC_NONE:
      audio_data->n_frames = src_size / N_BLOCK_ALIGN_PCM;
      audio_data->data = g_memdup2 (src_data, audio_data->n_frames *
                                              N_BLOCK_ALIGN_PCM);
      success = TRUE;
      break;
    case GRD_RDP_DSP_CODEC_AAC:
    case GRD_RDP_DSP_CODEC_ALAW:
    case GRD_RDP_DSP_CODEC_OPUS:
      success = grd_rdp_dsp_decode (audio_input->rdp_dsp, audio_input->codec,
                                    src_data, src_size,
                                    &audio_data->data, &dst_size);
      audio_data->n_frames = dst_size / N_BLOCK_ALIGN_PCM;
      break;
    }
  if (!success || audio_data->n_frames == 0)
    return CHANNEL_RC_OK;

  g_mutex_lock (&audio_input->pending_frames_mutex);
  g_queue_push_tail (audio_input->pending_frames, g_steal_pointer (&audio_data));
  g_mutex_unlock (&audio_input->pending_frames_mutex);

  return CHANNEL_RC_OK;
}

static uint32_t
audin_receive_format_change (audin_server_context     *audin_context,
                             const SNDIN_FORMATCHANGE *format_change)
{
  GrdRdpAudioInput *audio_input = audin_context->userdata;

  if (audio_input->negotiation_state < NEGOTIATION_STATE_COMPLETE &&
      audio_input->negotiation_state != NEGOTIATION_STATE_AWAIT_FORMAT_CHANGE)
    {
      g_warning ("[RDP.AUDIO_INPUT] Protocol violation: Received stray Format "
                 "Change PDU (Negotiation state: %s). Terminating protocol",
                 negotiation_state_to_string (audio_input->negotiation_state));
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);

      return CHANNEL_RC_INITIALIZATION_ERROR;
    }

  if (audio_input->negotiation_state == NEGOTIATION_STATE_COMPLETE)
    {
      g_warning ("[RDP.AUDIO_INPUT] Protocol violation: Received stray Format "
                 "Change PDU (Runtime state: %s). Terminating protocol",
                 runtime_state_to_string (audio_input->runtime_state));
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);

      return CHANNEL_RC_UNSUPPORTED_VERSION;
    }

  if (format_change->NewFormat != audio_input->requested_format_idx)
    {
      g_warning ("[RDP.AUDIO_INPUT] Protocol violation: Received Format Change "
                 "PDU with invalid new format (%u), expected %li. "
                 "Terminating protocol",
                 format_change->NewFormat, audio_input->requested_format_idx);
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);

      return CHANNEL_RC_INITIALIZATION_ERROR;
    }

  if (audio_input->requested_format_idx == audio_input->alaw_client_format_idx)
    audio_input->codec = GRD_RDP_DSP_CODEC_ALAW;
  else if (audio_input->requested_format_idx == audio_input->pcm_client_format_idx)
    audio_input->codec = GRD_RDP_DSP_CODEC_NONE;
  else
    g_assert_not_reached ();

  g_debug ("[RDP.AUDIO_INPUT] Format Change: New Format: %u, Codec: %s",
           format_change->NewFormat,
           grd_rdp_dsp_codec_to_string (audio_input->codec));

  if (audio_input->negotiation_state < NEGOTIATION_STATE_COMPLETE)
    audio_input->negotiation_state = NEGOTIATION_STATE_AWAIT_OPEN_REPLY;
  else
    audio_input->runtime_state = RT_STATE_AWAIT_INCOMING_DATA;

  return CHANNEL_RC_OK;
}

static void
pipewire_core_error (void       *user_data,
                     uint32_t    id,
                     int         seq,
                     int         res,
                     const char *message)
{
  GrdRdpAudioInput *audio_input = user_data;

  g_warning ("[RDP.AUDIO_INPUT] PipeWire core error: "
             "id: %u, seq: %i, res: %i, %s", id, seq, res, message);

  if (id == PW_ID_CORE && res == -EPIPE)
    g_source_set_ready_time (audio_input->channel_teardown_source, 0);
}

static const struct pw_core_events pipewire_core_events =
{
  .version = PW_VERSION_CORE_EVENTS,
  .error = pipewire_core_error,
};

static gboolean
set_up_pipewire (GrdRdpAudioInput  *audio_input,
                 GError           **error)
{
  GrdPipeWireSource *pipewire_source;

  pipewire_source = grd_attached_pipewire_source_new ("RDP.AUDIO_INPUT", error);
  if (!pipewire_source)
    return FALSE;

  audio_input->pipewire_source = (GSource *) pipewire_source;

  audio_input->pipewire_context =
    pw_context_new (pipewire_source->pipewire_loop, NULL, 0);
  if (!audio_input->pipewire_context)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire context");
      return FALSE;
    }

  audio_input->pipewire_core =
    pw_context_connect (audio_input->pipewire_context, NULL, 0);
  if (!audio_input->pipewire_core)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire core");
      return FALSE;
    }

  pw_core_add_listener (audio_input->pipewire_core,
                        &audio_input->pipewire_core_listener,
                        &pipewire_core_events, audio_input);

  return TRUE;
}

static void
ensure_dvc_is_closed (GrdRdpAudioInput *audio_input)
{
  g_mutex_lock (&audio_input->prevent_dvc_init_mutex);
  audio_input->prevent_dvc_initialization = TRUE;
  g_mutex_unlock (&audio_input->prevent_dvc_init_mutex);

  if (audio_input->channel_opened)
    {
      g_debug ("[RDP.AUDIO_INPUT] Closing DVC");

      audio_input->audin_context->Close (audio_input->audin_context);
      audio_input->channel_opened = FALSE;
    }
  if (audio_input->subscribed_status)
    {
      grd_rdp_dvc_unsubscribe_dvc_creation_status (audio_input->rdp_dvc,
                                                   audio_input->channel_id,
                                                   audio_input->dvc_subscription_id);
      audio_input->subscribed_status = FALSE;
    }

  if (audio_input->protocol_timeout_source)
    {
      g_source_destroy (audio_input->protocol_timeout_source);
      g_clear_pointer (&audio_input->protocol_timeout_source, g_source_unref);
    }

  g_mutex_lock (&audio_input->pending_frames_mutex);
  g_queue_clear_full (audio_input->pending_frames, audio_data_free);
  g_mutex_unlock (&audio_input->pending_frames_mutex);

  audio_input->negotiation_state = NEGOTIATION_STATE_AWAIT_VERSION;
  audio_input->runtime_state = RT_STATE_AWAIT_INCOMING_DATA;

  audio_input->requested_format_idx = -1;
  audio_input->alaw_client_format_idx = -1;
  audio_input->pcm_client_format_idx = -1;
  audio_input->codec = GRD_RDP_DSP_CODEC_NONE;
}

static void
ensure_dvc_can_be_opened (GrdRdpAudioInput *audio_input)
{
  g_assert (!audio_input->protocol_timeout_source);

  /* No need to lock prevent_dvc_init_mutex */
  audio_input->prevent_dvc_initialization = FALSE;
}

static void
pipewire_stream_state_changed (void                 *user_data,
                               enum pw_stream_state  old,
                               enum pw_stream_state  state,
                               const char           *error)
{
  GrdRdpAudioInput *audio_input = user_data;

  g_debug ("[RDP.AUDIO_INPUT] PipeWire stream state changed from %s to %s",
           pw_stream_state_as_string (old),
           pw_stream_state_as_string (state));

  switch (state)
    {
    case PW_STREAM_STATE_ERROR:
      g_warning ("[RDP.AUDIO_INPUT] PipeWire stream error: %s", error);
      g_source_set_ready_time (audio_input->channel_teardown_source, 0);
      break;
    case PW_STREAM_STATE_PAUSED:
      ensure_dvc_is_closed (audio_input);
      break;
    case PW_STREAM_STATE_STREAMING:
      ensure_dvc_can_be_opened (audio_input);
      break;
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
  GrdRdpAudioInput *audio_input = user_data;
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

  pw_stream_update_params (audio_input->pipewire_stream, params, 1);
}

static void
clear_old_frames (GrdRdpAudioInput *audio_input)
{
  int64_t current_time_us;
  AudioData *audio_data;

  current_time_us = g_get_monotonic_time ();

  audio_data = g_queue_peek_head (audio_input->pending_frames);
  if (!audio_data)
    return;

  if (current_time_us - audio_data->timestamp_us <= MAX_LOCAL_FRAMES_LIFETIME_US)
    return;

  audio_data = g_queue_pop_tail (audio_input->pending_frames);
  g_assert (audio_data);

  g_queue_clear_full (audio_input->pending_frames, audio_data_free);
  g_queue_push_tail (audio_input->pending_frames, audio_data);
}

static void
pipewire_stream_process (void *user_data)
{
  GrdRdpAudioInput *audio_input = user_data;
  g_autoptr (GMutexLocker) locker = NULL;
  struct pw_buffer *buffer = NULL;
  uint32_t n_frames;
  uint8_t *data;
  uint32_t *size;

  locker = g_mutex_locker_new (&audio_input->pending_frames_mutex);
  clear_old_frames (audio_input);

  if (g_queue_get_length (audio_input->pending_frames) == 0)
    return;

  buffer = pw_stream_dequeue_buffer (audio_input->pipewire_stream);
  if (!buffer)
    {
      g_warning ("[RDP.AUDIO_INPUT] Failed to dequeue PipeWire buffer");
      return;
    }

  n_frames = buffer->buffer->datas[0].maxsize / N_BLOCK_ALIGN_PCM;
  if (buffer->requested > 0 && buffer->requested < n_frames)
    n_frames = buffer->requested;

  buffer->buffer->datas[0].chunk->offset = 0;
  buffer->buffer->datas[0].chunk->stride = N_BLOCK_ALIGN_PCM;
  buffer->buffer->datas[0].chunk->size = 0;

  data = buffer->buffer->datas[0].data;
  g_assert (data);

  size = &buffer->buffer->datas[0].chunk->size;
  while (*size < n_frames * N_BLOCK_ALIGN_PCM)
    {
      AudioData *audio_data;
      uint32_t frames_taken;

      audio_data = g_queue_pop_head (audio_input->pending_frames);
      if (!audio_data)
        break;

      frames_taken = MIN (n_frames - *size / N_BLOCK_ALIGN_PCM,
                          audio_data->n_frames);
      memcpy (data + *size, audio_data->data + audio_data->sample_offset,
              frames_taken * N_BLOCK_ALIGN_PCM);
      *size += frames_taken * N_BLOCK_ALIGN_PCM;

      audio_data->n_frames -= frames_taken;
      audio_data->sample_offset += frames_taken * N_CHANNELS;
      if (audio_data->n_frames == 0)
        audio_data_free (audio_data);
      else
        g_queue_push_head (audio_input->pending_frames, audio_data);
    }
  g_clear_pointer (&locker, g_mutex_locker_free);

  pw_stream_queue_buffer (audio_input->pipewire_stream, buffer);
}

static const struct pw_stream_events pipewire_stream_events =
{
  .version = PW_VERSION_STREAM_EVENTS,
  .state_changed = pipewire_stream_state_changed,
  .param_changed = pipewire_stream_param_changed,
  .process = pipewire_stream_process,
};

static gboolean
set_up_audio_source (GrdRdpAudioInput  *audio_input,
                     GError           **error)
{
  struct spa_pod_builder pod_builder;
  const struct spa_pod *params[1];
  uint8_t params_buffer[1024];
  uint32_t position[N_CHANNELS] = {};
  int result;

  g_message ("[RDP.AUDIO_INPUT] Setting up Audio Source");

  position[0] = SPA_AUDIO_CHANNEL_FL;
  position[1] = SPA_AUDIO_CHANNEL_FR;

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));
  params[0] = spa_pod_builder_add_object (
    &pod_builder,
    SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
    SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_audio),
    SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_AUDIO_format, SPA_POD_Id (SPA_AUDIO_FORMAT_S16),
    SPA_FORMAT_AUDIO_rate, SPA_POD_Int (N_SAMPLES_PER_SEC),
    SPA_FORMAT_AUDIO_channels, SPA_POD_Int (N_CHANNELS),
    SPA_FORMAT_AUDIO_position, SPA_POD_Array (sizeof (uint32_t), SPA_TYPE_Id,
                                              N_CHANNELS, position),
    0);

  audio_input->pipewire_stream =
    pw_stream_new (audio_input->pipewire_core,
                   "GRD::RDP::AUDIO_INPUT",
                   pw_properties_new (PW_KEY_MEDIA_TYPE, "Audio",
                                      PW_KEY_MEDIA_CATEGORY, "Playback",
                                      PW_KEY_MEDIA_CLASS, "Audio/Source",
                                      PW_KEY_NODE_FORCE_QUANTUM, "256",
                                      PW_KEY_NODE_SUSPEND_ON_IDLE, "true",
                                      PW_KEY_PRIORITY_DRIVER, "24009",
                                      PW_KEY_PRIORITY_SESSION, "24009",
                                      PW_KEY_NODE_NAME, "grd_remote_audio_source",
                                      PW_KEY_NODE_NICK, "GNOME Remote Desktop Audio Input",
                                      PW_KEY_NODE_DESCRIPTION, "Remoteaudio Source",
                                      NULL));
  if (!audio_input->pipewire_stream)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire stream");
      return FALSE;
    }

  pw_stream_add_listener (audio_input->pipewire_stream,
                          &audio_input->pipewire_stream_listener,
                          &pipewire_stream_events, audio_input);

  result = pw_stream_connect (audio_input->pipewire_stream,
                              PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              PW_STREAM_FLAG_RT_PROCESS |
                              PW_STREAM_FLAG_AUTOCONNECT |
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

GrdRdpAudioInput *
grd_rdp_audio_input_new (GrdSessionRdp *session_rdp,
                         GrdRdpDvc     *rdp_dvc,
                         HANDLE         vcm,
                         rdpContext    *rdp_context)
{
  g_autoptr (GrdRdpAudioInput) audio_input = NULL;
  audin_server_context *audin_context;
  GrdRdpDspDescriptor dsp_descriptor = {};
  g_autoptr (GError) error = NULL;

  audio_input = g_object_new (GRD_TYPE_RDP_AUDIO_INPUT, NULL);
  audin_context = audin_server_context_new (vcm);
  if (!audin_context)
    g_error ("[RDP.AUDIO_INPUT] Failed to create server context (OOM)");

  audio_input->audin_context = audin_context;
  audio_input->session_rdp = session_rdp;
  audio_input->rdp_dvc = rdp_dvc;

  audin_context->serverVersion = SNDIN_VERSION_Version_2;

  audin_context->ChannelIdAssigned = audin_channel_id_assigned;
  audin_context->ReceiveVersion = audin_receive_version;
  audin_context->ReceiveFormats = audin_receive_formats;
  audin_context->OpenReply = audin_open_reply;
  audin_context->IncomingData = audin_incoming_data;
  audin_context->Data = audin_data;
  audin_context->ReceiveFormatChange = audin_receive_format_change;
  audin_context->rdpcontext = rdp_context;
  audin_context->userdata = audio_input;

  dsp_descriptor.create_flags = GRD_RDP_DSP_CREATE_FLAG_DECODER;

  audio_input->rdp_dsp = grd_rdp_dsp_new (&dsp_descriptor, &error);
  if (!audio_input->rdp_dsp)
    {
      g_warning ("[RDP.AUDIO_INPUT] Failed to create DSP instance: %s",
                 error->message);
      return NULL;
    }

  if (!set_up_pipewire (audio_input, &error))
    {
      g_warning ("[RDP.AUDIO_INPUT] Failed to set up PipeWire: %s",
                 error->message);
      return NULL;
    }
  if (!set_up_audio_source (audio_input, &error))
    {
      g_warning ("[RDP.AUDIO_INPUT] Failed to create audio source: %s",
                 error->message);
      return NULL;
    }

  return g_steal_pointer (&audio_input);
}

static void
grd_rdp_audio_input_dispose (GObject *object)
{
  GrdRdpAudioInput *audio_input = GRD_RDP_AUDIO_INPUT (object);

  ensure_dvc_is_closed (audio_input);
  g_assert (!audio_input->protocol_timeout_source);

  if (audio_input->pipewire_stream)
    {
      spa_hook_remove (&audio_input->pipewire_stream_listener);
      g_clear_pointer (&audio_input->pipewire_stream, pw_stream_destroy);
    }

  if (audio_input->pipewire_core)
    {
      spa_hook_remove (&audio_input->pipewire_core_listener);
      g_clear_pointer (&audio_input->pipewire_core, pw_core_disconnect);
    }
  g_clear_pointer (&audio_input->pipewire_context, pw_context_destroy);

  if (audio_input->pipewire_source)
    {
      g_source_destroy (audio_input->pipewire_source);
      g_clear_pointer (&audio_input->pipewire_source, g_source_unref);
    }

  g_clear_object (&audio_input->rdp_dsp);

  if (audio_input->channel_teardown_source)
    {
      g_source_destroy (audio_input->channel_teardown_source);
      g_clear_pointer (&audio_input->channel_teardown_source, g_source_unref);
    }

  if (audio_input->pending_frames)
    {
      g_queue_free_full (audio_input->pending_frames, audio_data_free);
      audio_input->pending_frames = NULL;
    }

  g_clear_pointer (&audio_input->audin_context, audin_server_context_free);

  G_OBJECT_CLASS (grd_rdp_audio_input_parent_class)->dispose (object);
}

static void
grd_rdp_audio_input_finalize (GObject *object)
{
  GrdRdpAudioInput *audio_input = GRD_RDP_AUDIO_INPUT (object);

  pw_deinit ();

  g_mutex_clear (&audio_input->pending_frames_mutex);
  g_mutex_clear (&audio_input->protocol_timeout_mutex);
  g_mutex_clear (&audio_input->prevent_dvc_init_mutex);

  G_OBJECT_CLASS (grd_rdp_audio_input_parent_class)->finalize (object);
}

static gboolean
tear_down_channel (gpointer user_data)
{
  GrdRdpAudioInput *audio_input = user_data;

  g_debug ("[RDP.AUDIO_INPUT] Tearing down channel");

  g_clear_pointer (&audio_input->channel_teardown_source, g_source_unref);

  grd_session_rdp_tear_down_channel (audio_input->session_rdp,
                                     GRD_RDP_CHANNEL_AUDIO_INPUT);

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
grd_rdp_audio_input_init (GrdRdpAudioInput *audio_input)
{
  GSource *channel_teardown_source;

  audio_input->prevent_dvc_initialization = TRUE;
  audio_input->negotiation_state = NEGOTIATION_STATE_AWAIT_VERSION;
  audio_input->runtime_state = RT_STATE_AWAIT_INCOMING_DATA;

  audio_input->requested_format_idx = -1;
  audio_input->alaw_client_format_idx = -1;
  audio_input->pcm_client_format_idx = -1;
  audio_input->codec = GRD_RDP_DSP_CODEC_NONE;

  audio_input->pending_frames = g_queue_new ();

  g_mutex_init (&audio_input->prevent_dvc_init_mutex);
  g_mutex_init (&audio_input->protocol_timeout_mutex);
  g_mutex_init (&audio_input->pending_frames_mutex);

  pw_init (NULL, NULL);

  channel_teardown_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (channel_teardown_source, tear_down_channel,
                         audio_input, NULL);
  g_source_set_ready_time (channel_teardown_source, -1);
  g_source_attach (channel_teardown_source, NULL);
  audio_input->channel_teardown_source = channel_teardown_source;
}

static void
grd_rdp_audio_input_class_init (GrdRdpAudioInputClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_audio_input_dispose;
  object_class->finalize = grd_rdp_audio_input_finalize;
}
