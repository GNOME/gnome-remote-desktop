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

#include "grd-rdp-network-autodetection.h"

#include "grd-rdp-connect-time-autodetection.h"
#include "grd-rdp-graphics-pipeline.h"
#include "grd-rdp-private.h"

#define BW_MEASURE_SEQUENCE_NUMBER 0
#define MIN_NW_CHAR_RES_INTERVAL_US G_USEC_PER_SEC
#define PING_INTERVAL_HIGH_MS 70
#define PING_INTERVAL_LOW_MS 700
#define RTT_AVG_PERIOD_US (500 * 1000)

typedef enum _PingInterval
{
  PING_INTERVAL_NONE,
  PING_INTERVAL_HIGH,
  PING_INTERVAL_LOW,
} PingInterval;

typedef enum _BwMeasureState
{
  BW_MEASURE_STATE_NONE,
  BW_MEASURE_STATE_PENDING_STOP,
  BW_MEASURE_STATE_QUEUED_STOP,
  BW_MEASURE_STATE_PENDING_RESULTS,
} BwMeasureState;

typedef struct _PingInfo
{
  uint16_t sequence_number;
  int64_t ping_time_us;
} PingInfo;

typedef struct _RTTInfo
{
  int64_t round_trip_time_us;
  int64_t response_time_us;
} RTTInfo;

struct _GrdRdpNetworkAutodetection
{
  GObject parent;

  rdpAutoDetect *rdp_autodetect;
  GMutex shutdown_mutex;
  gboolean in_shutdown;

  GrdRdpConnectTimeAutodetection *ct_autodetection;

  GMutex consumer_mutex;
  GrdRdpNwAutodetectRTTConsumer rtt_consumers;
  GrdRdpNwAutodetectRTTConsumer rtt_high_nec_consumers;

  GMutex sequence_mutex;
  GHashTable *sequences;

  GSource *ping_source;
  GQueue *pings;
  GQueue *round_trip_times;
  PingInterval ping_interval;

  GMutex bw_measure_mutex;
  uint32_t bandwidth_kbits;
  BwMeasureState bw_measure_state;
  HANDLE bw_measure_stop_event;

  int64_t last_nw_char_res_notification_us;

  uint16_t next_sequence_number;
};

G_DEFINE_TYPE (GrdRdpNetworkAutodetection,
               grd_rdp_network_autodetection,
               G_TYPE_OBJECT)

GrdRdpConnectTimeAutodetection *
grd_rdp_network_autodetection_get_ct_handler (GrdRdpNetworkAutodetection *network_autodetection)
{
  return network_autodetection->ct_autodetection;
}

void
grd_rdp_network_autodetection_start_connect_time_autodetection (GrdRdpNetworkAutodetection *network_autodetection)
{
  GrdRdpConnectTimeAutodetection *ct_autodetection =
    network_autodetection->ct_autodetection;

  grd_rdp_connect_time_autodetection_start_detection (ct_autodetection);
}

void
grd_rdp_network_autodetection_invoke_shutdown (GrdRdpNetworkAutodetection *network_autodetection)
{
  GrdRdpConnectTimeAutodetection *ct_autodetection =
    network_autodetection->ct_autodetection;

  g_mutex_lock (&network_autodetection->shutdown_mutex);
  network_autodetection->in_shutdown = TRUE;
  g_mutex_unlock (&network_autodetection->shutdown_mutex);

  grd_rdp_connect_time_autodetection_invoke_shutdown (ct_autodetection);
}

static uint16_t
get_next_free_sequence_number (GrdRdpNetworkAutodetection *network_autodetection)
{
  uint16_t sequence_number = network_autodetection->next_sequence_number;

  if (g_hash_table_size (network_autodetection->sequences) > UINT16_MAX >> 2)
    {
      g_warning ("[RDP] Network Autodetect: Protocol violation: Client leaves "
                 "requests unanswered");
      g_hash_table_remove_all (network_autodetection->sequences);
    }

  while (sequence_number == BW_MEASURE_SEQUENCE_NUMBER ||
         g_hash_table_contains (network_autodetection->sequences,
                                GUINT_TO_POINTER (sequence_number)))
    ++sequence_number;

  network_autodetection->next_sequence_number = sequence_number + 1;

  return sequence_number;
}

