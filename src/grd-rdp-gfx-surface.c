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

#include "grd-rdp-gfx-surface.h"

#include "grd-rdp-gfx-frame-log.h"
#include "grd-rdp-graphics-pipeline.h"
#include "grd-rdp-surface.h"
#include "grd-session-rdp.h"

#define ACTIVATE_THROTTLING_TH_DEFAULT 2
#define DEACTIVATE_THROTTLING_TH_DEFAULT 1

typedef enum _ThrottlingState
{
  THROTTLING_STATE_INACTIVE,
  THROTTLING_STATE_ACTIVE,
  THROTTLING_STATE_ACTIVE_LOWERING_LATENCY,
} ThrottlingState;

struct _GrdRdpGfxSurface
{
  GObject parent;

  GrdRdpGraphicsPipeline *graphics_pipeline;
  GrdSessionRdp *session_rdp;
  GrdRdpSurface *rdp_surface;
  gboolean created;

  uint16_t surface_id;
  uint32_t codec_context_id;
  uint32_t serial;

  GSource *pending_encode_source;

  GrdRdpGfxFrameLog *frame_log;

  int64_t nw_auto_last_rtt_us;

  ThrottlingState throttling_state;
  /* Throttling triggers on >= activate_throttling_th */
  uint32_t activate_throttling_th;
  /* Throttling triggers on <= deactivate_throttling_th */
  uint32_t deactivate_throttling_th;
};

G_DEFINE_TYPE (GrdRdpGfxSurface, grd_rdp_gfx_surface, G_TYPE_OBJECT);

uint16_t
grd_rdp_gfx_surface_get_surface_id (GrdRdpGfxSurface *gfx_surface)
{
  return gfx_surface->surface_id;
}

uint32_t
grd_rdp_gfx_surface_get_codec_context_id (GrdRdpGfxSurface *gfx_surface)
{
  return gfx_surface->codec_context_id;
}

uint32_t
grd_rdp_gfx_surface_get_serial (GrdRdpGfxSurface *gfx_surface)
{
  return gfx_surface->serial;
}

GrdRdpSurface *
grd_rdp_gfx_surface_get_rdp_surface (GrdRdpGfxSurface *gfx_surface)
{
  return gfx_surface->rdp_surface;
}

static uint32_t
get_activate_throttling_th_from_rtt (GrdRdpGfxSurface *gfx_surface,
                                     int64_t           rtt_us)
{
  GrdRdpSurface *rdp_surface = gfx_surface->rdp_surface;
  int64_t refresh_rate = rdp_surface->refresh_rate;
  uint32_t activate_throttling_th;
  uint32_t delayed_frames;

  delayed_frames = rtt_us * refresh_rate / G_USEC_PER_SEC;

  activate_throttling_th = MAX (2, MIN (delayed_frames + 2, refresh_rate));
  g_assert (activate_throttling_th > gfx_surface->deactivate_throttling_th);

  return activate_throttling_th;
}

void
grd_rdp_gfx_surface_unack_frame (GrdRdpGfxSurface *gfx_surface,
                                 uint32_t          frame_id,
                                 int64_t           enc_time_us)
{
  GrdRdpSurface *rdp_surface = gfx_surface->rdp_surface;
  GrdRdpGfxFrameLog *frame_log = gfx_surface->frame_log;
  uint32_t current_activate_throttling_th;
  uint32_t n_unacked_frames;
  uint32_t enc_rate = 0;
  uint32_t ack_rate = 0;

  grd_rdp_gfx_frame_log_track_frame (frame_log, frame_id, enc_time_us);

  n_unacked_frames = grd_rdp_gfx_frame_log_get_unacked_frames_count (frame_log);
  grd_rdp_gfx_frame_log_update_rates (frame_log, &enc_rate, &ack_rate);

  switch (gfx_surface->throttling_state)
    {
    case THROTTLING_STATE_INACTIVE:
      gfx_surface->activate_throttling_th = get_activate_throttling_th_from_rtt (
        gfx_surface, gfx_surface->nw_auto_last_rtt_us);

      if (n_unacked_frames >= gfx_surface->activate_throttling_th)
        {
          gfx_surface->throttling_state = THROTTLING_STATE_ACTIVE;
          rdp_surface->encoding_suspended = TRUE;
        }
      break;
    case THROTTLING_STATE_ACTIVE:
      current_activate_throttling_th = get_activate_throttling_th_from_rtt (
        gfx_surface, gfx_surface->nw_auto_last_rtt_us);

      if (current_activate_throttling_th < gfx_surface->activate_throttling_th)
        {
          gfx_surface->throttling_state = THROTTLING_STATE_ACTIVE_LOWERING_LATENCY;
          rdp_surface->encoding_suspended = TRUE;
        }
      else
        {
          gfx_surface->activate_throttling_th = current_activate_throttling_th;
          rdp_surface->encoding_suspended = enc_rate > ack_rate + 1;
        }
      break;
    case THROTTLING_STATE_ACTIVE_LOWERING_LATENCY:
      g_assert (rdp_surface->encoding_suspended);
      break;
    }
}

