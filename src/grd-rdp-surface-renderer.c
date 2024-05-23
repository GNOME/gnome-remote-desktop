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

#include "grd-rdp-surface-renderer.h"

#include <drm_fourcc.h>

#include "grd-rdp-buffer.h"
#include "grd-rdp-damage-detector.h"
#include "grd-rdp-pw-buffer.h"
#include "grd-rdp-renderer.h"
#include "grd-rdp-session-metrics.h"
#include "grd-rdp-surface.h"
#include "grd-session-rdp.h"

struct _GrdRdpSurfaceRenderer
{
  GObject parent;

  GrdRdpSurface *rdp_surface;
  GrdRdpRenderer *renderer;
  GrdSessionRdp *session_rdp;

  uint32_t refresh_rate;

  GSource *render_source;
  gboolean rendering_suspended;
  gboolean pending_render_context_reset;

  gboolean graphics_subsystem_failed;

  GMutex render_mutex;

  GHashTable *registered_buffers;
  GrdRdpBufferInfo *rdp_buffer_info;
};

G_DEFINE_TYPE (GrdRdpSurfaceRenderer, grd_rdp_surface_renderer, G_TYPE_OBJECT)

uint32_t
grd_rdp_surface_renderer_get_refresh_rate (GrdRdpSurfaceRenderer *surface_renderer)
{
  return surface_renderer->refresh_rate;
}

gboolean
grd_rdp_surface_renderer_is_rendering_suspended (GrdRdpSurfaceRenderer *surface_renderer)
{
  return surface_renderer->rendering_suspended;
}

void
grd_rdp_surface_renderer_update_suspension_state (GrdRdpSurfaceRenderer *surface_renderer,
                                                  gboolean               suspend_rendering)
{
  gboolean rendering_was_suspended = surface_renderer->rendering_suspended;

  surface_renderer->rendering_suspended = suspend_rendering;
  if (rendering_was_suspended && !surface_renderer->rendering_suspended)
    grd_rdp_surface_renderer_trigger_render_source (surface_renderer);
}

static gboolean
is_buffer_combination_valid (GrdRdpSurfaceRenderer  *surface_renderer,
                             GrdRdpPwBuffer         *rdp_pw_buffer,
                             GError                **error)
{
  GrdRdpBufferInfo *rdp_buffer_info = surface_renderer->rdp_buffer_info;
  GrdRdpBufferType buffer_type;

  if (g_hash_table_size (surface_renderer->registered_buffers) == 0)
    return TRUE;

  g_assert (rdp_buffer_info);

  buffer_type = grd_rdp_pw_buffer_get_buffer_type (rdp_pw_buffer);
  if (buffer_type != rdp_buffer_info->buffer_type)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid buffer combination: Mixed buffer types");
      return FALSE;
    }

  /*
   * No need to check whether an implicit or explicit DRM format modifier is
   * used per buffer, since per PipeWire stream there is only one DRM format
   * modifier and that one is used by all PW buffers of that PW stream.
   */

  return TRUE;
}

static gboolean
is_render_context_reset_required (GrdRdpSurfaceRenderer *surface_renderer,
                                  GrdRdpPwBuffer        *rdp_pw_buffer,
                                  uint64_t               new_drm_format_modifier)
{
  GrdRdpBufferInfo *rdp_buffer_info;
  GrdRdpBufferType buffer_type;
  uint64_t current_drm_format_modifier;

  if (g_hash_table_size (surface_renderer->registered_buffers) > 0)
    return FALSE;

  rdp_buffer_info = surface_renderer->rdp_buffer_info;
  if (!rdp_buffer_info)
    return FALSE;

  buffer_type = rdp_buffer_info->buffer_type;
  if (buffer_type != grd_rdp_pw_buffer_get_buffer_type (rdp_pw_buffer))
    return TRUE;

  current_drm_format_modifier = rdp_buffer_info->drm_format_modifier;
  if (current_drm_format_modifier != DRM_FORMAT_MOD_INVALID &&
      new_drm_format_modifier == DRM_FORMAT_MOD_INVALID)
    return TRUE;
  if (current_drm_format_modifier == DRM_FORMAT_MOD_INVALID &&
      new_drm_format_modifier != DRM_FORMAT_MOD_INVALID)
    return TRUE;

  return FALSE;
}

