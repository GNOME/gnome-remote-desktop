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

#include "grd-rdp-gfx-frame-controller.h"

#include "grd-rdp-gfx-frame-log.h"
#include "grd-rdp-surface.h"
#include "grd-rdp-surface-renderer.h"

#define ACTIVATE_THROTTLING_TH_DEFAULT 2
#define DEACTIVATE_THROTTLING_TH_DEFAULT 1

#define UNLIMITED_FRAME_SLOTS (UINT32_MAX)

typedef enum _ThrottlingState
{
  THROTTLING_STATE_INACTIVE,
  THROTTLING_STATE_ACTIVE,
  THROTTLING_STATE_ACTIVE_LOWERING_LATENCY,
} ThrottlingState;

struct _GrdRdpGfxFrameController
{
  GObject parent;

  GrdRdpSurface *rdp_surface;

  GrdRdpGfxFrameLog *frame_log;

  int64_t nw_auto_last_rtt_us;

  ThrottlingState throttling_state;
  /* Throttling triggers on >= activate_throttling_th */
  uint32_t activate_throttling_th;
  /* Throttling triggers on <= deactivate_throttling_th */
  uint32_t deactivate_throttling_th;
};

G_DEFINE_TYPE (GrdRdpGfxFrameController,
               grd_rdp_gfx_frame_controller,
               G_TYPE_OBJECT)

static gboolean
is_rendering_suspended (GrdRdpGfxFrameController *frame_controller)
{
  GrdRdpSurface *rdp_surface = frame_controller->rdp_surface;
  GrdRdpSurfaceRenderer *surface_renderer =
    grd_rdp_surface_get_surface_renderer (rdp_surface);
  uint32_t total_frame_slots =
    grd_rdp_surface_renderer_get_total_frame_slots (surface_renderer);

  return total_frame_slots == 0;
}

static uint32_t
get_activate_throttling_th_from_rtt (GrdRdpGfxFrameController *frame_controller,
                                     int64_t                   rtt_us)
{
  GrdRdpSurface *rdp_surface = frame_controller->rdp_surface;
  GrdRdpSurfaceRenderer *surface_renderer =
    grd_rdp_surface_get_surface_renderer (rdp_surface);
  int64_t refresh_rate =
    grd_rdp_surface_renderer_get_refresh_rate (surface_renderer);
  uint32_t activate_throttling_th;
  uint32_t delayed_frames;

  delayed_frames = rtt_us * refresh_rate / G_USEC_PER_SEC;

  activate_throttling_th = MAX (2, MIN (delayed_frames + 2, refresh_rate));
  g_assert (activate_throttling_th > frame_controller->deactivate_throttling_th);

  return activate_throttling_th;
}

void
grd_rdp_gfx_frame_controller_unack_frame (GrdRdpGfxFrameController *frame_controller,
                                          uint32_t                  frame_id,
                                          int64_t                   enc_time_us)
{
  GrdRdpSurface *rdp_surface = frame_controller->rdp_surface;
  GrdRdpGfxFrameLog *frame_log = frame_controller->frame_log;
  GrdRdpSurfaceRenderer *surface_renderer =
    grd_rdp_surface_get_surface_renderer (rdp_surface);
  uint32_t current_activate_throttling_th;
  uint32_t n_unacked_frames;
  uint32_t enc_rate = 0;
  uint32_t ack_rate = 0;

  grd_rdp_gfx_frame_log_track_frame (frame_log, frame_id, enc_time_us);

  n_unacked_frames = grd_rdp_gfx_frame_log_get_unacked_frames_count (frame_log);
  grd_rdp_gfx_frame_log_update_rates (frame_log, &enc_rate, &ack_rate);

  switch (frame_controller->throttling_state)
    {
    case THROTTLING_STATE_INACTIVE:
      frame_controller->activate_throttling_th =
        get_activate_throttling_th_from_rtt (frame_controller,
                                             frame_controller->nw_auto_last_rtt_us);

      if (n_unacked_frames >= frame_controller->activate_throttling_th)
        {
          frame_controller->throttling_state = THROTTLING_STATE_ACTIVE;
          grd_rdp_surface_renderer_update_total_frame_slots (surface_renderer, 0);
        }
      break;
    case THROTTLING_STATE_ACTIVE:
      current_activate_throttling_th =
        get_activate_throttling_th_from_rtt (frame_controller,
                                             frame_controller->nw_auto_last_rtt_us);

      if (current_activate_throttling_th < frame_controller->activate_throttling_th)
        {
          frame_controller->throttling_state = THROTTLING_STATE_ACTIVE_LOWERING_LATENCY;
          grd_rdp_surface_renderer_update_total_frame_slots (surface_renderer, 0);
        }
      else
        {
          uint32_t total_frame_slots;

          frame_controller->activate_throttling_th = current_activate_throttling_th;

          total_frame_slots = enc_rate > ack_rate + 1 ? 0 : ack_rate + 2 - enc_rate;
          grd_rdp_surface_renderer_update_total_frame_slots (surface_renderer,
                                                             total_frame_slots);
        }
      break;
    case THROTTLING_STATE_ACTIVE_LOWERING_LATENCY:
      g_assert (is_rendering_suspended (frame_controller));
      break;
    }
}

