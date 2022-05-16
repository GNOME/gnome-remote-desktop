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

#include "grd-rdp-graphics-pipeline.h"
#include "grd-rdp-private.h"

#define BW_MEASURE_SEQUENCE_NUMBER 0
#define PING_INTERVAL_HIGH_MS 70
#define PING_INTERVAL_LOW_MS 700
#define RTT_AVG_PERIOD_US (500 * 1000)

typedef enum _PingInterval
{
  PING_INTERVAL_NONE,
  PING_INTERVAL_HIGH,
  PING_INTERVAL_LOW,
} PingInterval;

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

  rdpContext *rdp_context;
  rdpAutoDetect *rdp_autodetect;
  GMutex shutdown_mutex;
  gboolean in_shutdown;

  GMutex consumer_mutex;
  GrdRdpNwAutodetectRTTConsumer rtt_consumers;
  GrdRdpNwAutodetectRTTConsumer rtt_high_nec_consumers;

  GMutex sequence_mutex;
  GHashTable *sequences;

  GSource *ping_source;
  GQueue *pings;
  GQueue *round_trip_times;
  PingInterval ping_interval;

  uint16_t next_sequence_number;
};

G_DEFINE_TYPE (GrdRdpNetworkAutodetection,
               grd_rdp_network_autodetection,
               G_TYPE_OBJECT)