static GrdRdpBufferInfo *
rdp_buffer_info_new (GrdRdpPwBuffer *rdp_pw_buffer,
                     uint64_t        drm_format_modifier)
{
  GrdRdpBufferType buffer_type =
    grd_rdp_pw_buffer_get_buffer_type (rdp_pw_buffer);
  GrdRdpBufferInfo *rdp_buffer_info;

  rdp_buffer_info = g_new0 (GrdRdpBufferInfo, 1);
  rdp_buffer_info->buffer_type = buffer_type;
  rdp_buffer_info->drm_format_modifier = drm_format_modifier;

  return rdp_buffer_info;
}

gboolean
grd_rdp_surface_renderer_register_pw_buffer (GrdRdpSurfaceRenderer  *surface_renderer,
                                             GrdRdpPwBuffer         *rdp_pw_buffer,
                                             uint64_t                drm_format_modifier,
                                             GError                **error)
{
  if (!is_buffer_combination_valid (surface_renderer, rdp_pw_buffer, error))
    return FALSE;

  if (is_render_context_reset_required (surface_renderer, rdp_pw_buffer,
                                        drm_format_modifier))
    surface_renderer->pending_render_context_reset = TRUE;

  if (surface_renderer->rdp_buffer_info &&
      surface_renderer->rdp_buffer_info->buffer_type == GRD_RDP_BUFFER_TYPE_DMA_BUF &&
      surface_renderer->rdp_buffer_info->drm_format_modifier != drm_format_modifier)
    g_assert (g_hash_table_size (surface_renderer->registered_buffers) == 0);

  if (g_hash_table_size (surface_renderer->registered_buffers) == 0)
    g_clear_pointer (&surface_renderer->rdp_buffer_info, g_free);

  if (surface_renderer->pending_render_context_reset)
    g_assert (!surface_renderer->rdp_buffer_info);

  if (!surface_renderer->rdp_buffer_info)
    {
      surface_renderer->rdp_buffer_info =
        rdp_buffer_info_new (rdp_pw_buffer, drm_format_modifier);
    }
  g_hash_table_add (surface_renderer->registered_buffers, rdp_pw_buffer);

  return TRUE;
}

void
grd_rdp_surface_renderer_unregister_pw_buffer (GrdRdpSurfaceRenderer *surface_renderer,
                                               GrdRdpPwBuffer        *rdp_pw_buffer)
{
  g_hash_table_remove (surface_renderer->registered_buffers, rdp_pw_buffer);
}

void
grd_rdp_surface_renderer_submit_buffer (GrdRdpSurfaceRenderer *surface_renderer,
                                        GrdRdpBuffer          *buffer)
{
  GrdRdpSurface *rdp_surface = surface_renderer->rdp_surface;

  g_mutex_lock (&surface_renderer->render_mutex);
  g_clear_pointer (&rdp_surface->pending_framebuffer, grd_rdp_buffer_release);

  rdp_surface->pending_framebuffer = buffer;
  g_mutex_unlock (&surface_renderer->render_mutex);

  grd_rdp_surface_renderer_trigger_render_source (surface_renderer);
}

void
grd_rdp_surface_renderer_trigger_render_source (GrdRdpSurfaceRenderer *surface_renderer)
{
  g_source_set_ready_time (surface_renderer->render_source, 0);
}

void
grd_rdp_surface_renderer_reset (GrdRdpSurfaceRenderer *surface_renderer)
{
  GrdRdpSurface *rdp_surface = surface_renderer->rdp_surface;

  g_mutex_lock (&surface_renderer->render_mutex);
  g_clear_pointer (&rdp_surface->pending_framebuffer, grd_rdp_buffer_release);
  g_mutex_unlock (&surface_renderer->render_mutex);
}

static void
handle_graphics_subsystem_failure (GrdRdpSurfaceRenderer *surface_renderer)
{
  surface_renderer->graphics_subsystem_failed = TRUE;

  grd_session_rdp_notify_error (surface_renderer->session_rdp,
                                GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED);
}

static void
maybe_encode_pending_frame (GrdRdpSurfaceRenderer *surface_renderer,
                            GrdRdpRenderContext   *render_context)
{
  GrdRdpSurface *rdp_surface = surface_renderer->rdp_surface;
  GrdRdpSessionMetrics *session_metrics =
    grd_session_rdp_get_session_metrics (surface_renderer->session_rdp);
  GrdRdpBuffer *buffer;

  buffer = g_steal_pointer (&rdp_surface->pending_framebuffer);
  if (!grd_rdp_damage_detector_submit_new_framebuffer (rdp_surface->detector,
                                                       buffer))
    {
      grd_rdp_buffer_release (buffer);
      handle_graphics_subsystem_failure (surface_renderer);
      return;
    }

  if (!grd_rdp_damage_detector_is_region_damaged (rdp_surface->detector))
    return;

  if (!grd_rdp_renderer_render_frame (surface_renderer->renderer,
                                      rdp_surface, render_context, buffer))
    {
      handle_graphics_subsystem_failure (surface_renderer);
      return;
    }

  grd_rdp_session_metrics_notify_frame_transmission (session_metrics,
                                                     rdp_surface);
}