static void
emit_ping_with_callback (GrdRdpNetworkAutodetection                    *network_autodetection,
                         GrdRdpNwAutodetectSequenceNumberReadyCallback  callback,
                         gpointer                                       callback_user_data)
{
  rdpAutoDetect *rdp_autodetect = network_autodetection->rdp_autodetect;
  uint16_t sequence_number;
  PingInfo *ping_info;

  ping_info = g_new0 (PingInfo, 1);

  g_mutex_lock (&network_autodetection->sequence_mutex);
  sequence_number = get_next_free_sequence_number (network_autodetection);

  ping_info->sequence_number = sequence_number;
  ping_info->ping_time_us = g_get_monotonic_time ();

  g_hash_table_add (network_autodetection->sequences,
                    GUINT_TO_POINTER (sequence_number));
  g_queue_push_tail (network_autodetection->pings, ping_info);
  g_mutex_unlock (&network_autodetection->sequence_mutex);

  if (callback)
    callback (callback_user_data, sequence_number);

  rdp_autodetect->RTTMeasureRequest (rdp_autodetect, RDP_TRANSPORT_TCP,
                                     sequence_number);
}

void
grd_rdp_network_autodetection_emit_ping (GrdRdpNetworkAutodetection                    *network_autodetection,
                                         GrdRdpNwAutodetectSequenceNumberReadyCallback  callback,
                                         gpointer                                       callback_user_data)
{
  emit_ping_with_callback (network_autodetection, callback, callback_user_data);
}

static gboolean
has_rtt_consumer (GrdRdpNetworkAutodetection    *network_autodetection,
                  GrdRdpNwAutodetectRTTConsumer  rtt_consumer)
{
  g_assert (!g_mutex_trylock (&network_autodetection->consumer_mutex));

  return !!(network_autodetection->rtt_consumers & rtt_consumer);
}

static gboolean
is_active_high_nec_rtt_consumer (GrdRdpNetworkAutodetection    *network_autodetection,
                                 GrdRdpNwAutodetectRTTConsumer  rtt_consumer)
{
  if (!has_rtt_consumer (network_autodetection, rtt_consumer))
    return FALSE;
  if (network_autodetection->rtt_high_nec_consumers & rtt_consumer)
    return TRUE;

  return FALSE;
}

static gboolean
has_active_high_nec_rtt_consumers (GrdRdpNetworkAutodetection *network_autodetection)
{
  if (is_active_high_nec_rtt_consumer (network_autodetection,
                                       GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_RDPGFX))
    return TRUE;

  return FALSE;
}

static gboolean
emit_ping (gpointer user_data)
{
  GrdRdpNetworkAutodetection *network_autodetection = user_data;

  emit_ping_with_callback (network_autodetection, NULL, NULL);

  return G_SOURCE_CONTINUE;
}

static void
update_ping_source (GrdRdpNetworkAutodetection *network_autodetection)
{
  PingInterval new_ping_interval_type;
  uint32_t ping_interval_ms;

  if (network_autodetection->rtt_consumers == GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_NONE)
    new_ping_interval_type = PING_INTERVAL_NONE;
  else if (has_active_high_nec_rtt_consumers (network_autodetection))
    new_ping_interval_type = PING_INTERVAL_HIGH;
  else
    new_ping_interval_type = PING_INTERVAL_LOW;

  if (network_autodetection->ping_interval == new_ping_interval_type)
    return;

  if (network_autodetection->ping_source)
    {
      g_source_destroy (network_autodetection->ping_source);
      g_clear_pointer (&network_autodetection->ping_source, g_source_unref);
    }

  network_autodetection->ping_interval = new_ping_interval_type;

  if (network_autodetection->ping_interval == PING_INTERVAL_NONE)
    return;

  g_assert (!network_autodetection->ping_source);
  emit_ping (network_autodetection);

  switch (new_ping_interval_type)
    {
    case PING_INTERVAL_NONE:
      g_assert_not_reached ();
    case PING_INTERVAL_HIGH:
      ping_interval_ms = PING_INTERVAL_HIGH_MS;
      break;
    case PING_INTERVAL_LOW:
      ping_interval_ms = PING_INTERVAL_LOW_MS;
      break;
    }

  network_autodetection->ping_source = g_timeout_source_new (ping_interval_ms);
  g_source_set_callback (network_autodetection->ping_source, emit_ping,
                         network_autodetection, NULL);
  g_source_attach (network_autodetection->ping_source, NULL);
}

