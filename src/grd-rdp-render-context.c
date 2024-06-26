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
#include "grd-rdp-graphics-pipeline.h"
#include "grd-rdp-surface.h"
#include "grd-rdp-surface-renderer.h"
#include "grd-rdp-view-creator-avc.h"

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
