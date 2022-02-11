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
#include "grd-session-rdp.h"

#define ACTIVATE_THROTTLING_TH_DEFAULT 2
#define DEACTIVATE_THROTTLING_TH_DEFAULT 1

typedef enum _ThrottlingState
{
  THROTTLING_STATE_INACTIVE,
  THROTTLING_STATE_ACTIVE,
  THROTTLING_STATE_ACTIVE_LOWERING_LATENCY,
} ThrottlingState;

struct _GrdRdpGfxFrameController
{
  GObject parent;

  GrdSessionRdp *session_rdp;
  GrdRdpSurface *rdp_surface;

  GSource *pending_encode_source;

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

static uint32_t
get_activate_throttling_th_from_rtt (GrdRdpGfxFrameController *frame_controller,
                                     int64_t                   rtt_us)
{
  GrdRdpSurface *rdp_surface = frame_controller->rdp_surface;
  int64_t refresh_rate = rdp_surface->refresh_rate;
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
          rdp_surface->encoding_suspended = TRUE;
        }
      break;
    case THROTTLING_STATE_ACTIVE:
      current_activate_throttling_th =
        get_activate_throttling_th_from_rtt (frame_controller,
                                             frame_controller->nw_auto_last_rtt_us);

      if (current_activate_throttling_th < frame_controller->activate_throttling_th)
        {
          frame_controller->throttling_state = THROTTLING_STATE_ACTIVE_LOWERING_LATENCY;
          rdp_surface->encoding_suspended = TRUE;
        }
      else
        {
          frame_controller->activate_throttling_th = current_activate_throttling_th;
          rdp_surface->encoding_suspended = enc_rate > ack_rate + 1;
        }
      break;
    case THROTTLING_STATE_ACTIVE_LOWERING_LATENCY:
      g_assert (rdp_surface->encoding_suspended);
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
  gboolean encoding_was_suspended;
  uint32_t current_activate_throttling_th;
  uint32_t n_unacked_frames;
  uint32_t enc_rate = 0;
  uint32_t ack_rate = 0;

  grd_rdp_gfx_frame_log_ack_tracked_frame (frame_log, frame_id, ack_time_us);

  encoding_was_suspended = rdp_surface->encoding_suspended;
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
          rdp_surface->encoding_suspended = FALSE;
          break;
        }

      current_activate_throttling_th =
        get_activate_throttling_th_from_rtt (frame_controller,
                                             frame_controller->nw_auto_last_rtt_us);
      if (current_activate_throttling_th < frame_controller->activate_throttling_th)
        {
          frame_controller->throttling_state = THROTTLING_STATE_ACTIVE_LOWERING_LATENCY;
          rdp_surface->encoding_suspended = TRUE;
        }
      else
        {
          frame_controller->activate_throttling_th = current_activate_throttling_th;
          rdp_surface->encoding_suspended = enc_rate > ack_rate;
        }
      break;
    case THROTTLING_STATE_ACTIVE_LOWERING_LATENCY:
      current_activate_throttling_th =
        get_activate_throttling_th_from_rtt (frame_controller,
                                             frame_controller->nw_auto_last_rtt_us);

      if (n_unacked_frames < current_activate_throttling_th)
        {
          frame_controller->throttling_state = THROTTLING_STATE_INACTIVE;
          rdp_surface->encoding_suspended = FALSE;
        }
      else if (n_unacked_frames == current_activate_throttling_th)
        {
          frame_controller->throttling_state = THROTTLING_STATE_ACTIVE;
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
    g_source_set_ready_time (frame_controller->pending_encode_source, 0);
}

static void
reevaluate_encoding_suspension_state (GrdRdpGfxFrameController *frame_controller)
{
  GrdRdpSurface *rdp_surface = frame_controller->rdp_surface;
  GrdRdpGfxFrameLog *frame_log = frame_controller->frame_log;
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
          rdp_surface->encoding_suspended = TRUE;
        }
      break;
    case THROTTLING_STATE_ACTIVE:
      g_assert (frame_controller->activate_throttling_th >
                frame_controller->deactivate_throttling_th);
      g_assert (n_unacked_frames > frame_controller->deactivate_throttling_th);
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
  gboolean encoding_was_suspended;

  grd_rdp_gfx_frame_log_clear (frame_controller->frame_log);

  encoding_was_suspended = rdp_surface->encoding_suspended;

  frame_controller->throttling_state = THROTTLING_STATE_INACTIVE;
  rdp_surface->encoding_suspended = FALSE;

  if (encoding_was_suspended)
    g_source_set_ready_time (frame_controller->pending_encode_source, 0);
}

void
grd_rdp_gfx_frame_controller_notify_new_round_trip_time (GrdRdpGfxFrameController *frame_controller,
                                                         int64_t                   round_trip_time_us)
{
  frame_controller->nw_auto_last_rtt_us = round_trip_time_us;
}

static gboolean
maybe_encode_pending_frame (gpointer user_data)
{
  GrdRdpGfxFrameController *frame_controller = user_data;

  grd_session_rdp_maybe_encode_pending_frame (frame_controller->session_rdp,
                                              frame_controller->rdp_surface);

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

GrdRdpGfxFrameController *
grd_rdp_gfx_frame_controller_new (GrdSessionRdp *session_rdp,
                                  GMainContext  *pipeline_context,
                                  GrdRdpSurface *rdp_surface)
{
  GrdRdpGfxFrameController *frame_controller;

  frame_controller = g_object_new (GRD_TYPE_RDP_GFX_FRAME_CONTROLLER, NULL);
  frame_controller->session_rdp = session_rdp;
  frame_controller->rdp_surface = rdp_surface;

  frame_controller->pending_encode_source =
    g_source_new (&pending_encode_source_funcs, sizeof (GSource));
  g_source_set_callback (frame_controller->pending_encode_source,
                         maybe_encode_pending_frame, frame_controller, NULL);
  g_source_set_ready_time (frame_controller->pending_encode_source, -1);
  g_source_attach (frame_controller->pending_encode_source, pipeline_context);

  return frame_controller;
}

static void
grd_rdp_gfx_frame_controller_dispose (GObject *object)
{
  GrdRdpGfxFrameController *frame_controller = GRD_RDP_GFX_FRAME_CONTROLLER (object);

  if (frame_controller->pending_encode_source)
    {
      g_source_destroy (frame_controller->pending_encode_source);
      g_clear_pointer (&frame_controller->pending_encode_source, g_source_unref);
    }

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