void
grd_rdp_network_autodetection_ensure_rtt_consumer (GrdRdpNetworkAutodetection    *network_autodetection,
                                                   GrdRdpNwAutodetectRTTConsumer  rtt_consumer)
{
  g_assert (rtt_consumer != GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_NONE);

  g_mutex_lock (&network_autodetection->consumer_mutex);
  network_autodetection->rtt_consumers |= rtt_consumer;

  update_ping_source (network_autodetection);
  g_mutex_unlock (&network_autodetection->consumer_mutex);
}

void
grd_rdp_network_autodetection_remove_rtt_consumer (GrdRdpNetworkAutodetection    *network_autodetection,
                                                   GrdRdpNwAutodetectRTTConsumer  rtt_consumer)
{
  g_assert (rtt_consumer != GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_NONE);

  g_mutex_lock (&network_autodetection->consumer_mutex);
  network_autodetection->rtt_consumers &= ~rtt_consumer;

  update_ping_source (network_autodetection);
  g_mutex_unlock (&network_autodetection->consumer_mutex);
}

void
grd_rdp_network_autodetection_set_rtt_consumer_necessity (GrdRdpNetworkAutodetection     *network_autodetection,
                                                          GrdRdpNwAutodetectRTTConsumer   rtt_consumer,
                                                          GrdRdpNwAutodetectRTTNecessity  rtt_necessity)
{
  GrdRdpNwAutodetectRTTNecessity current_rtt_necessity;

  g_assert (rtt_consumer != GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_NONE);
  g_assert (rtt_necessity == GRD_RDP_NW_AUTODETECT_RTT_NEC_HIGH ||
            rtt_necessity == GRD_RDP_NW_AUTODETECT_RTT_NEC_LOW);

  g_mutex_lock (&network_autodetection->consumer_mutex);
  if (network_autodetection->rtt_high_nec_consumers & rtt_consumer)
    current_rtt_necessity = GRD_RDP_NW_AUTODETECT_RTT_NEC_HIGH;
  else
    current_rtt_necessity = GRD_RDP_NW_AUTODETECT_RTT_NEC_LOW;

  if (current_rtt_necessity == rtt_necessity)
    {
      g_mutex_unlock (&network_autodetection->consumer_mutex);
      return;
    }

  switch (rtt_necessity)
    {
    case GRD_RDP_NW_AUTODETECT_RTT_NEC_HIGH:
      network_autodetection->rtt_high_nec_consumers |= rtt_consumer;
      break;
    case GRD_RDP_NW_AUTODETECT_RTT_NEC_LOW:
      network_autodetection->rtt_high_nec_consumers &= ~rtt_consumer;
      break;
    }

  if (has_rtt_consumer (network_autodetection, rtt_consumer))
    update_ping_source (network_autodetection);
  g_mutex_unlock (&network_autodetection->consumer_mutex);
}

