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

#include "grd-rdp-render-context.h"

#include <drm_fourcc.h>

#include "grd-encode-session.h"
#include "grd-hwaccel-vaapi.h"
#include "grd-rdp-buffer-info.h"
#include "grd-rdp-damage-detector.h"
#include "grd-rdp-frame.h"
#include "grd-rdp-gfx-surface.h"
#include "grd-rdp-graphics-pipeline.h"
#include "grd-rdp-render-state.h"
#include "grd-rdp-surface.h"
#include "grd-rdp-surface-renderer.h"
#include "grd-rdp-view-creator-avc.h"
#include "grd-utils.h"

#define STATE_TILE_WIDTH 64
#define STATE_TILE_HEIGHT 64

struct _GrdRdpRenderContext
{
  GObject parent;

  GrdVkDevice *vk_device;
  GrdHwAccelVaapi *hwaccel_vaapi;

  GrdRdpCodec codec;

  GrdRdpGfxSurface *gfx_surface;
  GrdRdpViewCreator *view_creator;
  GrdEncodeSession *encode_session;

  GHashTable *image_views;
  GHashTable *acquired_image_views;

  uint32_t *chroma_state_buffer;
  uint32_t state_buffer_length;
};

G_DEFINE_TYPE (GrdRdpRenderContext, grd_rdp_render_context, G_TYPE_OBJECT)

GrdRdpCodec
grd_rdp_render_context_get_codec (GrdRdpRenderContext *render_context)
{
  return render_context->codec;
}

GrdRdpGfxSurface *
grd_rdp_render_context_get_gfx_surface (GrdRdpRenderContext *render_context)
{
  return render_context->gfx_surface;
}

GrdRdpViewCreator *
grd_rdp_render_context_get_view_creator (GrdRdpRenderContext *render_context)
{
  return render_context->view_creator;
}

GrdEncodeSession *
grd_rdp_render_context_get_encode_session (GrdRdpRenderContext *render_context)
{
  return render_context->encode_session;
}

GrdImageView *
grd_rdp_render_context_acquire_image_view (GrdRdpRenderContext *render_context)
{
  GrdImageView *image_view = NULL;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, render_context->image_views);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &image_view))
    {
      if (g_hash_table_contains (render_context->acquired_image_views,
                                 image_view))
        continue;

      g_hash_table_add (render_context->acquired_image_views, image_view);
      return image_view;
    }

  g_assert_not_reached ();
  return NULL;
}

void
grd_rdp_render_context_release_image_view (GrdRdpRenderContext *render_context,
                                           GrdImageView        *image_view)
{
  if (!g_hash_table_remove (render_context->acquired_image_views, image_view))
    g_assert_not_reached ();
}

static gboolean
is_auxiliary_view_needed (GrdRdpRenderState *render_state)
{
  uint32_t *chroma_state_buffer =
    grd_rdp_render_state_get_chroma_state_buffer (render_state);
  uint32_t state_buffer_length =
    grd_rdp_render_state_get_state_buffer_length (render_state);
  uint32_t i;

  for (i = 0; i < state_buffer_length; ++i)
    {
      if (chroma_state_buffer[i] != 0)
        return TRUE;
    }

  return FALSE;
}

static void
update_stereo_frame (GrdRdpRenderContext *render_context,
                     GrdRdpFrame         *rdp_frame,
                     GrdRdpRenderState   *render_state)
{
  uint32_t *damage_buffer =
    grd_rdp_render_state_get_damage_buffer (render_state);
  uint32_t state_buffer_length =
    grd_rdp_render_state_get_state_buffer_length (render_state);
  uint32_t i;

  if (!render_context->chroma_state_buffer)
    {
      if (!is_auxiliary_view_needed (render_state))
        {
          grd_rdp_frame_set_avc_view_type (rdp_frame,
                                           GRD_RDP_FRAME_VIEW_TYPE_MAIN);
        }

      return;
    }

  g_assert (render_context->state_buffer_length == state_buffer_length);

  for (i = 0; i < render_context->state_buffer_length; ++i)
    {
      if (render_context->chroma_state_buffer[i] != 0)
        damage_buffer[i] = 1;
    }
  g_clear_pointer (&render_context->chroma_state_buffer, g_free);
}

static void
update_main_frame (GrdRdpRenderContext *render_context,
                   GrdRdpFrame         *rdp_frame,
                   GrdRdpRenderState   *render_state)
{
  uint32_t *damage_buffer =
    grd_rdp_render_state_get_damage_buffer (render_state);
  uint32_t *chroma_state_buffer =
    grd_rdp_render_state_get_chroma_state_buffer (render_state);
  uint32_t state_buffer_length =
    grd_rdp_render_state_get_state_buffer_length (render_state);
  gboolean pending_auxiliary_view = FALSE;
  uint32_t i;

  if (!render_context->chroma_state_buffer)
    {
      if (is_auxiliary_view_needed (render_state))
        {
          render_context->chroma_state_buffer =
            g_memdup2 (chroma_state_buffer,
                       state_buffer_length * sizeof (uint32_t));
          render_context->state_buffer_length = state_buffer_length;
        }

      return;
    }

  g_assert (render_context->state_buffer_length == state_buffer_length);

  for (i = 0; i < render_context->state_buffer_length; ++i)
    {
      if (chroma_state_buffer[i] != 0)
        render_context->chroma_state_buffer[i] = 1;
      else if (damage_buffer[i] != 0)
        render_context->chroma_state_buffer[i] = 0;

      if (render_context->chroma_state_buffer[i] != 0)
        pending_auxiliary_view = TRUE;
    }
  if (!pending_auxiliary_view)
    g_clear_pointer (&render_context->chroma_state_buffer, g_free);
}