void
grd_rdp_gfx_surface_ack_frame (GrdRdpGfxSurface *gfx_surface,
                               uint32_t          frame_id,
                               int64_t           ack_time_us)
{
  GrdRdpSurface *rdp_surface = gfx_surface->rdp_surface;
  GrdRdpGfxFrameLog *frame_log = gfx_surface->frame_log;
  gboolean encoding_was_suspended;
  uint32_t current_activate_throttling_th;
  uint32_t n_unacked_frames;
  uint32_t enc_rate = 0;
  uint32_t ack_rate = 0;

  grd_rdp_gfx_frame_log_ack_tracked_frame (frame_log, frame_id, ack_time_us);

  encoding_was_suspended = rdp_surface->encoding_suspended;
  n_unacked_frames = grd_rdp_gfx_frame_log_get_unacked_frames_count (frame_log);
  grd_rdp_gfx_frame_log_update_rates (frame_log, &enc_rate, &ack_rate);

  switch (gfx_surface->throttling_state)
    {
    case THROTTLING_STATE_INACTIVE:
      break;
    case THROTTLING_STATE_ACTIVE:
      if (n_unacked_frames <= gfx_surface->deactivate_throttling_th)
        {
          gfx_surface->throttling_state = THROTTLING_STATE_INACTIVE;
          rdp_surface->encoding_suspended = FALSE;
          break;
        }

      current_activate_throttling_th = get_activate_throttling_th_from_rtt (
        gfx_surface, gfx_surface->nw_auto_last_rtt_us);
      if (current_activate_throttling_th < gfx_surface->activate_throttling_th)
        {
          gfx_surface->throttling_state = THROTTLING_STATE_ACTIVE_LOWERING_LATENCY;
          rdp_surface->encoding_suspended = TRUE;
        }
      else
        {
          gfx_surface->activate_throttling_th = current_activate_throttling_th;
          rdp_surface->encoding_suspended = enc_rate > ack_rate;
        }
      break;
    case THROTTLING_STATE_ACTIVE_LOWERING_LATENCY:
      current_activate_throttling_th = get_activate_throttling_th_from_rtt (
        gfx_surface, gfx_surface->nw_auto_last_rtt_us);

      if (n_unacked_frames < current_activate_throttling_th)
        {
          gfx_surface->throttling_state = THROTTLING_STATE_INACTIVE;
          rdp_surface->encoding_suspended = FALSE;
        }
      else if (n_unacked_frames == current_activate_throttling_th)
        {
          gfx_surface->throttling_state = THROTTLING_STATE_ACTIVE;
          rdp_surface->encoding_suspended = enc_rate > ack_rate;
        }
      else if (n_unacked_frames > current_activate_throttling_th)
        {
          g_assert (rdp_surface->encoding_suspended);
        }
      else
        {
          g_assert_not_reached ();
        }
      break;
    }

  if (encoding_was_suspended && !rdp_surface->encoding_suspended)
    g_source_set_ready_time (gfx_surface->pending_encode_source, 0);
}

static void
reevaluate_encoding_suspension_state (GrdRdpGfxSurface *gfx_surface)
{
  GrdRdpSurface *rdp_surface = gfx_surface->rdp_surface;
  GrdRdpGfxFrameLog *frame_log = gfx_surface->frame_log;
  uint32_t n_unacked_frames;
  uint32_t enc_rate = 0;
  uint32_t ack_rate = 0;

  n_unacked_frames = grd_rdp_gfx_frame_log_get_unacked_frames_count (frame_log);
  grd_rdp_gfx_frame_log_update_rates (frame_log, &enc_rate, &ack_rate);

  switch (gfx_surface->throttling_state)
    {
    case THROTTLING_STATE_INACTIVE:
      gfx_surface->activate_throttling_th = get_activate_throttling_th_from_rtt (
        gfx_surface, gfx_surface->nw_auto_last_rtt_us);

      if (n_unacked_frames >= gfx_surface->activate_throttling_th)
        {
          gfx_surface->throttling_state = THROTTLING_STATE_ACTIVE;
          rdp_surface->encoding_suspended = TRUE;
        }
      break;
    case THROTTLING_STATE_ACTIVE:
      g_assert (gfx_surface->activate_throttling_th >
                gfx_surface->deactivate_throttling_th);
      g_assert (n_unacked_frames > gfx_surface->deactivate_throttling_th);
      g_assert (rdp_surface->encoding_suspended);
      break;
    case THROTTLING_STATE_ACTIVE_LOWERING_LATENCY:
      /*
       * While the graphics pipeline rewrites the frame history, the RTT
       * detection mechanism cannot submit a new round trip time.
       */
      g_assert_not_reached ();
      break;
    }
}