gboolean
grd_rdp_network_autodetection_try_bw_measure_start (GrdRdpNetworkAutodetection *network_autodetection)
{
  rdpAutoDetect *rdp_autodetect = network_autodetection->rdp_autodetect;
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&network_autodetection->bw_measure_mutex);
  if (network_autodetection->bw_measure_state != BW_MEASURE_STATE_NONE)
    return FALSE;

  network_autodetection->bw_measure_state = BW_MEASURE_STATE_PENDING_STOP;
  g_clear_pointer (&locker, g_mutex_locker_free);

  rdp_autodetect->BandwidthMeasureStart (rdp_autodetect, RDP_TRANSPORT_TCP,
                                         BW_MEASURE_SEQUENCE_NUMBER);

  return TRUE;
}

void
grd_rdp_network_autodetection_bw_measure_stop (GrdRdpNetworkAutodetection *network_autodetection)
{
  rdpAutoDetect *rdp_autodetect = network_autodetection->rdp_autodetect;
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&network_autodetection->bw_measure_mutex);
  ResetEvent (network_autodetection->bw_measure_stop_event);

  if (network_autodetection->bw_measure_state != BW_MEASURE_STATE_QUEUED_STOP)
    return;

  network_autodetection->bw_measure_state = BW_MEASURE_STATE_PENDING_RESULTS;
  g_clear_pointer (&locker, g_mutex_locker_free);

  rdp_autodetect->BandwidthMeasureStop (rdp_autodetect, RDP_TRANSPORT_TCP,
                                        BW_MEASURE_SEQUENCE_NUMBER, 0);
}

void
grd_rdp_network_autodetection_queue_bw_measure_stop (GrdRdpNetworkAutodetection *network_autodetection)
{
  g_mutex_lock (&network_autodetection->bw_measure_mutex);
  g_assert (network_autodetection->bw_measure_state == BW_MEASURE_STATE_PENDING_STOP);

  SetEvent (network_autodetection->bw_measure_stop_event);
  network_autodetection->bw_measure_state = BW_MEASURE_STATE_QUEUED_STOP;
  g_mutex_unlock (&network_autodetection->bw_measure_mutex);
}

HANDLE
grd_rdp_network_autodetection_get_bw_measure_stop_event_handle (GrdRdpNetworkAutodetection *network_autodetection)
{
  return network_autodetection->bw_measure_stop_event;
}

static void
track_round_trip_time (GrdRdpNetworkAutodetection *network_autodetection,
                       int64_t                     ping_time_us,
                       int64_t                     pong_time_us)
{
  RTTInfo *rtt_info;

  rtt_info = g_malloc0 (sizeof (RTTInfo));
  rtt_info->round_trip_time_us = pong_time_us - ping_time_us;
  rtt_info->response_time_us = pong_time_us;

  g_queue_push_tail (network_autodetection->round_trip_times, rtt_info);
}

static void
remove_old_round_trip_times (GrdRdpNetworkAutodetection *network_autodetection)
{
  int64_t current_time_us;
  RTTInfo *rtt_info;

  current_time_us = g_get_monotonic_time ();
  while ((rtt_info = g_queue_peek_head (network_autodetection->round_trip_times)) &&
         current_time_us - rtt_info->response_time_us >= RTT_AVG_PERIOD_US)
    g_free (g_queue_pop_head (network_autodetection->round_trip_times));
}

static void
update_round_trip_time_values (GrdRdpNetworkAutodetection *network_autodetection,
                               int64_t                    *base_round_trip_time_us,
                               int64_t                    *avg_round_trip_time_us)
{
  int64_t sum_round_trip_times_us = 0;
  uint32_t total_round_trip_times;
  RTTInfo *rtt_info;
  GQueue *tmp;

  *base_round_trip_time_us = 0;
  *avg_round_trip_time_us = 0;

  remove_old_round_trip_times (network_autodetection);
  if (g_queue_get_length (network_autodetection->round_trip_times) == 0)
    return;

  tmp = g_queue_copy (network_autodetection->round_trip_times);
  total_round_trip_times = g_queue_get_length (tmp);
  g_assert (total_round_trip_times > 0);

  *base_round_trip_time_us = INT64_MAX;

  while ((rtt_info = g_queue_pop_head (tmp)))
    {
      *base_round_trip_time_us = MIN (*base_round_trip_time_us,
                                      rtt_info->round_trip_time_us);
      sum_round_trip_times_us += rtt_info->round_trip_time_us;
    }

  g_queue_free (tmp);

  *avg_round_trip_time_us = sum_round_trip_times_us / total_round_trip_times;
}