static void
update_avc444_render_state (GrdRdpRenderContext *render_context,
                            GrdRdpFrame         *rdp_frame,
                            GrdRdpRenderState   *render_state)
{
  switch (grd_rdp_frame_get_avc_view_type (rdp_frame))
    {
    case GRD_RDP_FRAME_VIEW_TYPE_STEREO:
      update_stereo_frame (render_context, rdp_frame, render_state);
      break;
    case GRD_RDP_FRAME_VIEW_TYPE_MAIN:
      update_main_frame (render_context, rdp_frame, render_state);
      break;
    case GRD_RDP_FRAME_VIEW_TYPE_AUX:
      g_assert_not_reached ();
      break;
    }
}

static cairo_region_t *
create_damage_region (GrdRdpRenderContext *render_context,
                      GrdRdpRenderState   *render_state)
{
  GrdRdpGfxSurface *gfx_surface = render_context->gfx_surface;
  uint32_t *damage_buffer =
    grd_rdp_render_state_get_damage_buffer (render_state);
  GrdRdpSurface *rdp_surface;
  uint32_t surface_width;
  uint32_t surface_height;
  uint32_t state_buffer_width;
  uint32_t state_buffer_height;
  uint32_t state_buffer_stride;
  cairo_region_t *damage_region;
  uint32_t x, y;

  g_assert (gfx_surface);
  rdp_surface = grd_rdp_gfx_surface_get_rdp_surface (gfx_surface);

  surface_width = grd_rdp_surface_get_width (rdp_surface);
  surface_height = grd_rdp_surface_get_height (rdp_surface);

  state_buffer_width = grd_get_aligned_size (surface_width, 64) / 64;
  state_buffer_height = grd_get_aligned_size (surface_height, 64) / 64;
  state_buffer_stride = state_buffer_width;

  damage_region = cairo_region_create ();
  g_assert (cairo_region_status (damage_region) == CAIRO_STATUS_SUCCESS);

  for (y = 0; y < state_buffer_height; ++y)
    {
      for (x = 0; x < state_buffer_width; ++x)
        {
          cairo_rectangle_int_t tile = {};
          uint32_t target_pos;

          target_pos = y * state_buffer_stride + x;
          if (damage_buffer[target_pos] == 0)
            continue;

          tile.x = x * STATE_TILE_WIDTH;
          tile.y = y * STATE_TILE_HEIGHT;
          tile.width = surface_width - tile.x < STATE_TILE_WIDTH ?
                         surface_width - tile.x : STATE_TILE_WIDTH;
          tile.height = surface_height - tile.y < STATE_TILE_HEIGHT ?
                          surface_height - tile.y : STATE_TILE_HEIGHT;

          cairo_region_union_rectangle (damage_region, &tile);
        }
    }

  return damage_region;
}

void
grd_rdp_render_context_update_frame_state (GrdRdpRenderContext *render_context,
                                           GrdRdpFrame         *rdp_frame,
                                           GrdRdpRenderState   *render_state)
{
  cairo_region_t *damage_region;

  switch (render_context->codec)
    {
    case GRD_RDP_CODEC_CAPROGRESSIVE:
    case GRD_RDP_CODEC_AVC420:
      break;
    case GRD_RDP_CODEC_AVC444v2:
      update_avc444_render_state (render_context, rdp_frame, render_state);
      break;
    }

  damage_region = create_damage_region (render_context, render_state);
  grd_rdp_frame_set_damage_region (rdp_frame, damage_region);

  grd_rdp_render_state_free (render_state);
}