static gboolean
maybe_render_frame (gpointer user_data)
{
  GrdRdpSurfaceRenderer *surface_renderer = user_data;
  GrdRdpRenderer *renderer = surface_renderer->renderer;
  GrdRdpSurface *rdp_surface = surface_renderer->rdp_surface;
  GrdRdpAcquireContextFlags acquire_flags;
  GrdRdpRenderContext *render_context;
  g_autoptr (GMutexLocker) locker = NULL;

  if (surface_renderer->graphics_subsystem_failed)
    return G_SOURCE_CONTINUE;

  locker = g_mutex_locker_new (&surface_renderer->render_mutex);
  if (!rdp_surface->pending_framebuffer)
    return G_SOURCE_CONTINUE;

  if (surface_renderer->rendering_suspended)
    return G_SOURCE_CONTINUE;

  acquire_flags = GRD_RDP_ACQUIRE_CONTEXT_FLAG_NONE;
  if (surface_renderer->pending_render_context_reset)
    acquire_flags |= GRD_RDP_ACQUIRE_CONTEXT_FLAG_FORCE_RESET;

  render_context =
    grd_rdp_renderer_try_acquire_render_context (renderer, rdp_surface,
                                                 acquire_flags);
  if (!render_context)
    return G_SOURCE_CONTINUE;

  surface_renderer->pending_render_context_reset = FALSE;

  maybe_encode_pending_frame (surface_renderer, render_context);
  grd_rdp_renderer_release_render_context (renderer, render_context);

  return G_SOURCE_CONTINUE;
}

static gboolean
render_source_dispatch (GSource     *source,
                        GSourceFunc  callback,
                        gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs render_source_funcs =
{
  .dispatch = render_source_dispatch,
};

GrdRdpSurfaceRenderer *
grd_rdp_surface_renderer_new (GrdRdpSurface  *rdp_surface,
                              GrdRdpRenderer *renderer,
                              GrdSessionRdp  *session_rdp,
                              uint32_t        refresh_rate)
{
  GMainContext *graphics_context =
    grd_rdp_renderer_get_graphics_context (renderer);
  GrdRdpSurfaceRenderer *surface_renderer;
  GSource *render_source;

  surface_renderer = g_object_new (GRD_TYPE_RDP_SURFACE_RENDERER, NULL);
  surface_renderer->rdp_surface = rdp_surface;
  surface_renderer->renderer = renderer;
  surface_renderer->session_rdp = session_rdp;
  surface_renderer->refresh_rate = refresh_rate;

  render_source = g_source_new (&render_source_funcs, sizeof (GSource));
  g_source_set_callback (render_source, maybe_render_frame,
                         surface_renderer, NULL);
  g_source_set_ready_time (render_source, -1);
  g_source_attach (render_source, graphics_context);
  surface_renderer->render_source = render_source;

  return surface_renderer;
}

static void
grd_rdp_surface_renderer_dispose (GObject *object)
{
  GrdRdpSurfaceRenderer *surface_renderer = GRD_RDP_SURFACE_RENDERER (object);

  if (surface_renderer->render_source)
    {
      g_source_destroy (surface_renderer->render_source);
      g_clear_pointer (&surface_renderer->render_source, g_source_unref);
    }

  g_clear_pointer (&surface_renderer->registered_buffers, g_hash_table_unref);
  g_clear_pointer (&surface_renderer->rdp_buffer_info, g_free);

  G_OBJECT_CLASS (grd_rdp_surface_renderer_parent_class)->dispose (object);
}

static void
grd_rdp_surface_renderer_finalize (GObject *object)
{
  GrdRdpSurfaceRenderer *surface_renderer = GRD_RDP_SURFACE_RENDERER (object);

  g_mutex_clear (&surface_renderer->render_mutex);

  G_OBJECT_CLASS (grd_rdp_surface_renderer_parent_class)->finalize (object);
}

static void
grd_rdp_surface_renderer_init (GrdRdpSurfaceRenderer *surface_renderer)
{
  surface_renderer->registered_buffers = g_hash_table_new (NULL, NULL);

  g_mutex_init (&surface_renderer->render_mutex);
}

static void
grd_rdp_surface_renderer_class_init (GrdRdpSurfaceRendererClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_surface_renderer_dispose;
  object_class->finalize = grd_rdp_surface_renderer_finalize;
}