static void
maybe_send_network_characteristics_results (GrdRdpNetworkAutodetection *network_autodetection,
                                            uint32_t                    base_round_trip_time_ms,
                                            uint32_t                    avg_round_trip_time_ms)
{
  rdpAutoDetect *rdp_autodetect = network_autodetection->rdp_autodetect;
  uint32_t bandwidth_kbits = network_autodetection->bandwidth_kbits;
  rdpNetworkCharacteristicsResult result = {};
  int64_t last_notification_us;
  int64_t current_time_us;
  uint16_t sequence_number;

  if (bandwidth_kbits == 0)
    return;

  if (g_queue_get_length (network_autodetection->round_trip_times) == 0)
    return;

  current_time_us = g_get_monotonic_time ();
  last_notification_us = network_autodetection->last_nw_char_res_notification_us;

  if (current_time_us - last_notification_us < MIN_NW_CHAR_RES_INTERVAL_US)
    return;

  result.type = RDP_NETCHAR_RESULT_TYPE_BASE_RTT_BW_AVG_RTT;
  result.baseRTT = base_round_trip_time_ms;
  result.averageRTT = avg_round_trip_time_ms;
  result.bandwidth = bandwidth_kbits;

  g_mutex_lock (&network_autodetection->sequence_mutex);
  sequence_number = get_next_free_sequence_number (network_autodetection);
  g_mutex_unlock (&network_autodetection->sequence_mutex);

  rdp_autodetect->NetworkCharacteristicsResult (rdp_autodetect,
                                                RDP_TRANSPORT_TCP,
                                                sequence_number,
                                                &result);

  network_autodetection->last_nw_char_res_notification_us = current_time_us;
}

static BOOL
autodetect_rtt_measure_response (rdpAutoDetect      *rdp_autodetect,
                                 RDP_TRANSPORT_TYPE  transport_type,
                                 uint16_t            sequence_number)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_autodetect->context;
  GrdRdpNetworkAutodetection *network_autodetection = rdp_autodetect->custom;
  GrdRdpConnectTimeAutodetection *ct_autodetection =
    network_autodetection->ct_autodetection;
  g_autofree PingInfo *ping_info = NULL;
  int64_t pong_time_us;
  int64_t ping_time_us;
  int64_t base_round_trip_time_us;
  int64_t avg_round_trip_time_us;
  gboolean has_rtt_consumer_rdpgfx = FALSE;

  g_assert (transport_type == RDP_TRANSPORT_TCP);

  pong_time_us = g_get_monotonic_time ();

  g_mutex_lock (&network_autodetection->sequence_mutex);
  if (!g_hash_table_contains (network_autodetection->sequences,
                              GUINT_TO_POINTER (sequence_number)))
    {
      g_mutex_unlock (&network_autodetection->sequence_mutex);
      return TRUE;
    }

  while ((ping_info = g_queue_pop_head (network_autodetection->pings)) &&
         ping_info->sequence_number != sequence_number)
    {
      g_hash_table_remove (network_autodetection->sequences,
                           GUINT_TO_POINTER (ping_info->sequence_number));
      g_clear_pointer (&ping_info, g_free);
    }

  g_assert (ping_info);
  g_assert (ping_info->sequence_number == sequence_number);

  g_hash_table_remove (network_autodetection->sequences,
                       GUINT_TO_POINTER (ping_info->sequence_number));
  g_mutex_unlock (&network_autodetection->sequence_mutex);

  ping_time_us = ping_info->ping_time_us;
  track_round_trip_time (network_autodetection, ping_time_us, pong_time_us);
  update_round_trip_time_values (network_autodetection,
                                 &base_round_trip_time_us,
                                 &avg_round_trip_time_us);

  if (!grd_rdp_connect_time_autodetection_is_complete (ct_autodetection))
    {
      grd_rdp_connect_time_autodetection_notify_rtt_measure_response (ct_autodetection,
                                                                      sequence_number,
                                                                      base_round_trip_time_us,
                                                                      avg_round_trip_time_us);
      return TRUE;
    }

  g_mutex_lock (&network_autodetection->consumer_mutex);
  has_rtt_consumer_rdpgfx = has_rtt_consumer (
    network_autodetection, GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_RDPGFX);
  g_mutex_unlock (&network_autodetection->consumer_mutex);

  g_mutex_lock (&network_autodetection->shutdown_mutex);
  if (!network_autodetection->in_shutdown && has_rtt_consumer_rdpgfx &&
      rdp_peer_context->graphics_pipeline)
    {
      grd_rdp_graphics_pipeline_notify_new_round_trip_time (
        rdp_peer_context->graphics_pipeline, avg_round_trip_time_us);
    }
  g_mutex_unlock (&network_autodetection->shutdown_mutex);

  maybe_send_network_characteristics_results (network_autodetection,
                                              base_round_trip_time_us / 1000,
                                              avg_round_trip_time_us / 1000);

  return TRUE;
}

