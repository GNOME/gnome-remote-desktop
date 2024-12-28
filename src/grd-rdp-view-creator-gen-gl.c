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

#include "grd-rdp-view-creator-gen-gl.h"

#include <drm_fourcc.h>

#include "grd-damage-detector-sw.h"
#include "grd-egl-thread.h"
#include "grd-image-view-rgb.h"
#include "grd-local-buffer-copy.h"
#include "grd-rdp-buffer.h"
#include "grd-rdp-pw-buffer.h"
#include "grd-rdp-render-state.h"
#include "grd-utils.h"

/*
 * One buffer is needed, when encoding a frame,
 * one is needed as download-target for the current dma-buf image,
 * one is needed for an already encoded frame, that was not yet
 * submitted to the client
 *
 * In total, this makes three needed buffers
 */
#define N_LOCAL_BUFFERS 3

typedef struct
{
  GrdRdpViewCreatorGenGL *view_creator_gen_gl;

  GrdImageView *image_view;
  GrdRdpBuffer *src_buffer_new;

  GrdLocalBuffer *local_buffer_new;
  GrdLocalBuffer *local_buffer_old;

  GrdSyncPoint sync_point;
} ViewContext;

struct _GrdRdpViewCreatorGenGL
{
  GrdRdpViewCreator parent;

  GrdEglThread *egl_thread;
  uint32_t surface_width;
  uint32_t surface_height;

  GrdLocalBuffer *local_buffers[N_LOCAL_BUFFERS];
  GHashTable *acquired_buffers;

  GrdDamageDetectorSw *damage_detector;

  GrdRdpBuffer *last_src_buffer;
  GrdLocalBuffer *last_local_buffer;

  ViewContext *current_view_context;
};

G_DEFINE_TYPE (GrdRdpViewCreatorGenGL, grd_rdp_view_creator_gen_gl,
               GRD_TYPE_RDP_VIEW_CREATOR)

static void
view_context_free (ViewContext *view_context);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ViewContext, view_context_free)

static GrdLocalBuffer *
acquire_local_buffer (GrdRdpViewCreatorGenGL *view_creator_gen_gl)
{
  uint32_t i;

  for (i = 0; i < N_LOCAL_BUFFERS; ++i)
    {
      GrdLocalBuffer *local_buffer = view_creator_gen_gl->local_buffers[i];

      if (g_hash_table_contains (view_creator_gen_gl->acquired_buffers,
                                 local_buffer))
        continue;
      if (local_buffer == view_creator_gen_gl->last_local_buffer)
        continue;

      g_hash_table_add (view_creator_gen_gl->acquired_buffers, local_buffer);

      return local_buffer;
    }

  g_assert_not_reached ();
  return NULL;
}

static void
release_local_buffer (GrdRdpViewCreatorGenGL *view_creator_gen_gl,
                      GrdLocalBuffer         *local_buffer)
{
  if (!g_hash_table_remove (view_creator_gen_gl->acquired_buffers, local_buffer))
    g_assert_not_reached ();
}

static ViewContext *
view_context_new (GrdRdpViewCreatorGenGL *view_creator_gen_gl,
                  GrdImageView           *image_view,
                  GrdRdpBuffer           *src_buffer_new,
                  GrdRdpBuffer           *src_buffer_old)
{
  ViewContext *view_context;

  view_context = g_new0 (ViewContext, 1);
  view_context->view_creator_gen_gl = view_creator_gen_gl;
  view_context->image_view = image_view;
  view_context->src_buffer_new = src_buffer_new;

  view_context->local_buffer_new = acquire_local_buffer (view_creator_gen_gl);

  if (src_buffer_old &&
      src_buffer_old == view_creator_gen_gl->last_src_buffer)
    view_context->local_buffer_old = view_creator_gen_gl->last_local_buffer;

  grd_sync_point_init (&view_context->sync_point);

  return view_context;
}

