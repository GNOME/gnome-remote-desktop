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

#include "grd-rdp-frame.h"

#include "grd-encode-context.h"
#include "grd-rdp-render-context.h"
#include "grd-rdp-renderer.h"

struct _GrdRdpFrame
{
  GrdRdpRenderer *renderer;
  GrdRdpRenderContext *render_context;

  GrdRdpFrameCallback frame_picked_up;
  GrdRdpFrameCallback view_finalized;
  GrdRdpFrameCallback frame_submitted;
  GrdRdpFrameCallback frame_finalized;
  gpointer callback_user_data;
  GDestroyNotify user_data_destroy;

  gboolean pending_view_finalization;

  GrdEncodeContext *encode_context;

  GList *acquired_image_views;
  GQueue *unused_image_views;

  GrdRdpBuffer *src_buffer_new;
  GrdRdpBuffer *src_buffer_old;

  GrdRdpFrameViewType view_type;

  cairo_region_t *damage_region;
  GList *bitstreams;
};

GrdRdpRenderer *
grd_rdp_frame_get_renderer (GrdRdpFrame *rdp_frame)
{
  return rdp_frame->renderer;
}

GrdRdpRenderContext *
grd_rdp_frame_get_render_context (GrdRdpFrame *rdp_frame)
{
  return rdp_frame->render_context;
}

GrdEncodeContext *
grd_rdp_frame_get_encode_context (GrdRdpFrame *rdp_frame)
{
  return rdp_frame->encode_context;
}

GList *
grd_rdp_frame_get_image_views (GrdRdpFrame *rdp_frame)
{
  return rdp_frame->acquired_image_views;
}

GrdRdpBuffer *
grd_rdp_frame_get_source_buffer (GrdRdpFrame *rdp_frame)
{
  return rdp_frame->src_buffer_new;
}

GrdRdpBuffer *
grd_rdp_frame_get_last_source_buffer (GrdRdpFrame *rdp_frame)
{
  return rdp_frame->src_buffer_old;
}

GrdRdpFrameViewType
grd_rdp_frame_get_avc_view_type (GrdRdpFrame *rdp_frame)
{
  return rdp_frame->view_type;
}

cairo_region_t *
grd_rdp_frame_get_damage_region (GrdRdpFrame *rdp_frame)
{
  return rdp_frame->damage_region;
}

GList *
grd_rdp_frame_get_bitstreams (GrdRdpFrame *rdp_frame)
{
  return rdp_frame->bitstreams;
}

gboolean
grd_rdp_frame_has_valid_view (GrdRdpFrame *rdp_frame)
{
  return !!rdp_frame->damage_region;
}

gboolean
grd_rdp_frame_is_surface_damaged (GrdRdpFrame *rdp_frame)
{
  return cairo_region_num_rectangles (rdp_frame->damage_region) > 0;
}

void
grd_rdp_frame_set_renderer (GrdRdpFrame    *rdp_frame,
                            GrdRdpRenderer *renderer)
{
  rdp_frame->renderer = renderer;
}

void
grd_rdp_frame_set_avc_view_type (GrdRdpFrame         *rdp_frame,
                                 GrdRdpFrameViewType  view_type)
{
  g_assert (view_type != GRD_RDP_FRAME_VIEW_TYPE_STEREO);
  g_assert (view_type == GRD_RDP_FRAME_VIEW_TYPE_MAIN ||
            view_type == GRD_RDP_FRAME_VIEW_TYPE_AUX);

  switch (rdp_frame->view_type)
    {
    case GRD_RDP_FRAME_VIEW_TYPE_STEREO:
      g_assert (g_queue_get_length (rdp_frame->unused_image_views) == 2);
      g_queue_pop_tail (rdp_frame->unused_image_views);
      break;
    case GRD_RDP_FRAME_VIEW_TYPE_MAIN:
    case GRD_RDP_FRAME_VIEW_TYPE_AUX:
      g_assert (g_queue_get_length (rdp_frame->unused_image_views) == 1);
      break;
    }

  rdp_frame->view_type = view_type;
}