static BOOL
handle_bw_measure_results_connect_time (GrdRdpNetworkAutodetection *network_autodetection,
                                        RDP_TRANSPORT_TYPE          transport_type,
                                        uint32_t                    time_delta_ms,
                                        uint32_t                    byte_count)
{
  GrdRdpConnectTimeAutodetection *ct_autodetection =
    network_autodetection->ct_autodetection;
  uint64_t bit_count;
  uint32_t bandwidth_kbits;

  g_assert (transport_type == RDP_TRANSPORT_TCP);

  if (!grd_rdp_connect_time_autodetection_notify_bw_measure_results (ct_autodetection,
                                                                     time_delta_ms,
                                                                     byte_count))
    return FALSE;

  bit_count = ((uint64_t) byte_count) * UINT64_C (8);
  bandwidth_kbits = bit_count / MAX (time_delta_ms, 1);

  network_autodetection->bandwidth_kbits = bandwidth_kbits;

  return TRUE;
}

static BOOL
handle_bw_measure_results_continuous (GrdRdpNetworkAutodetection *network_autodetection,
                                      uint32_t                    time_delta_ms,
                                      uint32_t                    byte_count)
{
  g_autoptr (GMutexLocker) locker = NULL;
  int64_t base_round_trip_time_us;
  int64_t avg_round_trip_time_us;
  uint64_t bit_count;

  locker = g_mutex_locker_new (&network_autodetection->bw_measure_mutex);
  if (network_autodetection->bw_measure_state != BW_MEASURE_STATE_PENDING_RESULTS)
    return TRUE;

  network_autodetection->bw_measure_state = BW_MEASURE_STATE_NONE;
  g_clear_pointer (&locker, g_mutex_locker_free);

  bit_count = ((uint64_t) byte_count) * UINT64_C (8);
  network_autodetection->bandwidth_kbits = bit_count / MAX (time_delta_ms, 1);

  update_round_trip_time_values (network_autodetection,
                                 &base_round_trip_time_us,
                                 &avg_round_trip_time_us);

  maybe_send_network_characteristics_results (network_autodetection,
                                              base_round_trip_time_us / 1000,
                                              avg_round_trip_time_us / 1000);

  return TRUE;
}

static BOOL
autodetect_bw_measure_results (rdpAutoDetect      *rdp_autodetect,
                               RDP_TRANSPORT_TYPE  transport_type,
                               uint16_t            sequence_number,
                               uint16_t            response_type,
                               uint32_t            time_delta_ms,
                               uint32_t            byte_count)
{
  GrdRdpNetworkAutodetection *network_autodetection = rdp_autodetect->custom;
  GrdRdpConnectTimeAutodetection *ct_autodetection =
    network_autodetection->ct_autodetection;

  if (!grd_rdp_connect_time_autodetection_is_complete (ct_autodetection))
    {
      return handle_bw_measure_results_connect_time (network_autodetection,
                                                     transport_type,
                                                     time_delta_ms, byte_count);
    }

  return handle_bw_measure_results_continuous (network_autodetection,
                                               time_delta_ms, byte_count);
}

