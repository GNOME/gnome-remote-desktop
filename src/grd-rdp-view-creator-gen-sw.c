/*
 * Copyright (C) 2025 Pascal Nowack
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

#include "grd-rdp-view-creator-gen-sw.h"

#include "grd-damage-detector-sw.h"
#include "grd-image-view-rgb.h"
#include "grd-local-buffer-wrapper-rdp.h"
#include "grd-rdp-render-state.h"

/*
 * One buffer is needed, when encoding a frame,
 * one is needed for the current view creation,
 * one is needed for an already encoded frame,
 * that was not yet submitted to the client
 *
 * In total, this makes three needed buffers
 */
#define N_LOCAL_BUFFERS 3

typedef struct
{
  GrdRdpViewCreatorGenSW *view_creator_gen_sw;

  GrdImageView *image_view;
  GrdRdpBuffer *src_buffer_new;

  GrdLocalBuffer *local_buffer_new;
  GrdLocalBuffer *local_buffer_old;
} ViewContext;

struct _GrdRdpViewCreatorGenSW
{
  GrdRdpViewCreator parent;

  GrdLocalBuffer *local_buffers[N_LOCAL_BUFFERS];
  GHashTable *acquired_buffers;

  GrdDamageDetectorSw *damage_detector;

  GrdRdpBuffer *last_src_buffer;
  GrdLocalBuffer *last_local_buffer;

  ViewContext *current_view_context;
};

G_DEFINE_TYPE (GrdRdpViewCreatorGenSW, grd_rdp_view_creator_gen_sw,
               GRD_TYPE_RDP_VIEW_CREATOR)

static void
view_context_free (ViewContext *view_context);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ViewContext, view_context_free)

static GrdLocalBuffer *
acquire_local_buffer (GrdRdpViewCreatorGenSW *view_creator_gen_sw)
{
  uint32_t i;

  for (i = 0; i < N_LOCAL_BUFFERS; ++i)
    {
      GrdLocalBuffer *local_buffer = view_creator_gen_sw->local_buffers[i];

      if (g_hash_table_contains (view_creator_gen_sw->acquired_buffers,
                                 local_buffer))
        continue;
      if (local_buffer == view_creator_gen_sw->last_local_buffer)
        continue;

      g_hash_table_add (view_creator_gen_sw->acquired_buffers, local_buffer);

      return local_buffer;
    }

  g_assert_not_reached ();
  return NULL;
}

static void
release_local_buffer (GrdRdpViewCreatorGenSW *view_creator_gen_sw,
                      GrdLocalBuffer         *local_buffer)
{
  if (!g_hash_table_remove (view_creator_gen_sw->acquired_buffers, local_buffer))
    g_assert_not_reached ();
}

static ViewContext *
view_context_new (GrdRdpViewCreatorGenSW *view_creator_gen_sw,
                  GrdImageView           *image_view,
                  GrdRdpBuffer           *src_buffer_new,
                  GrdRdpBuffer           *src_buffer_old)
{
  ViewContext *view_context;
  GrdLocalBufferWrapperRdp *buffer_wrapper;

  view_context = g_new0 (ViewContext, 1);
  view_context->view_creator_gen_sw = view_creator_gen_sw;
  view_context->image_view = image_view;
  view_context->src_buffer_new = src_buffer_new;

  view_context->local_buffer_new = acquire_local_buffer (view_creator_gen_sw);

  buffer_wrapper =
    GRD_LOCAL_BUFFER_WRAPPER_RDP (view_context->local_buffer_new);
  grd_local_buffer_wrapper_rdp_attach_rdp_buffer (buffer_wrapper,
                                                  src_buffer_new);

  if (src_buffer_old &&
      src_buffer_old == view_creator_gen_sw->last_src_buffer)
    view_context->local_buffer_old = view_creator_gen_sw->last_local_buffer;

  return view_context;
}

static void
view_context_free (ViewContext *view_context)
{
  GrdRdpViewCreatorGenSW *view_creator_gen_sw =
    view_context->view_creator_gen_sw;

  if (view_context->local_buffer_new)
    release_local_buffer (view_creator_gen_sw, view_context->local_buffer_new);

  g_free (view_context);
}

static gboolean
grd_rdp_view_creator_gen_sw_create_view (GrdRdpViewCreator  *view_creator,
                                         GList              *image_views,
                                         GrdRdpBuffer       *src_buffer_new,
                                         GrdRdpBuffer       *src_buffer_old,
                                         GError            **error)
{
  GrdRdpViewCreatorGenSW *view_creator_gen_sw =
    GRD_RDP_VIEW_CREATOR_GEN_SW (view_creator);

  g_assert (image_views);
  g_assert (!image_views->next);

  g_assert (!view_creator_gen_sw->current_view_context);

  view_creator_gen_sw->current_view_context =
    view_context_new (view_creator_gen_sw, image_views->data,
                      src_buffer_new, src_buffer_old);

  return TRUE;
}