void
grd_rdp_gfx_surface_unack_last_acked_frame (GrdRdpGfxSurface *gfx_surface,
                                            uint32_t          frame_id,
                                            int64_t           enc_ack_time_us)
{
  grd_rdp_gfx_frame_log_unack_last_acked_frame (gfx_surface->frame_log, frame_id,
                                                enc_ack_time_us);
  reevaluate_encoding_suspension_state (gfx_surface);
}

void
grd_rdp_gfx_surface_clear_all_unacked_frames (GrdRdpGfxSurface *gfx_surface)
{
  GrdRdpSurface *rdp_surface = gfx_surface->rdp_surface;
  gboolean encoding_was_suspended;

  grd_rdp_gfx_frame_log_clear (gfx_surface->frame_log);

  encoding_was_suspended = rdp_surface->encoding_suspended;

  gfx_surface->throttling_state = THROTTLING_STATE_INACTIVE;
  rdp_surface->encoding_suspended = FALSE;

  if (encoding_was_suspended)
    g_source_set_ready_time (gfx_surface->pending_encode_source, 0);
}

void
grd_rdp_gfx_surface_notify_new_round_trip_time (GrdRdpGfxSurface *gfx_surface,
                                                int64_t           round_trip_time_us)
{
  gfx_surface->nw_auto_last_rtt_us = round_trip_time_us;
}

static gboolean
maybe_encode_pending_frame (gpointer user_data)
{
  GrdRdpGfxSurface *gfx_surface = user_data;

  grd_session_rdp_maybe_encode_pending_frame (gfx_surface->session_rdp,
                                              gfx_surface->rdp_surface);

  return G_SOURCE_CONTINUE;
}

static gboolean
pending_encode_source_dispatch (GSource     *source,
                                GSourceFunc  callback,
                                gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs pending_encode_source_funcs =
{
  .dispatch = pending_encode_source_dispatch,
};

GrdRdpGfxSurface *
grd_rdp_gfx_surface_new (GrdRdpGraphicsPipeline *graphics_pipeline,
                         GrdSessionRdp          *session_rdp,
                         GMainContext           *pipeline_context,
                         GrdRdpSurface          *rdp_surface,
                         uint16_t                surface_id,
                         uint32_t                serial)
{
  GrdRdpGfxSurface *gfx_surface;

  gfx_surface = g_object_new (GRD_TYPE_RDP_GFX_SURFACE, NULL);
  gfx_surface->graphics_pipeline = graphics_pipeline;
  gfx_surface->session_rdp = session_rdp;
  gfx_surface->rdp_surface = rdp_surface;
  gfx_surface->surface_id = surface_id;
  /*
   * Use the same id for the codec context as for the surface
   * (only relevant for RDPGFX_WIRE_TO_SURFACE_PDU_2 PDUs)
   */
  gfx_surface->codec_context_id = surface_id;

  grd_rdp_graphics_pipeline_create_surface (graphics_pipeline, gfx_surface);
  gfx_surface->created = TRUE;

  gfx_surface->pending_encode_source = g_source_new (&pending_encode_source_funcs,
                                                     sizeof (GSource));
  g_source_set_callback (gfx_surface->pending_encode_source,
                         maybe_encode_pending_frame, gfx_surface, NULL);
  g_source_set_ready_time (gfx_surface->pending_encode_source, -1);
  g_source_attach (gfx_surface->pending_encode_source, pipeline_context);

  return gfx_surface;
}

static void
grd_rdp_gfx_surface_dispose (GObject *object)
{
  GrdRdpGfxSurface *gfx_surface = GRD_RDP_GFX_SURFACE (object);

  if (gfx_surface->pending_encode_source)
    {
      g_source_destroy (gfx_surface->pending_encode_source);
      g_clear_pointer (&gfx_surface->pending_encode_source, g_source_unref);
    }

  if (gfx_surface->created)
    {
      grd_rdp_graphics_pipeline_delete_surface (gfx_surface->graphics_pipeline,
                                                gfx_surface);
      gfx_surface->created = FALSE;
    }

  g_clear_object (&gfx_surface->frame_log);

  G_OBJECT_CLASS (grd_rdp_gfx_surface_parent_class)->dispose (object);
}

static void
grd_rdp_gfx_surface_init (GrdRdpGfxSurface *gfx_surface)
{
  gfx_surface->throttling_state = THROTTLING_STATE_INACTIVE;
  gfx_surface->activate_throttling_th = ACTIVATE_THROTTLING_TH_DEFAULT;
  gfx_surface->deactivate_throttling_th = DEACTIVATE_THROTTLING_TH_DEFAULT;

  g_assert (gfx_surface->activate_throttling_th >
            gfx_surface->deactivate_throttling_th);

  gfx_surface->frame_log = grd_rdp_gfx_frame_log_new ();
}

static void
grd_rdp_gfx_surface_class_init (GrdRdpGfxSurfaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_gfx_surface_dispose;
}