static void
finalize_view (GrdRdpFrame *rdp_frame)
{
  g_assert (rdp_frame->pending_view_finalization);

  rdp_frame->view_finalized (rdp_frame, rdp_frame->callback_user_data);
  rdp_frame->pending_view_finalization = FALSE;
}

void
grd_rdp_frame_set_damage_region (GrdRdpFrame    *rdp_frame,
                                 cairo_region_t *damage_region)
{
  g_assert (!rdp_frame->damage_region);

  rdp_frame->damage_region = damage_region;
  grd_encode_context_set_damage_region (rdp_frame->encode_context,
                                        damage_region);

  finalize_view (rdp_frame);
}

void
grd_rdp_frame_set_bitstreams (GrdRdpFrame *rdp_frame,
                              GList       *bitstreams)
{
  rdp_frame->bitstreams = bitstreams;
}

void
grd_rdp_frame_notify_picked_up (GrdRdpFrame *rdp_frame)
{
  rdp_frame->frame_picked_up (rdp_frame, rdp_frame->callback_user_data);
}

void
grd_rdp_frame_notify_frame_submission (GrdRdpFrame *rdp_frame)
{
  rdp_frame->frame_submitted (rdp_frame, rdp_frame->callback_user_data);
}

GrdImageView *
grd_rdp_frame_pop_image_view (GrdRdpFrame *rdp_frame)
{
  return g_queue_pop_head (rdp_frame->unused_image_views);
}

static void
set_view_type (GrdRdpFrame *rdp_frame,
               gboolean     frame_upgrade)
{
  GrdRdpRenderContext *render_context = rdp_frame->render_context;
  GrdRdpCodec codec = grd_rdp_render_context_get_codec (render_context);

  switch (codec)
    {
    case GRD_RDP_CODEC_CAPROGRESSIVE:
    case GRD_RDP_CODEC_AVC420:
      rdp_frame->view_type = GRD_RDP_FRAME_VIEW_TYPE_MAIN;
      break;
    case GRD_RDP_CODEC_AVC444v2:
      rdp_frame->view_type =
        frame_upgrade ? GRD_RDP_FRAME_VIEW_TYPE_AUX
                      : GRD_RDP_FRAME_VIEW_TYPE_STEREO;
      break;
    }
}

static uint32_t
get_n_required_image_views (GrdRdpFrame *rdp_frame)
{
  GrdRdpRenderContext *render_context = rdp_frame->render_context;
  GrdRdpCodec codec = grd_rdp_render_context_get_codec (render_context);

  switch (codec)
    {
    case GRD_RDP_CODEC_CAPROGRESSIVE:
      return 1;
    case GRD_RDP_CODEC_AVC420:
    case GRD_RDP_CODEC_AVC444v2:
      return 2;
    }

  g_assert_not_reached ();
}

static uint32_t
get_n_image_views_to_be_encoded (GrdRdpFrame *rdp_frame)
{
  switch (grd_rdp_frame_get_avc_view_type (rdp_frame))
    {
    case GRD_RDP_FRAME_VIEW_TYPE_STEREO:
      return 2;
    case GRD_RDP_FRAME_VIEW_TYPE_MAIN:
    case GRD_RDP_FRAME_VIEW_TYPE_AUX:
      return 1;
    }

  g_assert_not_reached ();
}

static void
acquire_image_views (GrdRdpFrame *rdp_frame)
{
  uint32_t n_image_views = get_n_required_image_views (rdp_frame);
  uint32_t n_image_views_to_be_encoded =
    get_n_image_views_to_be_encoded (rdp_frame);
  uint32_t i;

  g_assert (n_image_views_to_be_encoded <= n_image_views);

  for (i = 0; i < n_image_views; ++i)
    {
      GrdRdpRenderContext *render_context = rdp_frame->render_context;
      GrdImageView *image_view;

      image_view = grd_rdp_render_context_acquire_image_view (render_context);
      rdp_frame->acquired_image_views =
        g_list_append (rdp_frame->acquired_image_views, image_view);

      if (i < n_image_views_to_be_encoded)
        g_queue_push_tail (rdp_frame->unused_image_views, image_view);
    }
}