static void
view_context_free (ViewContext *view_context)
{
  GrdRdpViewCreatorGenGL *view_creator_gen_gl =
    view_context->view_creator_gen_gl;

  grd_sync_point_clear (&view_context->sync_point);
  if (view_context->local_buffer_new)
    release_local_buffer (view_creator_gen_gl, view_context->local_buffer_new);

  g_free (view_context);
}

static uint32_t
get_bpp_from_drm_format (uint32_t drm_format)
{
  switch (drm_format)
    {
    case DRM_FORMAT_BGRA8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_BGRX8888:
    case DRM_FORMAT_XRGB8888:
      return 4;
    default:
      g_assert_not_reached ();
    }
}

static void
on_dma_buf_downloaded (gboolean success,
                       gpointer user_data)
{
  GrdSyncPoint *sync_point = user_data;

  grd_sync_point_complete (sync_point, success);
}

static gboolean
grd_rdp_view_creator_gen_gl_create_view (GrdRdpViewCreator  *view_creator,
                                         GList              *image_views,
                                         GrdRdpBuffer       *src_buffer_new,
                                         GrdRdpBuffer       *src_buffer_old,
                                         GError            **error)
{
  GrdRdpViewCreatorGenGL *view_creator_gen_gl =
    GRD_RDP_VIEW_CREATOR_GEN_GL (view_creator);
  GrdRdpPwBuffer *rdp_pw_buffer =
    grd_rdp_buffer_get_rdp_pw_buffer (src_buffer_new);
  const GrdRdpPwBufferDmaBufInfo *dma_buf_info =
    grd_rdp_pw_buffer_get_dma_buf_info (rdp_pw_buffer);
  const GrdRdpBufferInfo *rdp_buffer_info =
    grd_rdp_buffer_get_rdp_buffer_info (src_buffer_new);
  ViewContext *view_context;
  GrdLocalBuffer *local_buffer_new;
  int row_width;

  g_assert (image_views);
  g_assert (!image_views->next);

  g_assert (!view_creator_gen_gl->current_view_context);

  view_context = view_context_new (view_creator_gen_gl, image_views->data,
                                   src_buffer_new, src_buffer_old);
  local_buffer_new = view_context->local_buffer_new;

  row_width = grd_local_buffer_get_buffer_stride (local_buffer_new) /
              get_bpp_from_drm_format (rdp_buffer_info->drm_format);

  grd_egl_thread_download (view_creator_gen_gl->egl_thread,
                           NULL,
                           grd_local_buffer_get_buffer (local_buffer_new),
                           row_width,
                           rdp_buffer_info->drm_format,
                           view_creator_gen_gl->surface_width,
                           view_creator_gen_gl->surface_height,
                           1,
                           &dma_buf_info->fd,
                           (uint32_t *) &dma_buf_info->stride,
                           &dma_buf_info->offset,
                           &rdp_buffer_info->drm_format_modifier,
                           on_dma_buf_downloaded,
                           &view_context->sync_point,
                           NULL);

  view_creator_gen_gl->current_view_context = view_context;

  return TRUE;
}

static void
on_image_view_release (gpointer        user_data,
                       GrdLocalBuffer *local_buffer)
{
  GrdRdpViewCreatorGenGL *view_creator_gen_gl = user_data;

  release_local_buffer (view_creator_gen_gl, local_buffer);
}