void
grd_rdp_gfx_frame_controller_ack_frame (GrdRdpGfxFrameController *frame_controller,
                                        uint32_t                  frame_id,
                                        int64_t                   ack_time_us)
{
  GrdRdpSurface *rdp_surface = frame_controller->rdp_surface;
  GrdRdpGfxFrameLog *frame_log = frame_controller->frame_log;
  GrdRdpSurfaceRenderer *surface_renderer =
    grd_rdp_surface_get_surface_renderer (rdp_surface);
  uint32_t current_activate_throttling_th;
  uint32_t n_unacked_frames;
  uint32_t enc_rate = 0;
  uint32_t ack_rate = 0;

  grd_rdp_gfx_frame_log_ack_tracked_frame (frame_log, frame_id, ack_time_us);

  n_unacked_frames = grd_rdp_gfx_frame_log_get_unacked_frames_count (frame_log);
  grd_rdp_gfx_frame_log_update_rates (frame_log, &enc_rate, &ack_rate);

  switch (frame_controller->throttling_state)
    {
    case THROTTLING_STATE_INACTIVE:
      break;
    case THROTTLING_STATE_ACTIVE:
      if (n_unacked_frames <= frame_controller->deactivate_throttling_th)
        {
          frame_controller->throttling_state = THROTTLING_STATE_INACTIVE;
          grd_rdp_surface_renderer_update_total_frame_slots (surface_renderer,
                                                             UNLIMITED_FRAME_SLOTS);
          break;
        }

      current_activate_throttling_th =
        get_activate_throttling_th_from_rtt (frame_controller,
                                             frame_controller->nw_auto_last_rtt_us);
      if (current_activate_throttling_th < frame_controller->activate_throttling_th)
        {
          frame_controller->throttling_state = THROTTLING_STATE_ACTIVE_LOWERING_LATENCY;
          grd_rdp_surface_renderer_update_total_frame_slots (surface_renderer, 0);
        }
      else
        {
          uint32_t total_frame_slots;

          frame_controller->activate_throttling_th = current_activate_throttling_th;

          total_frame_slots = enc_rate > ack_rate ? 0 : ack_rate + 1 - enc_rate;
          grd_rdp_surface_renderer_update_total_frame_slots (surface_renderer,
                                                             total_frame_slots);
        }
      break;
    case THROTTLING_STATE_ACTIVE_LOWERING_LATENCY:
      current_activate_throttling_th =
        get_activate_throttling_th_from_rtt (frame_controller,
                                             frame_controller->nw_auto_last_rtt_us);

      if (n_unacked_frames < current_activate_throttling_th)
        {
          frame_controller->throttling_state = THROTTLING_STATE_INACTIVE;
          grd_rdp_surface_renderer_update_total_frame_slots (surface_renderer,
                                                             UNLIMITED_FRAME_SLOTS);
        }
      else if (n_unacked_frames == current_activate_throttling_th)
        {
          uint32_t total_frame_slots;

          frame_controller->throttling_state = THROTTLING_STATE_ACTIVE;

          total_frame_slots = enc_rate > ack_rate ? 0 : ack_rate + 1 - enc_rate;
          grd_rdp_surface_renderer_update_total_frame_slots (surface_renderer,
                                                             total_frame_slots);
        }
      else if (n_unacked_frames > current_activate_throttling_th)
        {
          g_assert (is_rendering_suspended (frame_controller));
        }
      else
        {
          g_assert_not_reached ();
        }
      break;
    }
}