static void
try_create_vaapi_session (GrdRdpRenderContext *render_context,
                          GrdRdpSurface       *rdp_surface,
                          gboolean             have_avc444)
{
  GrdRdpSurfaceRenderer *surface_renderer =
    grd_rdp_surface_get_surface_renderer (rdp_surface);
  GrdRdpBufferInfo *buffer_info =
    grd_rdp_surface_renderer_get_buffer_info (surface_renderer);
  uint32_t refresh_rate =
    grd_rdp_surface_renderer_get_refresh_rate (surface_renderer);
  uint32_t surface_width = grd_rdp_surface_get_width (rdp_surface);
  uint32_t surface_height = grd_rdp_surface_get_height (rdp_surface);
  g_autoptr (GrdEncodeSession) encode_session = NULL;
  GrdRdpViewCreatorAVC *view_creator_avc;
  uint32_t render_surface_width = 0;
  uint32_t render_surface_height = 0;
  g_autoptr (GError) error = NULL;
  GList *image_views;
  GList *l;

  if (buffer_info->buffer_type != GRD_RDP_BUFFER_TYPE_DMA_BUF ||
      buffer_info->drm_format_modifier == DRM_FORMAT_MOD_INVALID)
    return;

  encode_session =
    grd_hwaccel_vaapi_create_encode_session (render_context->hwaccel_vaapi,
                                             surface_width, surface_height,
                                             refresh_rate, &error);
  if (!encode_session)
    {
      g_debug ("[HWAccel.VAAPI] Could not create VAAPI encode session: %s",
               error->message);
      return;
    }

  grd_encode_session_get_surface_size (encode_session,
                                       &render_surface_width,
                                       &render_surface_height);

  g_assert (render_surface_width % 16 == 0);
  g_assert (render_surface_height % 16 == 0);
  g_assert (render_surface_width >= 16);
  g_assert (render_surface_height >= 16);

  view_creator_avc =
    grd_rdp_view_creator_avc_new (render_context->vk_device,
                                  render_surface_width,
                                  render_surface_height,
                                  surface_width,
                                  surface_height,
                                  &error);
  if (!view_creator_avc)
    {
      g_debug ("[HWAccel.Vulkan] Failed to create view creator for VAAPI "
               "encode session: %s", error->message);
      return;
    }

  image_views = grd_encode_session_get_image_views (encode_session);
  for (l = image_views; l; l = l->next)
    {
      GrdImageView *image_view = l->data;

      g_hash_table_add (render_context->image_views, image_view);
    }
  g_list_free (image_views);

  render_context->view_creator = GRD_RDP_VIEW_CREATOR (view_creator_avc);
  render_context->encode_session = g_steal_pointer (&encode_session);

  if (have_avc444)
    render_context->codec = GRD_RDP_CODEC_AVC444v2;
  else
    render_context->codec = GRD_RDP_CODEC_AVC420;

  g_debug ("[HWAccel.VAAPI] Created VAAPI encode session for surface with "
           "size %ux%u", surface_width, surface_height);
}

GrdRdpRenderContext *
grd_rdp_render_context_new (GrdRdpGraphicsPipeline *graphics_pipeline,
                            GrdRdpSurface          *rdp_surface,
                            GrdVkDevice            *vk_device,
                            GrdHwAccelVaapi        *hwaccel_vaapi)
{
  GrdRdpRenderContext *render_context;
  gboolean have_avc444 = FALSE;
  gboolean have_avc420 = FALSE;

  g_assert (graphics_pipeline);

  if (!grd_rdp_damage_detector_invalidate_surface (rdp_surface->detector))
    return NULL;

  render_context = g_object_new (GRD_TYPE_RDP_RENDER_CONTEXT, NULL);
  render_context->vk_device = vk_device;
  render_context->hwaccel_vaapi = hwaccel_vaapi;

  render_context->gfx_surface =
    grd_rdp_graphics_pipeline_acquire_gfx_surface (graphics_pipeline,
                                                   rdp_surface);
  render_context->codec = GRD_RDP_CODEC_CAPROGRESSIVE;

  grd_rdp_graphics_pipeline_get_capabilities (graphics_pipeline,
                                              &have_avc444, &have_avc420);
  if ((have_avc444 || have_avc420) && render_context->hwaccel_vaapi)
    try_create_vaapi_session (render_context, rdp_surface, have_avc444);

  return render_context;
}

static void
grd_rdp_render_context_dispose (GObject *object)
{
  GrdRdpRenderContext *render_context = GRD_RDP_RENDER_CONTEXT (object);

  if (render_context->acquired_image_views)
    g_assert (g_hash_table_size (render_context->acquired_image_views) == 0);

  g_clear_pointer (&render_context->chroma_state_buffer, g_free);

  g_clear_object (&render_context->view_creator);
  g_clear_object (&render_context->encode_session);
  g_clear_object (&render_context->gfx_surface);

  G_OBJECT_CLASS (grd_rdp_render_context_parent_class)->dispose (object);
}

static void
grd_rdp_render_context_finalize (GObject *object)
{
  GrdRdpRenderContext *render_context = GRD_RDP_RENDER_CONTEXT (object);

  g_clear_pointer (&render_context->acquired_image_views, g_hash_table_unref);
  g_clear_pointer (&render_context->image_views, g_hash_table_unref);

  G_OBJECT_CLASS (grd_rdp_render_context_parent_class)->finalize (object);
}

static void
grd_rdp_render_context_init (GrdRdpRenderContext *render_context)
{
  render_context->image_views = g_hash_table_new (NULL, NULL);
  render_context->acquired_image_views = g_hash_table_new (NULL, NULL);
}

static void
grd_rdp_render_context_class_init (GrdRdpRenderContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_render_context_dispose;
  object_class->finalize = grd_rdp_render_context_finalize;
}