static GrdRdpRenderState *
grd_rdp_view_creator_gen_gl_finish_view (GrdRdpViewCreator  *view_creator,
                                         GError            **error)
{
  GrdRdpViewCreatorGenGL *view_creator_gen_gl =
    GRD_RDP_VIEW_CREATOR_GEN_GL (view_creator);
  GrdDamageDetectorSw *damage_detector = view_creator_gen_gl->damage_detector;
  g_autoptr (ViewContext) view_context = NULL;
  GrdLocalBuffer *local_buffer_new;
  GrdImageViewRGB *image_view_rgb;
  uint32_t *damage_buffer;
  uint32_t damage_buffer_length;

  view_context = g_steal_pointer (&view_creator_gen_gl->current_view_context);

  if (!grd_sync_point_wait_for_completion (&view_context->sync_point))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to download dma-buf image");
      return NULL;
    }

  grd_damage_detector_sw_compute_damage (damage_detector,
                                         view_context->local_buffer_new,
                                         view_context->local_buffer_old);

  local_buffer_new = g_steal_pointer (&view_context->local_buffer_new);
  image_view_rgb = GRD_IMAGE_VIEW_RGB (view_context->image_view);

  view_creator_gen_gl->last_src_buffer = view_context->src_buffer_new;
  view_creator_gen_gl->last_local_buffer = local_buffer_new;

  grd_image_view_rgb_attach_local_buffer (image_view_rgb,
                                          local_buffer_new,
                                          on_image_view_release,
                                          view_creator_gen_gl);

  damage_buffer = grd_damage_detector_sw_get_damage_buffer (damage_detector);
  damage_buffer_length =
    grd_damage_detector_sw_get_damage_buffer_length (damage_detector);

  return grd_rdp_render_state_new (damage_buffer, NULL, damage_buffer_length);
}

GrdRdpViewCreatorGenGL *
grd_rdp_view_creator_gen_gl_new (GrdEglThread *egl_thread,
                                 uint32_t      surface_width,
                                 uint32_t      surface_height)
{
  GrdRdpViewCreatorGenGL *view_creator_gen_gl;
  uint32_t i;

  view_creator_gen_gl = g_object_new (GRD_TYPE_RDP_VIEW_CREATOR_GEN_GL, NULL);
  view_creator_gen_gl->egl_thread = egl_thread;
  view_creator_gen_gl->surface_width = surface_width;
  view_creator_gen_gl->surface_height = surface_height;

  view_creator_gen_gl->damage_detector =
    grd_damage_detector_sw_new (surface_width, surface_height);

  for (i = 0; i < N_LOCAL_BUFFERS; ++i)
    {
      GrdLocalBufferCopy *local_buffer_copy;

      local_buffer_copy = grd_local_buffer_copy_new (surface_width,
                                                     surface_height);
      view_creator_gen_gl->local_buffers[i] =
        GRD_LOCAL_BUFFER (local_buffer_copy);
    }

  return view_creator_gen_gl;
}

static void
grd_rdp_view_creator_gen_gl_dispose (GObject *object)
{
  GrdRdpViewCreatorGenGL *view_creator_gen_gl =
    GRD_RDP_VIEW_CREATOR_GEN_GL (object);
  uint32_t i;

  if (view_creator_gen_gl->acquired_buffers)
    g_assert (g_hash_table_size (view_creator_gen_gl->acquired_buffers) == 0);

  g_assert (!view_creator_gen_gl->current_view_context);

  g_clear_object (&view_creator_gen_gl->damage_detector);

  g_clear_pointer (&view_creator_gen_gl->acquired_buffers, g_hash_table_unref);

  for (i = 0; i < N_LOCAL_BUFFERS; ++i)
    g_clear_object (&view_creator_gen_gl->local_buffers[i]);

  G_OBJECT_CLASS (grd_rdp_view_creator_gen_gl_parent_class)->dispose (object);
}

static void
grd_rdp_view_creator_gen_gl_init (GrdRdpViewCreatorGenGL *view_creator_gen_gl)
{
  view_creator_gen_gl->acquired_buffers = g_hash_table_new (NULL, NULL);
}

static void
grd_rdp_view_creator_gen_gl_class_init (GrdRdpViewCreatorGenGLClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdRdpViewCreatorClass *view_creator_class =
    GRD_RDP_VIEW_CREATOR_CLASS (klass);

  object_class->dispose = grd_rdp_view_creator_gen_gl_dispose;

  view_creator_class->create_view = grd_rdp_view_creator_gen_gl_create_view;
  view_creator_class->finish_view = grd_rdp_view_creator_gen_gl_finish_view;
}