static void
reevaluate_encoding_suspension_state (GrdRdpGfxFrameController *frame_controller)
{
  GrdRdpSurface *rdp_surface = frame_controller->rdp_surface;
  GrdRdpGfxFrameLog *frame_log = frame_controller->frame_log;
  GrdRdpSurfaceRenderer *surface_renderer =
    grd_rdp_surface_get_surface_renderer (rdp_surface);
  uint32_t n_unacked_frames;
  uint32_t enc_rate = 0;
  uint32_t ack_rate = 0;

  n_unacked_frames = grd_rdp_gfx_frame_log_get_unacked_frames_count (frame_log);
  grd_rdp_gfx_frame_log_update_rates (frame_log, &enc_rate, &ack_rate);

  switch (frame_controller->throttling_state)
    {
    case THROTTLING_STATE_INACTIVE:
      frame_controller->activate_throttling_th =
        get_activate_throttling_th_from_rtt (frame_controller,
                                             frame_controller->nw_auto_last_rtt_us);

      if (n_unacked_frames >= frame_controller->activate_throttling_th)
        {
          frame_controller->throttling_state = THROTTLING_STATE_ACTIVE;
          grd_rdp_surface_renderer_update_total_frame_slots (surface_renderer, 0);
        }
      break;
    case THROTTLING_STATE_ACTIVE:
      g_assert (frame_controller->activate_throttling_th >
                frame_controller->deactivate_throttling_th);
      g_assert (n_unacked_frames > frame_controller->deactivate_throttling_th);
      g_assert (is_rendering_suspended (frame_controller));
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
grd_rdp_gfx_frame_controller_unack_last_acked_frame (GrdRdpGfxFrameController *frame_controller,
                                                     uint32_t                  frame_id,
                                                     int64_t                   enc_ack_time_us)
{
  grd_rdp_gfx_frame_log_unack_last_acked_frame (frame_controller->frame_log,
                                                frame_id, enc_ack_time_us);
  reevaluate_encoding_suspension_state (frame_controller);
}

void
grd_rdp_gfx_frame_controller_clear_all_unacked_frames (GrdRdpGfxFrameController *frame_controller)
{
  GrdRdpSurface *rdp_surface = frame_controller->rdp_surface;
  GrdRdpSurfaceRenderer *surface_renderer =
    grd_rdp_surface_get_surface_renderer (rdp_surface);

  grd_rdp_gfx_frame_log_clear (frame_controller->frame_log);

  frame_controller->throttling_state = THROTTLING_STATE_INACTIVE;
  grd_rdp_surface_renderer_update_total_frame_slots (surface_renderer,
                                                     UNLIMITED_FRAME_SLOTS);
}

void
grd_rdp_gfx_frame_controller_notify_new_round_trip_time (GrdRdpGfxFrameController *frame_controller,
                                                         int64_t                   round_trip_time_us)
{
  frame_controller->nw_auto_last_rtt_us = round_trip_time_us;
}

GrdRdpGfxFrameController *
grd_rdp_gfx_frame_controller_new (GrdRdpSurface *rdp_surface)
{
  GrdRdpGfxFrameController *frame_controller;

  frame_controller = g_object_new (GRD_TYPE_RDP_GFX_FRAME_CONTROLLER, NULL);
  frame_controller->rdp_surface = rdp_surface;

  return frame_controller;
}

static void
grd_rdp_gfx_frame_controller_dispose (GObject *object)
{
  GrdRdpGfxFrameController *frame_controller = GRD_RDP_GFX_FRAME_CONTROLLER (object);

  g_clear_object (&frame_controller->frame_log);

  G_OBJECT_CLASS (grd_rdp_gfx_frame_controller_parent_class)->dispose (object);
}

static void
grd_rdp_gfx_frame_controller_init (GrdRdpGfxFrameController *frame_controller)
{
  frame_controller->throttling_state = THROTTLING_STATE_INACTIVE;
  frame_controller->activate_throttling_th = ACTIVATE_THROTTLING_TH_DEFAULT;
  frame_controller->deactivate_throttling_th = DEACTIVATE_THROTTLING_TH_DEFAULT;

  g_assert (frame_controller->activate_throttling_th >
            frame_controller->deactivate_throttling_th);

  frame_controller->frame_log = grd_rdp_gfx_frame_log_new ();
}

static void
grd_rdp_gfx_frame_controller_class_init (GrdRdpGfxFrameControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_gfx_frame_controller_dispose;
}
