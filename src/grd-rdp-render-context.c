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

#include "grd-rdp-damage-detector.h"
#include "grd-rdp-graphics-pipeline.h"
#include "grd-rdp-surface.h"

struct _GrdRdpRenderContext
{
  GObject parent;

  GrdRdpCodec codec;

  GrdRdpGfxSurface *gfx_surface;

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

GrdRdpRenderContext *
grd_rdp_render_context_new (GrdRdpGraphicsPipeline *graphics_pipeline,
                            GrdRdpSurface          *rdp_surface)
{
  GrdRdpRenderContext *render_context;

  g_assert (graphics_pipeline);

  if (!grd_rdp_damage_detector_invalidate_surface (rdp_surface->detector))
    return NULL;

  render_context = g_object_new (GRD_TYPE_RDP_RENDER_CONTEXT, NULL);

  render_context->gfx_surface =
    grd_rdp_graphics_pipeline_acquire_gfx_surface (graphics_pipeline,
                                                   rdp_surface);
  render_context->codec = GRD_RDP_CODEC_CAPROGRESSIVE;

  return render_context;
}

static void
grd_rdp_render_context_dispose (GObject *object)
{
  GrdRdpRenderContext *render_context = GRD_RDP_RENDER_CONTEXT (object);

  if (render_context->acquired_image_views)
    g_assert (g_hash_table_size (render_context->acquired_image_views) == 0);

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
