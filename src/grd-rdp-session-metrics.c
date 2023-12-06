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

#include "grd-rdp-session-metrics.h"

#include "grd-rdp-surface.h"

typedef struct
{
  gboolean pending_frame_reception;
  gboolean pending_frame_transmission;
  int64_t first_frame_reception;
  int64_t first_frame_transmission;
  uint32_t skipped_frames;
} SurfaceMetrics;

struct _GrdRdpSessionMetrics
{
  GObject parent;

  GrdRdpPhase phase;

  int64_t rd_session_init_us;
  int64_t rd_session_post_connect_us;
  int64_t rd_session_ready_us;
  int64_t rd_session_started_us;

  gboolean pending_layout_change;
  int64_t layout_change_notification;

  GMutex metrics_mutex;
  GHashTable *surface_metrics_table;
  uint32_t n_pending_surface_metrics;

  gboolean pending_output;
};

G_DEFINE_TYPE (GrdRdpSessionMetrics, grd_rdp_session_metrics, G_TYPE_OBJECT)

void
grd_rdp_session_metrics_notify_phase_completion (GrdRdpSessionMetrics *session_metrics,
                                                 GrdRdpPhase           phase)
{
  g_assert (session_metrics->pending_output);

  g_assert (phase > session_metrics->phase);
  session_metrics->phase = phase;

  switch (phase)
    {
    case GRD_RDP_PHASE_SESSION_INITIALIZATION:
      g_assert_not_reached ();
      break;
    case GRD_RDP_PHASE_POST_CONNECT:
      session_metrics->rd_session_post_connect_us = g_get_monotonic_time ();
      break;
    case GRD_RDP_PHASE_SESSION_READY:
      session_metrics->rd_session_ready_us = g_get_monotonic_time ();
      break;
    case GRD_RDP_PHASE_SESSION_STARTED:
      session_metrics->rd_session_started_us = g_get_monotonic_time ();
      break;
    }
}

void
grd_rdp_session_metrics_notify_frame_reception (GrdRdpSessionMetrics *session_metrics,
                                                GrdRdpSurface        *rdp_surface)
{
  SurfaceMetrics *surface_metrics = NULL;
  g_autoptr (GMutexLocker) locker = NULL;

  g_assert (session_metrics->phase == GRD_RDP_PHASE_SESSION_STARTED);

  locker = g_mutex_locker_new (&session_metrics->metrics_mutex);
  if (session_metrics->pending_layout_change)
    return;

  if (!session_metrics->pending_output)
    return;

  if (!g_hash_table_lookup_extended (session_metrics->surface_metrics_table,
                                     rdp_surface,
                                     NULL, (gpointer *) &surface_metrics))
    g_assert_not_reached ();

  if (surface_metrics->pending_frame_reception)
    {
      g_assert (surface_metrics->pending_frame_transmission);

      surface_metrics->first_frame_reception = g_get_monotonic_time ();
      surface_metrics->pending_frame_reception = FALSE;
    }
  else if (surface_metrics->pending_frame_transmission)
    {
      ++surface_metrics->skipped_frames;
    }
}

static void
print_session_metrics (GrdRdpSessionMetrics *session_metrics)
{
  GrdRdpSurface *rdp_surface = NULL;
  SurfaceMetrics *surface_metrics = NULL;
  GHashTableIter iter;

  g_debug ("[RDP] Remote Desktop session metrics: _post_connect: %lims, "
           "_ready: %lims, _started: %lims, whole startup: %lims",
           (session_metrics->rd_session_post_connect_us -
            session_metrics->rd_session_init_us) / 1000,
           (session_metrics->rd_session_ready_us -
            session_metrics->rd_session_post_connect_us) / 1000,
           (session_metrics->rd_session_started_us -
            session_metrics->rd_session_ready_us) / 1000,
           (session_metrics->rd_session_started_us -
            session_metrics->rd_session_init_us) / 1000);

  g_hash_table_iter_init (&iter, session_metrics->surface_metrics_table);
  while (g_hash_table_iter_next (&iter, (gpointer *) &rdp_surface,
                                        (gpointer *) &surface_metrics))
    {
      GrdRdpSurfaceMapping *surface_mapping =
        grd_rdp_surface_get_mapping (rdp_surface);
      GrdRdpSurfaceMappingType mapping_type = surface_mapping->mapping_type;

      g_assert (!surface_metrics->pending_frame_reception);
      g_assert (!surface_metrics->pending_frame_transmission);

      g_assert (mapping_type == GRD_RDP_SURFACE_MAPPING_TYPE_MAP_TO_OUTPUT);

      g_debug ("[RDP] Surface %ux%u (%i, %i) Metrics: First frame: "
               "Reception: %lims, Transmission: %lims, Skipped frames: %u",
               grd_rdp_surface_get_width (rdp_surface),
               grd_rdp_surface_get_height (rdp_surface),
               surface_mapping->output_origin_x,
               surface_mapping->output_origin_y,
               (surface_metrics->first_frame_reception -
                session_metrics->layout_change_notification) / 1000,
               (surface_metrics->first_frame_transmission -
                surface_metrics->first_frame_reception) / 1000,
               surface_metrics->skipped_frames);
    }
}

