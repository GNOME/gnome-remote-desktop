/*
 * Copyright (C) 2023 Pascal Nowack
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

#include "grd-rdp-connect-time-autodetection.h"

#define CT_BW_MAX_TOLERABLE_RESPONSE_LATENCY_US (400 * 1000)
#define CT_BW_MAX_TOLERABLE_TIME_DELTA_MS 100

#define CT_N_PINGS 10
#define CT_PING_INTERVAL_MS 10

#define BW_MEASURE_SEQUENCE_NUMBER 0
#define CT_NON_RTT_SEQUENCE_NUMBER 0

typedef enum
{
  CT_AUTODETECT_STATE_NONE,
  CT_AUTODETECT_STATE_MEASURE_BW_1,
  CT_AUTODETECT_STATE_MEASURE_BW_2,
  CT_AUTODETECT_STATE_MEASURE_BW_3,
  CT_AUTODETECT_STATE_AWAIT_BW_RESULT_1,
  CT_AUTODETECT_STATE_AWAIT_BW_RESULT_2,
  CT_AUTODETECT_STATE_AWAIT_BW_RESULT_3,
  CT_AUTODETECT_STATE_START_RTT_DETECTION,
  CT_AUTODETECT_STATE_IN_RTT_DETECTION,
  CT_AUTODETECT_STATE_AWAIT_LAST_RTT_RESPONSE,
  CT_AUTODETECT_STATE_SEND_NET_CHAR_RESULT,
  CT_AUTODETECT_STATE_COMPLETE,
} CtAutodetectState;

struct _GrdRdpConnectTimeAutodetection
{
  GObject parent;

  GrdRdpNetworkAutodetection *network_autodetection;
  rdpAutoDetect *rdp_autodetect;
  gboolean protocol_stopped;

  GMutex ct_autodetection_mutex;
  CtAutodetectState state;

  GSource *sync_source;
  GCond net_char_sync_cond;
  GMutex net_char_sync_mutex;
  gboolean pending_net_char_sync;

  GSource *ping_source;
  uint32_t pending_pings;
  uint16_t last_sequence_number;

  int64_t base_round_trip_time_us;
  int64_t avg_round_trip_time_us;

  int64_t bw_measure_time_us;

  uint32_t last_time_delta_ms;
  uint32_t last_byte_count;
};

G_DEFINE_TYPE (GrdRdpConnectTimeAutodetection, grd_rdp_connect_time_autodetection,
               G_TYPE_OBJECT)

gboolean
grd_rdp_connect_time_autodetection_is_complete (GrdRdpConnectTimeAutodetection *ct_autodetection)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&ct_autodetection->ct_autodetection_mutex);
  return ct_autodetection->state == CT_AUTODETECT_STATE_COMPLETE;
}

static void
determine_bw_measure_payloads (GrdRdpConnectTimeAutodetection *ct_autodetection,
                               uint16_t                       *payload_lengths,
                               uint32_t                       *n_payloads)
{
  *payload_lengths = 0;
  *n_payloads = 1;

  switch (ct_autodetection->state)
    {
    case CT_AUTODETECT_STATE_MEASURE_BW_1:
      *payload_lengths = 15 * 1024 + 512 + 256 + 128 + 64;
      break;
    case CT_AUTODETECT_STATE_MEASURE_BW_2:
      *payload_lengths = 15 * 1024 + 512 + 256 + 128 + 64;
      *n_payloads = 4;
      break;
    case CT_AUTODETECT_STATE_MEASURE_BW_3:
      *payload_lengths = 15 * 1024 + 512 + 256 + 128 + 64;
      *n_payloads = 16;
      break;
    default:
      g_assert_not_reached ();
    }
}

static CtAutodetectState
get_next_bw_measure_state (GrdRdpConnectTimeAutodetection *ct_autodetection)
{
  switch (ct_autodetection->state)
    {
    case CT_AUTODETECT_STATE_MEASURE_BW_1:
      return CT_AUTODETECT_STATE_AWAIT_BW_RESULT_1;
    case CT_AUTODETECT_STATE_MEASURE_BW_2:
      return CT_AUTODETECT_STATE_AWAIT_BW_RESULT_2;
    case CT_AUTODETECT_STATE_MEASURE_BW_3:
      return CT_AUTODETECT_STATE_AWAIT_BW_RESULT_3;
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_1:
      return CT_AUTODETECT_STATE_MEASURE_BW_2;
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_2:
      return CT_AUTODETECT_STATE_MEASURE_BW_3;
    default:
      g_assert_not_reached ();
    }

  return CT_AUTODETECT_STATE_NONE;
}

static void
measure_connect_time_bandwidth (GrdRdpConnectTimeAutodetection *ct_autodetection,
                                uint16_t                        payload_lengths,
                                uint32_t                        n_payloads)
{
  rdpAutoDetect *rdp_autodetect = ct_autodetection->rdp_autodetect;
  uint32_t i;

  g_assert (payload_lengths > 0);
  g_assert (n_payloads >= 1);

  rdp_autodetect->BandwidthMeasureStart (rdp_autodetect, RDP_TRANSPORT_TCP,
                                         BW_MEASURE_SEQUENCE_NUMBER);
  for (i = 0; i < n_payloads - 1; ++i)
    {
      rdp_autodetect->BandwidthMeasurePayload (rdp_autodetect,
                                               RDP_TRANSPORT_TCP,
                                               BW_MEASURE_SEQUENCE_NUMBER,
                                               payload_lengths);
    }
  rdp_autodetect->BandwidthMeasureStop (rdp_autodetect, RDP_TRANSPORT_TCP,
                                        BW_MEASURE_SEQUENCE_NUMBER,
                                        payload_lengths);
}

static const char *
ct_autodetect_state_to_string (CtAutodetectState state)
{
  switch (state)
    {
    case CT_AUTODETECT_STATE_NONE:
      return "NONE";
    case CT_AUTODETECT_STATE_MEASURE_BW_1:
      return "MEASURE_BW_1";
    case CT_AUTODETECT_STATE_MEASURE_BW_2:
      return "MEASURE_BW_2";
    case CT_AUTODETECT_STATE_MEASURE_BW_3:
      return "MEASURE_BW_3";
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_1:
      return "AWAIT_BW_RESULT_1";
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_2:
      return "AWAIT_BW_RESULT_2";
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_3:
      return "AWAIT_BW_RESULT_3";
    case CT_AUTODETECT_STATE_START_RTT_DETECTION:
      return "START_RTT_DETECTION";
    case CT_AUTODETECT_STATE_IN_RTT_DETECTION:
      return "IN_RTT_DETECTION";
    case CT_AUTODETECT_STATE_AWAIT_LAST_RTT_RESPONSE:
      return "AWAIT_LAST_RTT_RESPONSE";
    case CT_AUTODETECT_STATE_SEND_NET_CHAR_RESULT:
      return "SEND_NET_CHAR_RESULT";
    case CT_AUTODETECT_STATE_COMPLETE:
      return "COMPLETE";
    }

  g_assert_not_reached ();
}

static void
transition_to_state (GrdRdpConnectTimeAutodetection *ct_autodetection,
                     CtAutodetectState               state)
{
  g_assert (ct_autodetection->state != state);

  g_debug ("[RDP] Connect-Time Autodetect: Updating state from %s to %s",
           ct_autodetect_state_to_string (ct_autodetection->state),
           ct_autodetect_state_to_string (state));

  ct_autodetection->state = state;
}

static void
last_sequence_number_ready (gpointer user_data,
                            uint16_t sequence_number)
{
  GrdRdpConnectTimeAutodetection *ct_autodetection = user_data;

  g_mutex_lock (&ct_autodetection->ct_autodetection_mutex);
  ct_autodetection->last_sequence_number = sequence_number;

  transition_to_state (ct_autodetection,
                       CT_AUTODETECT_STATE_AWAIT_LAST_RTT_RESPONSE);
  g_mutex_unlock (&ct_autodetection->ct_autodetection_mutex);
}

static gboolean
emit_ping (gpointer user_data)
{
  GrdRdpConnectTimeAutodetection *ct_autodetection = user_data;
  GrdRdpNetworkAutodetection *network_autodetection =
    ct_autodetection->network_autodetection;
  GrdRdpNwAutodetectSequenceNumberReadyCallback sequence_number_ready = NULL;

  g_assert (!ct_autodetection->protocol_stopped);

  g_assert (ct_autodetection->pending_pings > 0);
  --ct_autodetection->pending_pings;

  if (ct_autodetection->pending_pings == 0)
    {
      g_mutex_lock (&ct_autodetection->ct_autodetection_mutex);
      g_clear_pointer (&ct_autodetection->ping_source, g_source_unref);
      g_mutex_unlock (&ct_autodetection->ct_autodetection_mutex);

      sequence_number_ready = last_sequence_number_ready;
    }

  grd_rdp_network_autodetection_emit_ping (network_autodetection,
                                           sequence_number_ready,
                                           ct_autodetection);

  if (ct_autodetection->pending_pings == 0)
    return G_SOURCE_REMOVE;

  return G_SOURCE_CONTINUE;
}

static void
start_ping_emission (GrdRdpConnectTimeAutodetection *ct_autodetection)
{
  emit_ping (ct_autodetection);

  g_mutex_lock (&ct_autodetection->ct_autodetection_mutex);
  ct_autodetection->ping_source = g_timeout_source_new (CT_PING_INTERVAL_MS);

  g_source_set_callback (ct_autodetection->ping_source, emit_ping,
                         ct_autodetection, NULL);
  g_source_attach (ct_autodetection->ping_source, NULL);
  g_mutex_unlock (&ct_autodetection->ct_autodetection_mutex);
}

static void
notify_network_characteristics_result (GrdRdpConnectTimeAutodetection *ct_autodetection)
{
  rdpAutoDetect *rdp_autodetect = ct_autodetection->rdp_autodetect;
  rdpNetworkCharacteristicsResult result = {};
  uint64_t bit_count;
  uint32_t bandwidth_kbits;

  bit_count = ((uint64_t) ct_autodetection->last_byte_count) * UINT64_C (8);
  bandwidth_kbits = bit_count / MAX (ct_autodetection->last_time_delta_ms, 1);

  result.type = RDP_NETCHAR_RESULT_TYPE_BASE_RTT_BW_AVG_RTT;
  result.baseRTT = ct_autodetection->base_round_trip_time_us / 1000;
  result.averageRTT = ct_autodetection->avg_round_trip_time_us / 1000;
  result.bandwidth = bandwidth_kbits;

  rdp_autodetect->NetworkCharacteristicsResult (rdp_autodetect,
                                                RDP_TRANSPORT_TCP,
                                                CT_NON_RTT_SEQUENCE_NUMBER,
                                                &result);
}

static FREERDP_AUTODETECT_STATE
detect_network_characteristics (GrdRdpConnectTimeAutodetection *ct_autodetection)
{
  CtAutodetectState new_state = CT_AUTODETECT_STATE_NONE;
  g_autoptr (GMutexLocker) locker = NULL;
  gboolean pending_bw_request = FALSE;
  gboolean pending_ping_emission = FALSE;
  gboolean pending_net_char_result = FALSE;
  uint16_t payload_lengths = 0;
  uint32_t n_payloads = 1;

  locker = g_mutex_locker_new (&ct_autodetection->ct_autodetection_mutex);
  switch (ct_autodetection->state)
    {
    case CT_AUTODETECT_STATE_NONE:
      g_assert_not_reached ();
    case CT_AUTODETECT_STATE_MEASURE_BW_1:
    case CT_AUTODETECT_STATE_MEASURE_BW_2:
    case CT_AUTODETECT_STATE_MEASURE_BW_3:
      determine_bw_measure_payloads (ct_autodetection,
                                     &payload_lengths, &n_payloads);
      ct_autodetection->bw_measure_time_us = g_get_monotonic_time ();
      new_state = get_next_bw_measure_state (ct_autodetection);
      pending_bw_request = TRUE;
      break;
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_1:
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_2:
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_3:
      return FREERDP_AUTODETECT_STATE_REQUEST;
    case CT_AUTODETECT_STATE_START_RTT_DETECTION:
      g_assert (!ct_autodetection->ping_source);
      g_assert (ct_autodetection->pending_pings >= 2);

      new_state = CT_AUTODETECT_STATE_IN_RTT_DETECTION;
      pending_ping_emission = TRUE;
      break;
    case CT_AUTODETECT_STATE_IN_RTT_DETECTION:
    case CT_AUTODETECT_STATE_AWAIT_LAST_RTT_RESPONSE:
      return FREERDP_AUTODETECT_STATE_REQUEST;
    case CT_AUTODETECT_STATE_SEND_NET_CHAR_RESULT:
      new_state = CT_AUTODETECT_STATE_COMPLETE;
      pending_net_char_result = TRUE;
      break;
    case CT_AUTODETECT_STATE_COMPLETE:
      return FREERDP_AUTODETECT_STATE_COMPLETE;
    }

  g_assert (new_state != CT_AUTODETECT_STATE_NONE);
  transition_to_state (ct_autodetection, new_state);
  g_clear_pointer (&locker, g_mutex_locker_free);

  if (pending_bw_request)
    {
      measure_connect_time_bandwidth (ct_autodetection, payload_lengths,
                                      n_payloads);
    }

  if (pending_ping_emission)
    start_ping_emission (ct_autodetection);

  if (pending_net_char_result)
    {
      notify_network_characteristics_result (ct_autodetection);
      return FREERDP_AUTODETECT_STATE_COMPLETE;
    }

  return FREERDP_AUTODETECT_STATE_REQUEST;
}

void
grd_rdp_connect_time_autodetection_start_detection (GrdRdpConnectTimeAutodetection *ct_autodetection)
{
  g_mutex_lock (&ct_autodetection->ct_autodetection_mutex);
  g_assert (ct_autodetection->state == CT_AUTODETECT_STATE_NONE);

  transition_to_state (ct_autodetection, CT_AUTODETECT_STATE_MEASURE_BW_1);
  g_mutex_unlock (&ct_autodetection->ct_autodetection_mutex);

  detect_network_characteristics (ct_autodetection);
}

void
grd_rdp_connect_time_autodetection_invoke_shutdown (GrdRdpConnectTimeAutodetection *ct_autodetection)
{
  g_mutex_lock (&ct_autodetection->net_char_sync_mutex);
  ct_autodetection->protocol_stopped = TRUE;
  g_cond_signal (&ct_autodetection->net_char_sync_cond);
  g_mutex_unlock (&ct_autodetection->net_char_sync_mutex);
}

static uint32_t
get_bw_measure_phase (GrdRdpConnectTimeAutodetection *ct_autodetection)
{
  switch (ct_autodetection->state)
    {
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_1:
      return 1;
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_2:
      return 2;
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_3:
      return 3;
    default:
      g_assert_not_reached ();
    }

  return 0;
}

BOOL
grd_rdp_connect_time_autodetection_notify_bw_measure_results (GrdRdpConnectTimeAutodetection *ct_autodetection,
                                                              uint32_t                        time_delta_ms,
                                                              uint32_t                        byte_count)
{
  CtAutodetectState new_state = CT_AUTODETECT_STATE_NONE;
  g_autoptr (GMutexLocker) locker = NULL;
  int64_t current_time_us;
  int64_t response_latency_us;

  locker = g_mutex_locker_new (&ct_autodetection->ct_autodetection_mutex);
  switch (ct_autodetection->state)
    {
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_1:
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_2:
      current_time_us = g_get_monotonic_time ();
      response_latency_us = current_time_us - ct_autodetection->bw_measure_time_us;

      if (response_latency_us < CT_BW_MAX_TOLERABLE_RESPONSE_LATENCY_US &&
          time_delta_ms < CT_BW_MAX_TOLERABLE_TIME_DELTA_MS)
        new_state = get_next_bw_measure_state (ct_autodetection);
      else
        new_state = CT_AUTODETECT_STATE_START_RTT_DETECTION;
      break;
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_3:
      new_state = CT_AUTODETECT_STATE_START_RTT_DETECTION;
      break;
    default:
      g_warning ("[RDP] Connect-Time Autodetect: Received stray Bandwidth "
                 "Measure Results PDU (current state: %s)",
                 ct_autodetect_state_to_string (ct_autodetection->state));
      return FALSE;
    }

  g_debug ("[RDP] Connect-Time Autodetect: BW Measure Phase %u: "
           "time delta: %ums, byte count: %u => %uKB/s",
           get_bw_measure_phase (ct_autodetection), time_delta_ms, byte_count,
           byte_count / MAX (time_delta_ms, 1));

  ct_autodetection->last_time_delta_ms = time_delta_ms;
  ct_autodetection->last_byte_count = byte_count;

  g_assert (new_state != CT_AUTODETECT_STATE_NONE);
  transition_to_state (ct_autodetection, new_state);

  return TRUE;
}

void
grd_rdp_connect_time_autodetection_notify_rtt_measure_response (GrdRdpConnectTimeAutodetection *ct_autodetection,
                                                                uint16_t                        sequence_number,
                                                                int64_t                         base_round_trip_time_us,
                                                                int64_t                         avg_round_trip_time_us)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&ct_autodetection->ct_autodetection_mutex);
  if (ct_autodetection->state != CT_AUTODETECT_STATE_AWAIT_LAST_RTT_RESPONSE ||
      ct_autodetection->last_sequence_number != sequence_number)
    return;

  g_debug ("[RDP] Connect-Time Autodetect: base RTT: %lims, average RTT: %lims",
           base_round_trip_time_us / 1000, avg_round_trip_time_us / 1000);

  ct_autodetection->base_round_trip_time_us = base_round_trip_time_us;
  ct_autodetection->avg_round_trip_time_us = avg_round_trip_time_us;

  transition_to_state (ct_autodetection, CT_AUTODETECT_STATE_SEND_NET_CHAR_RESULT);
}

static gboolean
sync_with_main_context (gpointer user_data)
{
  GrdRdpConnectTimeAutodetection *ct_autodetection = user_data;

  g_mutex_lock (&ct_autodetection->ct_autodetection_mutex);
  if (ct_autodetection->ping_source)
    {
      g_source_destroy (ct_autodetection->ping_source);
      g_clear_pointer (&ct_autodetection->ping_source, g_source_unref);
    }
  g_mutex_unlock (&ct_autodetection->ct_autodetection_mutex);

  g_mutex_lock (&ct_autodetection->net_char_sync_mutex);
  g_clear_pointer (&ct_autodetection->sync_source, g_source_unref);

  ct_autodetection->pending_net_char_sync = FALSE;
  g_cond_signal (&ct_autodetection->net_char_sync_cond);
  g_mutex_unlock (&ct_autodetection->net_char_sync_mutex);

  return G_SOURCE_REMOVE;
}

static void
sync_ct_autodetect_handling (GrdRdpConnectTimeAutodetection *ct_autodetection)
{
  g_mutex_lock (&ct_autodetection->net_char_sync_mutex);
  ct_autodetection->pending_net_char_sync = TRUE;

  ct_autodetection->sync_source = g_idle_source_new ();
  g_source_set_callback (ct_autodetection->sync_source, sync_with_main_context,
                         ct_autodetection, NULL);
  g_source_attach (ct_autodetection->sync_source, NULL);

  while (!ct_autodetection->protocol_stopped &&
         ct_autodetection->pending_net_char_sync)
    {
      g_cond_wait (&ct_autodetection->net_char_sync_cond,
                   &ct_autodetection->net_char_sync_mutex);
    }
  g_mutex_unlock (&ct_autodetection->net_char_sync_mutex);
}

static BOOL
autodetect_network_characteristics_sync (rdpAutoDetect      *rdp_autodetect,
                                         RDP_TRANSPORT_TYPE  transport_type,
                                         uint16_t            sequenceNumber,
                                         uint32_t            bandwidth,
                                         uint32_t            rtt)
{
  GrdRdpNetworkAutodetection *network_autodetection = rdp_autodetect->custom;
  GrdRdpConnectTimeAutodetection *ct_autodetection =
    grd_rdp_network_autodetection_get_ct_handler (network_autodetection);
  g_autoptr (GMutexLocker) locker = NULL;
  gboolean pending_sync = FALSE;

  locker = g_mutex_locker_new (&ct_autodetection->ct_autodetection_mutex);
  switch (ct_autodetection->state)
    {
    case CT_AUTODETECT_STATE_NONE:
    case CT_AUTODETECT_STATE_MEASURE_BW_1:
    case CT_AUTODETECT_STATE_MEASURE_BW_2:
    case CT_AUTODETECT_STATE_MEASURE_BW_3:
      g_assert_not_reached ();
      break;
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_1:
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_2:
    case CT_AUTODETECT_STATE_AWAIT_BW_RESULT_3:
      g_assert (!pending_sync);
      break;
    case CT_AUTODETECT_STATE_START_RTT_DETECTION:
      g_assert_not_reached ();
      break;
    case CT_AUTODETECT_STATE_IN_RTT_DETECTION:
    case CT_AUTODETECT_STATE_AWAIT_LAST_RTT_RESPONSE:
      pending_sync = TRUE;
      break;
    case CT_AUTODETECT_STATE_SEND_NET_CHAR_RESULT:
      g_assert_not_reached ();
      break;
    case CT_AUTODETECT_STATE_COMPLETE:
      g_warning ("[RDP] Connect-Time Autodetect: Received stray Network "
                 "Characteristics Sync PDU (current state: %s). Ignoring PDU...",
                 ct_autodetect_state_to_string (ct_autodetection->state));
      return TRUE;
    }
  g_clear_pointer (&locker, g_mutex_locker_free);

  if (pending_sync)
    sync_ct_autodetect_handling (ct_autodetection);

  g_debug ("[RDP] Connect-Time Autodetect: Received Sync PDU: "
           "RTT: %ums, bandwidth: %uKBit/s", rtt, bandwidth);

  locker = g_mutex_locker_new (&ct_autodetection->ct_autodetection_mutex);
  transition_to_state (ct_autodetection, CT_AUTODETECT_STATE_COMPLETE);

  return TRUE;
}

static FREERDP_AUTODETECT_STATE
autodetect_on_connect_time_autodetect_progress (rdpAutoDetect *rdp_autodetect)
{
  GrdRdpNetworkAutodetection *network_autodetection = rdp_autodetect->custom;
  GrdRdpConnectTimeAutodetection *ct_autodetection =
    grd_rdp_network_autodetection_get_ct_handler (network_autodetection);

  return detect_network_characteristics (ct_autodetection);
}

GrdRdpConnectTimeAutodetection *
grd_rdp_connect_time_autodetection_new (GrdRdpNetworkAutodetection *network_autodetection,
                                        rdpAutoDetect              *rdp_autodetect)
{
  GrdRdpConnectTimeAutodetection *ct_autodetection;

  ct_autodetection = g_object_new (GRD_TYPE_RDP_CONNECT_TIME_AUTODETECTION, NULL);
  ct_autodetection->network_autodetection = network_autodetection;
  ct_autodetection->rdp_autodetect = rdp_autodetect;

  rdp_autodetect->NetworkCharacteristicsSync = autodetect_network_characteristics_sync;
  rdp_autodetect->OnConnectTimeAutoDetectProgress = autodetect_on_connect_time_autodetect_progress;

  return ct_autodetection;
}

static void
grd_rdp_connect_time_autodetection_dispose (GObject *object)
{
  GrdRdpConnectTimeAutodetection *ct_autodetection = GRD_RDP_CONNECT_TIME_AUTODETECTION (object);

  g_assert (ct_autodetection->protocol_stopped);

  if (ct_autodetection->sync_source)
    {
      g_source_destroy (ct_autodetection->sync_source);
      g_clear_pointer (&ct_autodetection->sync_source, g_source_unref);
    }
  if (ct_autodetection->ping_source)
    {
      g_source_destroy (ct_autodetection->ping_source);
      g_clear_pointer (&ct_autodetection->ping_source, g_source_unref);
    }

  G_OBJECT_CLASS (grd_rdp_connect_time_autodetection_parent_class)->dispose (object);
}

static void
grd_rdp_connect_time_autodetection_finalize (GObject *object)
{
  GrdRdpConnectTimeAutodetection *ct_autodetection = GRD_RDP_CONNECT_TIME_AUTODETECTION (object);

  g_cond_clear (&ct_autodetection->net_char_sync_cond);
  g_mutex_clear (&ct_autodetection->net_char_sync_mutex);
  g_mutex_clear (&ct_autodetection->ct_autodetection_mutex);

  G_OBJECT_CLASS (grd_rdp_connect_time_autodetection_parent_class)->finalize (object);
}

static void
grd_rdp_connect_time_autodetection_init (GrdRdpConnectTimeAutodetection *ct_autodetection)
{
  ct_autodetection->state = CT_AUTODETECT_STATE_NONE;

  ct_autodetection->pending_pings = CT_N_PINGS;

  g_mutex_init (&ct_autodetection->ct_autodetection_mutex);
  g_mutex_init (&ct_autodetection->net_char_sync_mutex);
  g_cond_init (&ct_autodetection->net_char_sync_cond);
}

static void
grd_rdp_connect_time_autodetection_class_init (GrdRdpConnectTimeAutodetectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_connect_time_autodetection_dispose;
  object_class->finalize = grd_rdp_connect_time_autodetection_finalize;
}