static void
prepare_new_frame (GrdRdpFrame *rdp_frame)
{
  rdp_frame->pending_view_finalization = TRUE;

  set_view_type (rdp_frame, FALSE);
  acquire_image_views (rdp_frame);
}

static void
prepare_frame_upgrade (GrdRdpFrame *rdp_frame)
{
  GrdRdpRenderContext *render_context = rdp_frame->render_context;
  cairo_region_t *damage_region = NULL;
  GrdImageView *image_view = NULL;

  set_view_type (rdp_frame, TRUE);

  grd_rdp_render_context_fetch_progressive_render_state (render_context,
                                                         &image_view,
                                                         &damage_region);
  g_assert (image_view);
  g_assert (damage_region);

  rdp_frame->acquired_image_views =
    g_list_append (rdp_frame->acquired_image_views, image_view);
  g_queue_push_tail (rdp_frame->unused_image_views, image_view);

  rdp_frame->damage_region = damage_region;
}

GrdRdpFrame *
grd_rdp_frame_new (GrdRdpRenderContext *render_context,
                   GrdRdpBuffer        *src_buffer_new,
                   GrdRdpBuffer        *src_buffer_old,
                   GrdRdpFrameCallback  frame_picked_up,
                   GrdRdpFrameCallback  view_finalized,
                   GrdRdpFrameCallback  frame_submitted,
                   GrdRdpFrameCallback  frame_finalized,
                   gpointer             callback_user_data,
                   GDestroyNotify       user_data_destroy)
{
  GrdRdpFrame *rdp_frame;

  rdp_frame = g_new0 (GrdRdpFrame, 1);
  rdp_frame->render_context = render_context;
  rdp_frame->src_buffer_new = src_buffer_new;
  rdp_frame->src_buffer_old = src_buffer_old;

  rdp_frame->frame_picked_up = frame_picked_up;
  rdp_frame->view_finalized = view_finalized;
  rdp_frame->frame_submitted = frame_submitted;
  rdp_frame->frame_finalized = frame_finalized;
  rdp_frame->callback_user_data = callback_user_data;
  rdp_frame->user_data_destroy = user_data_destroy;

  rdp_frame->encode_context = grd_encode_context_new ();
  rdp_frame->unused_image_views = g_queue_new ();

  if (src_buffer_new)
    prepare_new_frame (rdp_frame);
  else
    prepare_frame_upgrade (rdp_frame);

  return rdp_frame;
}

static void
release_image_views (GrdRdpFrame *rdp_frame)
{
  GList *l;

  for (l = rdp_frame->acquired_image_views; l; l = l->next)
    {
      GrdImageView *image_view = l->data;

      grd_rdp_render_context_release_image_view (rdp_frame->render_context,
                                                 image_view);
    }
  g_clear_pointer (&rdp_frame->acquired_image_views, g_list_free);
}

void
grd_rdp_frame_free (GrdRdpFrame *rdp_frame)
{
  g_assert (!rdp_frame->bitstreams);
  g_assert (rdp_frame->renderer);

  if (rdp_frame->pending_view_finalization)
    finalize_view (rdp_frame);

  rdp_frame->frame_finalized (rdp_frame, rdp_frame->callback_user_data);

  g_clear_pointer (&rdp_frame->encode_context, grd_encode_context_free);
  g_clear_pointer (&rdp_frame->damage_region, cairo_region_destroy);
  g_clear_pointer (&rdp_frame->callback_user_data,
                   rdp_frame->user_data_destroy);

  g_clear_pointer (&rdp_frame->unused_image_views, g_queue_free);
  release_image_views (rdp_frame);
  grd_rdp_renderer_release_render_context (rdp_frame->renderer,
                                           rdp_frame->render_context);

  g_free (rdp_frame);
}