static void
on_image_view_release (gpointer        user_data,
                       GrdLocalBuffer *local_buffer)
{
  GrdRdpViewCreatorGenSW *view_creator_gen_sw = user_data;

  release_local_buffer (view_creator_gen_sw, local_buffer);
}

static GrdRdpRenderState *
grd_rdp_view_creator_gen_sw_finish_view (GrdRdpViewCreator  *view_creator,
                                         GError            **error)
{
  GrdRdpViewCreatorGenSW *view_creator_gen_sw =
    GRD_RDP_VIEW_CREATOR_GEN_SW (view_creator);
  GrdDamageDetectorSw *damage_detector = view_creator_gen_sw->damage_detector;
  g_autoptr (ViewContext) view_context = NULL;
  GrdLocalBuffer *local_buffer_new;
  GrdImageViewRGB *image_view_rgb;
  uint32_t *damage_buffer;
  uint32_t damage_buffer_length;

  view_context = g_steal_pointer (&view_creator_gen_sw->current_view_context);

  grd_damage_detector_sw_compute_damage (damage_detector,
                                         view_context->local_buffer_new,
                                         view_context->local_buffer_old);

  local_buffer_new = g_steal_pointer (&view_context->local_buffer_new);
  image_view_rgb = GRD_IMAGE_VIEW_RGB (view_context->image_view);

  view_creator_gen_sw->last_src_buffer = view_context->src_buffer_new;
  view_creator_gen_sw->last_local_buffer = local_buffer_new;

  grd_image_view_rgb_attach_local_buffer (image_view_rgb,
                                          local_buffer_new,
                                          on_image_view_release,
                                          view_creator_gen_sw);

  damage_buffer = grd_damage_detector_sw_get_damage_buffer (damage_detector);
  damage_buffer_length =
    grd_damage_detector_sw_get_damage_buffer_length (damage_detector);

  return grd_rdp_render_state_new (damage_buffer, NULL, damage_buffer_length);
}

GrdRdpViewCreatorGenSW *
grd_rdp_view_creator_gen_sw_new (uint32_t surface_width,
                                 uint32_t surface_height)
{
  GrdRdpViewCreatorGenSW *view_creator_gen_sw;
  uint32_t i;

  view_creator_gen_sw = g_object_new (GRD_TYPE_RDP_VIEW_CREATOR_GEN_SW, NULL);

  view_creator_gen_sw->damage_detector =
    grd_damage_detector_sw_new (surface_width, surface_height);

  for (i = 0; i < N_LOCAL_BUFFERS; ++i)
    {
      view_creator_gen_sw->local_buffers[i] =
        GRD_LOCAL_BUFFER (grd_local_buffer_wrapper_rdp_new ());
    }

  return view_creator_gen_sw;
}

static void
grd_rdp_view_creator_gen_sw_dispose (GObject *object)
{
  GrdRdpViewCreatorGenSW *view_creator_gen_sw =
    GRD_RDP_VIEW_CREATOR_GEN_SW (object);
  uint32_t i;

  if (view_creator_gen_sw->acquired_buffers)
    g_assert (g_hash_table_size (view_creator_gen_sw->acquired_buffers) == 0);

  g_assert (!view_creator_gen_sw->current_view_context);

  g_clear_object (&view_creator_gen_sw->damage_detector);

  g_clear_pointer (&view_creator_gen_sw->acquired_buffers, g_hash_table_unref);

  for (i = 0; i < N_LOCAL_BUFFERS; ++i)
    g_clear_object (&view_creator_gen_sw->local_buffers[i]);

  G_OBJECT_CLASS (grd_rdp_view_creator_gen_sw_parent_class)->dispose (object);
}

static void
grd_rdp_view_creator_gen_sw_init (GrdRdpViewCreatorGenSW *view_creator_gen_sw)
{
  view_creator_gen_sw->acquired_buffers = g_hash_table_new (NULL, NULL);
}

static void
grd_rdp_view_creator_gen_sw_class_init (GrdRdpViewCreatorGenSWClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdRdpViewCreatorClass *view_creator_class =
    GRD_RDP_VIEW_CREATOR_CLASS (klass);

  object_class->dispose = grd_rdp_view_creator_gen_sw_dispose;

  view_creator_class->create_view = grd_rdp_view_creator_gen_sw_create_view;
  view_creator_class->finish_view = grd_rdp_view_creator_gen_sw_finish_view;
}