GrdRdpNetworkAutodetection *
grd_rdp_network_autodetection_new (rdpContext *rdp_context)
{
  GrdRdpNetworkAutodetection *network_autodetection;
  rdpAutoDetect *rdp_autodetect = autodetect_get (rdp_context);

  network_autodetection = g_object_new (GRD_TYPE_RDP_NETWORK_AUTODETECTION,
                                        NULL);
  network_autodetection->rdp_autodetect = rdp_autodetect;

  rdp_autodetect->RTTMeasureResponse = autodetect_rtt_measure_response;
  rdp_autodetect->BandwidthMeasureResults = autodetect_bw_measure_results;
  rdp_autodetect->custom = network_autodetection;

  network_autodetection->ct_autodetection =
    grd_rdp_connect_time_autodetection_new (network_autodetection,
                                            rdp_autodetect);

  return network_autodetection;
}

static void
grd_rdp_network_autodetection_dispose (GObject *object)
{
  GrdRdpNetworkAutodetection *network_autodetection =
    GRD_RDP_NETWORK_AUTODETECTION (object);

  g_clear_object (&network_autodetection->ct_autodetection);

  if (network_autodetection->ping_source)
    {
      g_source_destroy (network_autodetection->ping_source);
      g_clear_pointer (&network_autodetection->ping_source, g_source_unref);
    }

  if (network_autodetection->round_trip_times)
    {
      g_queue_free_full (network_autodetection->round_trip_times, g_free);
      network_autodetection->round_trip_times = NULL;
    }

  if (network_autodetection->pings)
    {
      g_queue_free_full (network_autodetection->pings, g_free);
      network_autodetection->pings = NULL;
    }

  g_clear_pointer (&network_autodetection->sequences, g_hash_table_destroy);

  g_clear_pointer (&network_autodetection->bw_measure_stop_event, CloseHandle);

  G_OBJECT_CLASS (grd_rdp_network_autodetection_parent_class)->dispose (object);
}

static void
grd_rdp_network_autodetection_finalize (GObject *object)
{
  GrdRdpNetworkAutodetection *network_autodetection =
    GRD_RDP_NETWORK_AUTODETECTION (object);

  g_mutex_clear (&network_autodetection->bw_measure_mutex);
  g_mutex_clear (&network_autodetection->sequence_mutex);
  g_mutex_clear (&network_autodetection->consumer_mutex);
  g_mutex_clear (&network_autodetection->shutdown_mutex);

  G_OBJECT_CLASS (grd_rdp_network_autodetection_parent_class)->finalize (object);
}

static void
grd_rdp_network_autodetection_init (GrdRdpNetworkAutodetection *network_autodetection)
{
  network_autodetection->ping_interval = PING_INTERVAL_NONE;
  network_autodetection->bw_measure_state = BW_MEASURE_STATE_NONE;

  network_autodetection->bw_measure_stop_event =
    CreateEvent (NULL, TRUE, FALSE, NULL);

  network_autodetection->sequences = g_hash_table_new (NULL, NULL);
  network_autodetection->pings = g_queue_new ();
  network_autodetection->round_trip_times = g_queue_new ();

  g_mutex_init (&network_autodetection->shutdown_mutex);
  g_mutex_init (&network_autodetection->consumer_mutex);
  g_mutex_init (&network_autodetection->sequence_mutex);
  g_mutex_init (&network_autodetection->bw_measure_mutex);
}

static void
grd_rdp_network_autodetection_class_init (GrdRdpNetworkAutodetectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_network_autodetection_dispose;
  object_class->finalize = grd_rdp_network_autodetection_finalize;
}