void
grd_rdp_network_autodetection_invoke_shutdown (GrdRdpNetworkAutodetection *network_autodetection)
{
  g_mutex_lock (&network_autodetection->shutdown_mutex);
  network_autodetection->in_shutdown = TRUE;
  g_mutex_unlock (&network_autodetection->shutdown_mutex);
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

static uint16_t
get_next_free_sequence_number (GrdRdpNetworkAutodetection *network_autodetection)
{
  uint16_t sequence_number = network_autodetection->next_sequence_number;

  while (sequence_number == BW_MEASURE_SEQUENCE_NUMBER ||
         g_hash_table_contains (network_autodetection->sequences,
                                GUINT_TO_POINTER (sequence_number)))
    ++sequence_number;

  network_autodetection->next_sequence_number = sequence_number + 1;

  return sequence_number;
}

static gboolean
emit_ping (gpointer user_data)
{
  GrdRdpNetworkAutodetection *network_autodetection = user_data;
  rdpAutoDetect *rdp_autodetect = network_autodetection->rdp_autodetect;
  PingInfo *ping_info;

  ping_info = g_malloc0 (sizeof (PingInfo));

  g_mutex_lock (&network_autodetection->sequence_mutex);
  ping_info->sequence_number = get_next_free_sequence_number (network_autodetection);
  ping_info->ping_time_us = g_get_monotonic_time ();

  g_hash_table_add (network_autodetection->sequences,
                    GUINT_TO_POINTER (ping_info->sequence_number));
  g_queue_push_tail (network_autodetection->pings, ping_info);
  g_mutex_unlock (&network_autodetection->sequence_mutex);

  rdp_autodetect->RTTMeasureRequest (network_autodetection->rdp_context,
                                     ping_info->sequence_number);

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

  if (network_autodetection->ping_interval != new_ping_interval_type &&
      network_autodetection->ping_source)
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

static void
track_round_trip_time (GrdRdpNetworkAutodetection *network_autodetection,
                       int64_t                     ping_time_us,
                       int64_t                     pong_time_us)
{
  RTTInfo *rtt_info;

  rtt_info = g_malloc0 (sizeof (RTTInfo));
  rtt_info->round_trip_time_us = MIN (pong_time_us - ping_time_us, G_USEC_PER_SEC);
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

static int64_t
get_current_avg_round_trip_time_us (GrdRdpNetworkAutodetection *network_autodetection)
{
  int64_t sum_round_trip_times_us = 0;
  uint32_t total_round_trip_times;
  RTTInfo *rtt_info;
  GQueue *tmp;

  remove_old_round_trip_times (network_autodetection);
  if (!g_queue_get_length (network_autodetection->round_trip_times))
    return 0;

  tmp = g_queue_copy (network_autodetection->round_trip_times);
  total_round_trip_times = g_queue_get_length (tmp);

  while ((rtt_info = g_queue_pop_head (tmp)))
    sum_round_trip_times_us += rtt_info->round_trip_time_us;

  g_queue_free (tmp);

  return sum_round_trip_times_us / total_round_trip_times;
}

static BOOL
autodetect_rtt_measure_response (rdpContext *rdp_context,
                                 uint16_t    sequence_number)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  GrdRdpNetworkAutodetection *network_autodetection;
  PingInfo *ping_info;
  int64_t pong_time_us;
  int64_t avg_round_trip_time_us;
  gboolean has_rtt_consumer_rdpgfx = FALSE;

  network_autodetection = rdp_peer_context->network_autodetection;

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

  if (ping_info)
    {
      int64_t ping_time_us = ping_info->ping_time_us;

      g_assert (ping_info->sequence_number == sequence_number);

      track_round_trip_time (network_autodetection, ping_time_us, pong_time_us);
      avg_round_trip_time_us =
        get_current_avg_round_trip_time_us (network_autodetection);

      g_hash_table_remove (network_autodetection->sequences,
                           GUINT_TO_POINTER (ping_info->sequence_number));
    }
  g_mutex_unlock (&network_autodetection->sequence_mutex);

  g_mutex_lock (&network_autodetection->consumer_mutex);
  has_rtt_consumer_rdpgfx = has_rtt_consumer (
    network_autodetection, GRD_RDP_NW_AUTODETECT_RTT_CONSUMER_RDPGFX);
  g_mutex_unlock (&network_autodetection->consumer_mutex);

  g_mutex_lock (&network_autodetection->shutdown_mutex);
  if (ping_info && !network_autodetection->in_shutdown && has_rtt_consumer_rdpgfx)
    {
      grd_rdp_graphics_pipeline_notify_new_round_trip_time (
        rdp_peer_context->graphics_pipeline, avg_round_trip_time_us);
    }
  g_mutex_unlock (&network_autodetection->shutdown_mutex);

  g_free (ping_info);

  return TRUE;
}

GrdRdpNetworkAutodetection *
grd_rdp_network_autodetection_new (rdpContext *rdp_context)
{
  GrdRdpNetworkAutodetection *network_autodetection;
  rdpAutoDetect *rdp_autodetect = rdp_context->autodetect;

  network_autodetection = g_object_new (GRD_TYPE_RDP_NETWORK_AUTODETECTION,
                                        NULL);

  network_autodetection->rdp_context = rdp_context;
  network_autodetection->rdp_autodetect = rdp_autodetect;

  rdp_autodetect->RTTMeasureResponse = autodetect_rtt_measure_response;

  return network_autodetection;
}

static void
grd_rdp_network_autodetection_dispose (GObject *object)
{
  GrdRdpNetworkAutodetection *network_autodetection =
    GRD_RDP_NETWORK_AUTODETECTION (object);

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

  G_OBJECT_CLASS (grd_rdp_network_autodetection_parent_class)->dispose (object);
}

static void
grd_rdp_network_autodetection_finalize (GObject *object)
{
  GrdRdpNetworkAutodetection *network_autodetection =
    GRD_RDP_NETWORK_AUTODETECTION (object);

  g_mutex_clear (&network_autodetection->sequence_mutex);
  g_mutex_clear (&network_autodetection->consumer_mutex);
  g_mutex_clear (&network_autodetection->shutdown_mutex);

  G_OBJECT_CLASS (grd_rdp_network_autodetection_parent_class)->finalize (object);
}

static void
grd_rdp_network_autodetection_init (GrdRdpNetworkAutodetection *network_autodetection)
{
  network_autodetection->ping_interval = PING_INTERVAL_NONE;

  network_autodetection->sequences = g_hash_table_new (NULL, NULL);
  network_autodetection->pings = g_queue_new ();
  network_autodetection->round_trip_times = g_queue_new ();

  g_mutex_init (&network_autodetection->shutdown_mutex);
  g_mutex_init (&network_autodetection->consumer_mutex);
  g_mutex_init (&network_autodetection->sequence_mutex);
}

static void
grd_rdp_network_autodetection_class_init (GrdRdpNetworkAutodetectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_network_autodetection_dispose;
  object_class->finalize = grd_rdp_network_autodetection_finalize;
}
