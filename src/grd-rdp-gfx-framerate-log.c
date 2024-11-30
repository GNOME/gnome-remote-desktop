/*
 * Copyright (C) 2024 Pascal Nowack
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

#include "grd-rdp-gfx-framerate-log.h"

#include <math.h>

#include "grd-rdp-frame-stats.h"

#define STABLE_ENCODING_RATE_THRESHOLD_PERCENT 0.8

#define MIN_N_ENC_RATES 4
#define MIN_ENC_RATE_THRESHOLD 5
#define MIN_VIDEO_FRAMERATE 24

typedef struct
{
  uint32_t enc_rate;
  int64_t tracked_time_us;
} EncRateInfo;

struct _GrdRdpGfxFramerateLog
{
  GObject parent;

  GMutex framerate_log_mutex;
  GQueue *enc_rates;
  uint32_t last_ack_rate;

  uint32_t missing_stereo_frame_acks;
};

G_DEFINE_TYPE (GrdRdpGfxFramerateLog, grd_rdp_gfx_framerate_log,
               G_TYPE_OBJECT)

static void
clear_old_enc_rates (GrdRdpGfxFramerateLog *framerate_log)
{
  EncRateInfo *enc_rate_info;
  int64_t current_time_us;

  current_time_us = g_get_monotonic_time ();
  while ((enc_rate_info = g_queue_peek_head (framerate_log->enc_rates)) &&
         current_time_us - enc_rate_info->tracked_time_us >= G_USEC_PER_SEC >> 1)
    g_free (g_queue_pop_head (framerate_log->enc_rates));
}

void
grd_rdp_gfx_framerate_log_notify_frame_stats (GrdRdpGfxFramerateLog *framerate_log,
                                              GrdRdpFrameStats      *frame_stats)
{
  EncRateInfo *enc_rate_info;

  enc_rate_info = g_new0 (EncRateInfo, 1);
  enc_rate_info->enc_rate = grd_rdp_frame_stats_get_enc_rate (frame_stats);
  enc_rate_info->tracked_time_us = g_get_monotonic_time ();

  g_mutex_lock (&framerate_log->framerate_log_mutex);
  clear_old_enc_rates (framerate_log);
  g_queue_push_tail (framerate_log->enc_rates, enc_rate_info);

  framerate_log->last_ack_rate = grd_rdp_frame_stats_get_ack_rate (frame_stats);
  framerate_log->missing_stereo_frame_acks =
    grd_rdp_frame_stats_get_missing_stereo_frame_acks (frame_stats);
  g_mutex_unlock (&framerate_log->framerate_log_mutex);
}

static int
cmp_enc_rates (gconstpointer a,
               gconstpointer b,
               gpointer      user_data)
{
  const EncRateInfo *enc_rate_info_a = a;
  const EncRateInfo *enc_rate_info_b = b;
  int64_t enc_rate_a = enc_rate_info_a->enc_rate;
  int64_t enc_rate_b = enc_rate_info_b->enc_rate;

  return enc_rate_a - enc_rate_b;
}

static gboolean
has_stable_enc_rate (double enc_rate_min,
                     double enc_rate_median)
{
  return enc_rate_min >=
         floor (enc_rate_median * STABLE_ENCODING_RATE_THRESHOLD_PERCENT);
}

gboolean
grd_rdp_gfx_framerate_log_should_avoid_stereo_frame (GrdRdpGfxFramerateLog *framerate_log)
{
  g_autoptr (GMutexLocker) locker = NULL;
  g_autoptr (GQueue) tmp = NULL;
  EncRateInfo *enc_rate_info;
  uint32_t n_enc_rates;
  uint32_t enc_rate_min;
  uint32_t enc_rate_quartile3;
  uint32_t enc_rate_median;
  uint32_t last_ack_rate;
  uint32_t missing_stereo_frame_acks;
  uint32_t i;

  locker = g_mutex_locker_new (&framerate_log->framerate_log_mutex);
  clear_old_enc_rates (framerate_log);

  n_enc_rates = g_queue_get_length (framerate_log->enc_rates);
  if (n_enc_rates < MIN_N_ENC_RATES)
    return FALSE;

  tmp = g_queue_copy (framerate_log->enc_rates);
  g_queue_sort (tmp, cmp_enc_rates, NULL);

  enc_rate_info = g_queue_peek_head (tmp);
  enc_rate_min = enc_rate_info->enc_rate;

  for (i = 0; i < n_enc_rates >> 2; ++i)
    g_queue_pop_tail (tmp);

  enc_rate_info = g_queue_peek_tail (tmp);
  enc_rate_quartile3 = enc_rate_info->enc_rate;

  for (i = 0; i < n_enc_rates >> 2; ++i)
    g_queue_pop_tail (tmp);

  enc_rate_info = g_queue_peek_tail (tmp);
  enc_rate_median = enc_rate_info->enc_rate;

  last_ack_rate = framerate_log->last_ack_rate;
  missing_stereo_frame_acks = framerate_log->missing_stereo_frame_acks;
  g_clear_pointer (&locker, g_mutex_locker_free);

  if (enc_rate_median < MIN_ENC_RATE_THRESHOLD)
    return FALSE;

  if (enc_rate_median >= MIN_VIDEO_FRAMERATE ||
      has_stable_enc_rate (enc_rate_min, enc_rate_median))
    return enc_rate_quartile3 + 3 * missing_stereo_frame_acks >= last_ack_rate;

  return FALSE;
}

GrdRdpGfxFramerateLog *
grd_rdp_gfx_framerate_log_new (void)
{
  return g_object_new (GRD_TYPE_RDP_GFX_FRAMERATE_LOG, NULL);
}

static void
grd_rdp_gfx_framerate_log_dispose (GObject *object)
{
  GrdRdpGfxFramerateLog *framerate_log = GRD_RDP_GFX_FRAMERATE_LOG (object);

  if (framerate_log->enc_rates)
    {
      g_queue_free_full (framerate_log->enc_rates, g_free);
      framerate_log->enc_rates = NULL;
    }

  G_OBJECT_CLASS (grd_rdp_gfx_framerate_log_parent_class)->dispose (object);
}

static void
grd_rdp_gfx_framerate_log_finalize (GObject *object)
{
  GrdRdpGfxFramerateLog *framerate_log = GRD_RDP_GFX_FRAMERATE_LOG (object);

  g_mutex_clear (&framerate_log->framerate_log_mutex);

  G_OBJECT_CLASS (grd_rdp_gfx_framerate_log_parent_class)->finalize (object);
}

static void
grd_rdp_gfx_framerate_log_init (GrdRdpGfxFramerateLog *framerate_log)
{
  framerate_log->enc_rates = g_queue_new ();

  g_mutex_init (&framerate_log->framerate_log_mutex);
}

static void
grd_rdp_gfx_framerate_log_class_init (GrdRdpGfxFramerateLogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_gfx_framerate_log_dispose;
  object_class->finalize = grd_rdp_gfx_framerate_log_finalize;
}