void
grd_rdp_session_metrics_notify_frame_transmission (GrdRdpSessionMetrics *session_metrics,
                                                   GrdRdpSurface        *rdp_surface)
{
  SurfaceMetrics *surface_metrics = NULL;
  g_autoptr (GMutexLocker) locker = NULL;

  g_assert (session_metrics->phase == GRD_RDP_PHASE_SESSION_STARTED);

  locker = g_mutex_locker_new (&session_metrics->metrics_mutex);
  g_assert (!session_metrics->pending_layout_change);

  if (!session_metrics->pending_output)
    return;

  if (!g_hash_table_lookup_extended (session_metrics->surface_metrics_table,
                                     rdp_surface,
                                     NULL, (gpointer *) &surface_metrics))
    g_assert_not_reached ();

  g_assert (!surface_metrics->pending_frame_reception);

  if (surface_metrics->pending_frame_transmission)
    {
      surface_metrics->first_frame_transmission = g_get_monotonic_time ();
      surface_metrics->pending_frame_transmission = FALSE;

      g_assert (session_metrics->n_pending_surface_metrics > 0);
      --session_metrics->n_pending_surface_metrics;
    }

  if (session_metrics->n_pending_surface_metrics == 0)
    {
      print_session_metrics (session_metrics);
      session_metrics->pending_output = FALSE;
    }
}

void
grd_rdp_session_metrics_notify_layout_change (GrdRdpSessionMetrics *session_metrics)
{
  g_autoptr (GMutexLocker) locker = NULL;

  g_assert (session_metrics->phase == GRD_RDP_PHASE_SESSION_STARTED);

  locker = g_mutex_locker_new (&session_metrics->metrics_mutex);
  session_metrics->layout_change_notification = g_get_monotonic_time ();
  session_metrics->pending_layout_change = TRUE;
}

void
grd_rdp_session_metrics_prepare_surface_metrics (GrdRdpSessionMetrics *session_metrics,
                                                 GList                *surfaces)
{
  g_autoptr (GMutexLocker) locker = NULL;
  GList *l;

  g_assert (session_metrics->phase == GRD_RDP_PHASE_SESSION_STARTED);

  locker = g_mutex_locker_new (&session_metrics->metrics_mutex);
  g_hash_table_remove_all (session_metrics->surface_metrics_table);
  session_metrics->n_pending_surface_metrics = 0;

  for (l = surfaces; l; l = l->next)
    {
      GrdRdpSurface *rdp_surface = l->data;
      SurfaceMetrics *surface_metrics;

      surface_metrics = g_new0 (SurfaceMetrics, 1);
      surface_metrics->pending_frame_reception = TRUE;
      surface_metrics->pending_frame_transmission = TRUE;

      ++session_metrics->n_pending_surface_metrics;
      g_hash_table_insert (session_metrics->surface_metrics_table,
                           rdp_surface, surface_metrics);
    }

  session_metrics->pending_layout_change = FALSE;
}

GrdRdpSessionMetrics *
grd_rdp_session_metrics_new (void)
{
  return g_object_new (GRD_TYPE_RDP_SESSION_METRICS, NULL);
}

static void
grd_rdp_session_metrics_dispose (GObject *object)
{
  GrdRdpSessionMetrics *session_metrics = GRD_RDP_SESSION_METRICS (object);

  g_clear_pointer (&session_metrics->surface_metrics_table, g_hash_table_unref);

  G_OBJECT_CLASS (grd_rdp_session_metrics_parent_class)->dispose (object);
}

static void
grd_rdp_session_metrics_finalize (GObject *object)
{
  GrdRdpSessionMetrics *session_metrics = GRD_RDP_SESSION_METRICS (object);

  g_mutex_clear (&session_metrics->metrics_mutex);

  G_OBJECT_CLASS (grd_rdp_session_metrics_parent_class)->finalize (object);
}

static void
grd_rdp_session_metrics_init (GrdRdpSessionMetrics *session_metrics)
{
  session_metrics->pending_layout_change = TRUE;
  session_metrics->pending_output = TRUE;

  session_metrics->phase = GRD_RDP_PHASE_SESSION_INITIALIZATION;
  session_metrics->rd_session_init_us = g_get_monotonic_time ();

  session_metrics->surface_metrics_table = g_hash_table_new_full (NULL, NULL,
                                                                  NULL, g_free);

  g_mutex_init (&session_metrics->metrics_mutex);
}

static void
grd_rdp_session_metrics_class_init (GrdRdpSessionMetricsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_session_metrics_dispose;
  object_class->finalize = grd_rdp_session_metrics_finalize;
}
