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

#include "grd-rdp-renderer.h"

#include "grd-encode-session.h"
#include "grd-hwaccel-nvidia.h"
#include "grd-hwaccel-vaapi.h"
#include "grd-hwaccel-vulkan.h"
#include "grd-rdp-frame.h"
#include "grd-rdp-graphics-pipeline.h"
#include "grd-rdp-private.h"
#include "grd-rdp-render-context.h"
#include "grd-rdp-surface.h"
#include "grd-rdp-surface-renderer.h"
#include "grd-rdp-view-creator.h"
#include "grd-session-rdp.h"

enum
{
  INHIBITION_DONE,
  GFX_INITABLE,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _GrdRdpRenderer
{
  GObject parent;

  gboolean in_shutdown;

  GrdSessionRdp *session_rdp;
  GrdVkDevice *vk_device;
  GrdHwAccelNvidia *hwaccel_nvidia;
  GrdHwAccelVaapi *hwaccel_vaapi;

  GrdRdpGraphicsPipeline *graphics_pipeline;
  rdpContext *rdp_context;

  GThread *graphics_thread;
  GMainContext *graphics_context;

  gboolean graphics_subsystem_failed;

  gboolean stop_rendering;
  GCond stop_rendering_cond;

  gboolean rendering_inhibited;
  gboolean output_suppressed;

  gboolean pending_gfx_init;
  gboolean pending_gfx_graphics_reset;

  GMutex surface_renderers_mutex;
  GHashTable *surface_renderer_table;

  GMutex inhibition_mutex;
  GHashTable *render_context_table;
  GHashTable *acquired_render_contexts;
  GHashTable *render_resource_mappings;

  GSource *surface_disposal_source;
  GAsyncQueue *disposal_queue;

  GSource *surface_render_source;
  GHashTable *queued_frames;

  GMutex view_creations_mutex;
  GHashTable *finished_view_creations;

  GMutex frame_encodings_mutex;
  GHashTable *finished_frame_encodings;
};

G_DEFINE_TYPE (GrdRdpRenderer, grd_rdp_renderer, G_TYPE_OBJECT)

GMainContext *
grd_rdp_renderer_get_graphics_context (GrdRdpRenderer *renderer)
{
  return renderer->graphics_context;
}

static void
trigger_render_sources (GrdRdpRenderer *renderer)
{
  GrdRdpSurfaceRenderer *surface_renderer = NULL;
  g_autoptr (GMutexLocker) locker = NULL;
  GHashTableIter iter;

  locker = g_mutex_locker_new (&renderer->surface_renderers_mutex);
  g_hash_table_iter_init (&iter, renderer->surface_renderer_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &surface_renderer))
    grd_rdp_surface_renderer_trigger_render_source (surface_renderer);
}

void
grd_rdp_renderer_update_output_suppression_state (GrdRdpRenderer *renderer,
                                                  gboolean        suppress_output)
{
  renderer->output_suppressed = suppress_output;

  if (!renderer->output_suppressed)
    trigger_render_sources (renderer);
}

static void
stop_rendering (GrdRdpRenderer *renderer)
{
  g_mutex_lock (&renderer->inhibition_mutex);
  renderer->stop_rendering = TRUE;

  while (g_hash_table_size (renderer->acquired_render_contexts) > 0)
    g_cond_wait (&renderer->stop_rendering_cond, &renderer->inhibition_mutex);
  g_mutex_unlock (&renderer->inhibition_mutex);
}

void
grd_rdp_renderer_invoke_shutdown (GrdRdpRenderer *renderer)
{
  g_assert (renderer->graphics_context);

  stop_rendering (renderer);

  renderer->in_shutdown = TRUE;

  g_main_context_wakeup (renderer->graphics_context);
  g_clear_pointer (&renderer->graphics_thread, g_thread_join);
}

static gpointer
graphics_thread_func (gpointer data)
{
  GrdRdpRenderer *renderer = data;

  if (renderer->hwaccel_nvidia)
    grd_hwaccel_nvidia_push_cuda_context (renderer->hwaccel_nvidia);

  while (!renderer->in_shutdown)
    g_main_context_iteration (renderer->graphics_context, TRUE);

  if (renderer->hwaccel_nvidia)
    grd_hwaccel_nvidia_pop_cuda_context (renderer->hwaccel_nvidia);

  return NULL;
}

static gboolean
maybe_initialize_hardware_acceleration (GrdRdpRenderer   *renderer,
                                        GrdHwAccelVulkan *hwaccel_vulkan)
{
  g_autoptr (GError) error = NULL;

  renderer->vk_device = grd_hwaccel_vulkan_acquire_device (hwaccel_vulkan,
                                                           &error);
  if (!renderer->vk_device)
    {
      g_warning ("[RDP] Failed to acquire Vulkan device: %s",
                 error->message);
      return FALSE;
    }

  renderer->hwaccel_vaapi = grd_hwaccel_vaapi_new (renderer->vk_device,
                                                   &error);
  if (!renderer->hwaccel_vaapi)
    g_message ("[RDP] Did not initialize VAAPI: %s", error->message);

  return TRUE;
}

gboolean
grd_rdp_renderer_start (GrdRdpRenderer         *renderer,
                        GrdHwAccelVulkan       *hwaccel_vulkan,
                        GrdRdpGraphicsPipeline *graphics_pipeline,
                        rdpContext             *rdp_context)
{
  renderer->graphics_pipeline = graphics_pipeline;
  renderer->rdp_context = rdp_context;

  if (hwaccel_vulkan &&
      !maybe_initialize_hardware_acceleration (renderer, hwaccel_vulkan))
    return FALSE;

  renderer->graphics_thread = g_thread_new ("RDP graphics thread",
                                            graphics_thread_func,
                                            renderer);

  return TRUE;
}

void
grd_rdp_renderer_notify_new_desktop_layout (GrdRdpRenderer *renderer,
                                            uint32_t        desktop_width,
                                            uint32_t        desktop_height)
{
  rdpContext *rdp_context = renderer->rdp_context;
  rdpSettings *rdp_settings = rdp_context->settings;

  g_assert (renderer->graphics_pipeline);
  renderer->pending_gfx_graphics_reset = TRUE;

  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_DesktopWidth,
                               desktop_width);
  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_DesktopHeight,
                               desktop_height);
}

void
grd_rdp_renderer_notify_graphics_pipeline_ready (GrdRdpRenderer *renderer)
{
  renderer->pending_gfx_graphics_reset = TRUE;
  renderer->pending_gfx_init = FALSE;

  trigger_render_sources (renderer);
}

void
grd_rdp_renderer_notify_graphics_pipeline_reset (GrdRdpRenderer *renderer)
{
  gboolean gfx_initable = FALSE;

  g_mutex_lock (&renderer->inhibition_mutex);
  renderer->pending_gfx_init = TRUE;

  if (g_hash_table_size (renderer->acquired_render_contexts) == 0)
    gfx_initable = TRUE;
  g_mutex_unlock (&renderer->inhibition_mutex);

  if (gfx_initable)
    g_signal_emit (renderer, signals[GFX_INITABLE], 0);
}

void
grd_rdp_renderer_inhibit_rendering (GrdRdpRenderer *renderer)
{
  gboolean inhibition_done = FALSE;

  g_mutex_lock (&renderer->inhibition_mutex);
  renderer->rendering_inhibited = TRUE;

  if (g_hash_table_size (renderer->acquired_render_contexts) == 0)
    inhibition_done = TRUE;
  g_mutex_unlock (&renderer->inhibition_mutex);

  if (inhibition_done)
    g_signal_emit (renderer, signals[INHIBITION_DONE], 0);
}

void
grd_rdp_renderer_uninhibit_rendering (GrdRdpRenderer *renderer)
{
  renderer->rendering_inhibited = FALSE;

  trigger_render_sources (renderer);
}

GrdRdpSurface *
grd_rdp_renderer_try_acquire_surface (GrdRdpRenderer *renderer,
                                      uint32_t        refresh_rate)
{
  GrdRdpSurface *rdp_surface;
  GrdRdpSurfaceRenderer *surface_renderer;

  rdp_surface = grd_rdp_surface_new (renderer->hwaccel_nvidia);
  if (!rdp_surface)
    return NULL;

  surface_renderer = grd_rdp_surface_renderer_new (rdp_surface, renderer,
                                                   renderer->session_rdp,
                                                   renderer->vk_device,
                                                   refresh_rate);
  grd_rdp_surface_attach_surface_renderer (rdp_surface, surface_renderer);

  g_mutex_lock (&renderer->surface_renderers_mutex);
  g_hash_table_insert (renderer->surface_renderer_table,
                       rdp_surface, surface_renderer);
  g_mutex_unlock (&renderer->surface_renderers_mutex);

  return rdp_surface;
}

void
grd_rdp_renderer_release_surface (GrdRdpRenderer *renderer,
                                  GrdRdpSurface  *rdp_surface)
{
  g_assert (rdp_surface);

  g_async_queue_push (renderer->disposal_queue, rdp_surface);
  g_source_set_ready_time (renderer->surface_disposal_source, 0);
}

static void
invalidate_surfaces (GrdRdpRenderer *renderer,
                     gboolean        locked)
{
  GrdRdpSurfaceRenderer *surface_renderer = NULL;
  g_autoptr (GMutexLocker) locker = NULL;
  GHashTableIter iter;

  locker = g_mutex_locker_new (&renderer->surface_renderers_mutex);
  g_hash_table_iter_init (&iter, renderer->surface_renderer_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &surface_renderer))
    {
      if (!locked)
        grd_rdp_surface_renderer_invalidate_surface_unlocked (surface_renderer);
      else
        grd_rdp_surface_renderer_invalidate_surface (surface_renderer);
    }
}

static void
clear_render_contexts (GrdRdpRenderer *renderer,
                       gboolean        locked)
{
  g_assert (g_hash_table_size (renderer->acquired_render_contexts) == 0);
  g_assert (g_hash_table_size (renderer->queued_frames) == 0);

  invalidate_surfaces (renderer, locked);

  g_hash_table_remove_all (renderer->render_resource_mappings);
  g_hash_table_remove_all (renderer->render_context_table);
}

static void
maybe_reset_graphics (GrdRdpRenderer *renderer)
{
  rdpContext *rdp_context = renderer->rdp_context;
  rdpSettings *rdp_settings = rdp_context->settings;
  uint32_t desktop_width =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_DesktopWidth);
  uint32_t desktop_height =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_DesktopHeight);
  g_autofree MONITOR_DEF *monitor_defs = NULL;
  uint32_t n_monitors;
  uint32_t i;

  if (!renderer->pending_gfx_graphics_reset)
    return;

  clear_render_contexts (renderer, FALSE);

  n_monitors = freerdp_settings_get_uint32 (rdp_settings, FreeRDP_MonitorCount);
  g_assert (n_monitors > 0);

  monitor_defs = g_new0 (MONITOR_DEF, n_monitors);

  for (i = 0; i < n_monitors; ++i)
    {
      const rdpMonitor *monitor =
        freerdp_settings_get_pointer_array (rdp_settings,
                                            FreeRDP_MonitorDefArray, i);
      MONITOR_DEF *monitor_def = &monitor_defs[i];

      monitor_def->left = monitor->x;
      monitor_def->top = monitor->y;
      monitor_def->right = monitor_def->left + monitor->width - 1;
      monitor_def->bottom = monitor_def->top + monitor->height - 1;

      if (monitor->is_primary)
        monitor_def->flags = MONITOR_PRIMARY;
    }

  grd_rdp_graphics_pipeline_reset_graphics (renderer->graphics_pipeline,
                                            desktop_width, desktop_height,
                                            monitor_defs, n_monitors);
  renderer->pending_gfx_graphics_reset = FALSE;
}

static void
destroy_render_context_locked (GrdRdpRenderer *renderer,
                               GrdRdpSurface  *rdp_surface)
{
  GrdRdpRenderContext *render_context = NULL;

  if (!g_hash_table_lookup_extended (renderer->render_context_table,
                                     rdp_surface,
                                     NULL, (gpointer *) &render_context))
    return;

  g_assert (render_context);
  g_assert (!g_hash_table_contains (renderer->acquired_render_contexts,
                                    render_context));
  g_assert (!g_hash_table_contains (renderer->queued_frames,
                                    render_context));

  g_hash_table_remove (renderer->render_resource_mappings, render_context);
  g_hash_table_remove (renderer->render_context_table, rdp_surface);
}

static void
destroy_render_context (GrdRdpRenderer *renderer,
                        GrdRdpSurface  *rdp_surface)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&renderer->inhibition_mutex);
  destroy_render_context_locked (renderer, rdp_surface);
}

static GrdRdpRenderContext *
render_context_ref (GrdRdpRenderer      *renderer,
                    GrdRdpRenderContext *render_context)
{
  uint32_t *ref_count = NULL;

  if (g_hash_table_lookup_extended (renderer->acquired_render_contexts,
                                    render_context,
                                    NULL, (gpointer *) &ref_count))
    {
      g_assert (*ref_count > 0);
      ++(*ref_count);

      return render_context;
    }

  ref_count = g_new0 (uint32_t, 1);
  *ref_count = 1;

  g_hash_table_insert (renderer->acquired_render_contexts,
                       render_context, ref_count);

  return render_context;
}

static void
render_context_unref (GrdRdpRenderer      *renderer,
                      GrdRdpRenderContext *render_context)
{
  uint32_t *ref_count = NULL;

  if (!g_hash_table_lookup_extended (renderer->acquired_render_contexts,
                                     render_context,
                                     NULL, (gpointer *) &ref_count))
    g_assert_not_reached ();

  g_assert (*ref_count > 0);
  --(*ref_count);

  if (*ref_count == 0)
    g_hash_table_remove (renderer->acquired_render_contexts, render_context);
}

static void
handle_graphics_subsystem_failure (GrdRdpRenderer *renderer)
{
  renderer->graphics_subsystem_failed = TRUE;
  renderer->stop_rendering = TRUE;

  grd_session_rdp_notify_error (renderer->session_rdp,
                                GRD_SESSION_RDP_ERROR_GRAPHICS_SUBSYSTEM_FAILED);
}

GrdRdpRenderContext *
grd_rdp_renderer_try_acquire_render_context (GrdRdpRenderer            *renderer,
                                             GrdRdpSurface             *rdp_surface,
                                             GrdRdpAcquireContextFlags  flags)
{
  GrdRdpRenderContext *render_context = NULL;
  g_autoptr (GMutexLocker) locker = NULL;

  g_assert (!((flags & GRD_RDP_ACQUIRE_CONTEXT_FLAG_FORCE_RESET) &&
              (flags & GRD_RDP_ACQUIRE_CONTEXT_FLAG_RETAIN_OR_NULL)));

  locker = g_mutex_locker_new (&renderer->inhibition_mutex);
  if (renderer->stop_rendering ||
      renderer->rendering_inhibited ||
      renderer->pending_gfx_init ||
      renderer->output_suppressed)
    return NULL;

  maybe_reset_graphics (renderer);

  if (flags & GRD_RDP_ACQUIRE_CONTEXT_FLAG_FORCE_RESET)
    destroy_render_context_locked (renderer, rdp_surface);

  if (g_hash_table_lookup_extended (renderer->render_context_table, rdp_surface,
                                    NULL, (gpointer *) &render_context))
    return render_context_ref (renderer, render_context);

  if (flags & GRD_RDP_ACQUIRE_CONTEXT_FLAG_RETAIN_OR_NULL)
    return NULL;

  render_context = grd_rdp_render_context_new (renderer->graphics_pipeline,
                                               rdp_surface,
                                               renderer->vk_device,
                                               renderer->hwaccel_vaapi);
  if (!render_context)
    {
      handle_graphics_subsystem_failure (renderer);
      return NULL;
    }

  g_hash_table_insert (renderer->render_context_table,
                       rdp_surface, render_context);
  g_hash_table_insert (renderer->render_resource_mappings,
                       render_context, g_hash_table_new (NULL, NULL));

  return render_context_ref (renderer, render_context);
}

void
grd_rdp_renderer_release_render_context (GrdRdpRenderer      *renderer,
                                         GrdRdpRenderContext *render_context)
{
  gboolean inhibition_done = FALSE;
  gboolean gfx_initable = FALSE;

  g_mutex_lock (&renderer->inhibition_mutex);
  render_context_unref (renderer, render_context);

  if (renderer->stop_rendering &&
      g_hash_table_size (renderer->acquired_render_contexts) == 0)
    g_cond_signal (&renderer->stop_rendering_cond);

  if (renderer->rendering_inhibited &&
      g_hash_table_size (renderer->acquired_render_contexts) == 0)
    inhibition_done = TRUE;

  if (renderer->pending_gfx_init &&
      g_hash_table_size (renderer->acquired_render_contexts) == 0)
    gfx_initable = TRUE;
  g_mutex_unlock (&renderer->inhibition_mutex);

  if (inhibition_done)
    g_signal_emit (renderer, signals[INHIBITION_DONE], 0);
  if (gfx_initable)
    g_signal_emit (renderer, signals[GFX_INITABLE], 0);
}

void
grd_rdp_renderer_clear_render_contexts (GrdRdpRenderer *renderer)
{
  clear_render_contexts (renderer, TRUE);
}

static void
queue_prepared_frame (GrdRdpRenderer      *renderer,
                      GrdRdpRenderContext *render_context,
                      GrdRdpFrame         *rdp_frame)
{
  GHashTable *acquired_resources = NULL;
  GrdRdpViewCreator *view_creator;

  if (!g_hash_table_lookup_extended (renderer->render_resource_mappings,
                                     render_context,
                                     NULL, (gpointer *) &acquired_resources))
    g_assert_not_reached ();

  view_creator = grd_rdp_render_context_get_view_creator (render_context);
  g_assert (!g_hash_table_contains (acquired_resources, view_creator));

  grd_rdp_frame_notify_picked_up (rdp_frame);
  g_hash_table_insert (acquired_resources, view_creator, rdp_frame);

  g_mutex_lock (&renderer->view_creations_mutex);
  g_hash_table_add (renderer->finished_view_creations, rdp_frame);
  g_mutex_unlock (&renderer->view_creations_mutex);
}

void
grd_rdp_renderer_submit_frame (GrdRdpRenderer      *renderer,
                               GrdRdpRenderContext *render_context,
                               GrdRdpFrame         *rdp_frame)
{
  grd_rdp_frame_set_renderer (rdp_frame, renderer);

  if (grd_rdp_frame_has_valid_view (rdp_frame))
    queue_prepared_frame (renderer, render_context, rdp_frame);
  else
    g_hash_table_insert (renderer->queued_frames, render_context, rdp_frame);

  g_source_set_ready_time (renderer->surface_render_source, 0);
}

gboolean
grd_rdp_renderer_render_frame (GrdRdpRenderer      *renderer,
                               GrdRdpSurface       *rdp_surface,
                               GrdRdpRenderContext *render_context,
                               GrdRdpLegacyBuffer  *buffer)
{
  return grd_rdp_graphics_pipeline_refresh_gfx (renderer->graphics_pipeline,
                                                rdp_surface, render_context,
                                                buffer);
}

GrdRdpRenderer *
grd_rdp_renderer_new (GrdSessionRdp    *session_rdp,
                      GrdHwAccelNvidia *hwaccel_nvidia)
{
  GrdRdpRenderer *renderer;

  renderer = g_object_new (GRD_TYPE_RDP_RENDERER, NULL);
  renderer->session_rdp = session_rdp;
  renderer->hwaccel_nvidia = hwaccel_nvidia;

  return renderer;
}

static gboolean
dispose_surfaces (gpointer user_data)
{
  GrdRdpRenderer *renderer = user_data;
  GrdRdpSurface *rdp_surface;

  while ((rdp_surface = g_async_queue_try_pop (renderer->disposal_queue)))
    {
      destroy_render_context (renderer, rdp_surface);

      g_mutex_lock (&renderer->surface_renderers_mutex);
      g_hash_table_remove (renderer->surface_renderer_table, rdp_surface);
      g_mutex_unlock (&renderer->surface_renderers_mutex);

      grd_rdp_surface_free (rdp_surface);
    }

  return G_SOURCE_CONTINUE;
}

static void
grd_rdp_renderer_dispose (GObject *object)
{
  GrdRdpRenderer *renderer = GRD_RDP_RENDERER (object);

  grd_rdp_renderer_invoke_shutdown (renderer);

  if (renderer->acquired_render_contexts)
    g_assert (g_hash_table_size (renderer->acquired_render_contexts) == 0);
  if (renderer->render_resource_mappings)
    g_assert (g_hash_table_size (renderer->render_resource_mappings) == 0);
  if (renderer->queued_frames)
    g_assert (g_hash_table_size (renderer->queued_frames) == 0);
  if (renderer->finished_view_creations)
    g_assert (g_hash_table_size (renderer->finished_view_creations) == 0);
  if (renderer->finished_frame_encodings)
    g_assert (g_hash_table_size (renderer->finished_frame_encodings) == 0);

  if (renderer->surface_render_source)
    {
      g_source_destroy (renderer->surface_render_source);
      g_clear_pointer (&renderer->surface_render_source, g_source_unref);
    }
  if (renderer->surface_disposal_source)
    {
      g_source_destroy (renderer->surface_disposal_source);
      g_clear_pointer (&renderer->surface_disposal_source, g_source_unref);
    }

  g_clear_pointer (&renderer->graphics_context, g_main_context_unref);
  dispose_surfaces (renderer);

  g_clear_pointer (&renderer->finished_frame_encodings, g_hash_table_unref);
  g_clear_pointer (&renderer->finished_view_creations, g_hash_table_unref);
  g_clear_pointer (&renderer->queued_frames, g_hash_table_unref);
  g_clear_pointer (&renderer->render_resource_mappings, g_hash_table_unref);
  g_clear_pointer (&renderer->acquired_render_contexts, g_hash_table_unref);
  g_clear_pointer (&renderer->render_context_table, g_hash_table_unref);

  g_clear_pointer (&renderer->disposal_queue, g_async_queue_unref);

  g_assert (g_hash_table_size (renderer->surface_renderer_table) == 0);

  g_clear_object (&renderer->hwaccel_vaapi);
  g_clear_object (&renderer->vk_device);

  G_OBJECT_CLASS (grd_rdp_renderer_parent_class)->dispose (object);
}

static void
grd_rdp_renderer_finalize (GObject *object)
{
  GrdRdpRenderer *renderer = GRD_RDP_RENDERER (object);

  g_mutex_clear (&renderer->frame_encodings_mutex);
  g_mutex_clear (&renderer->view_creations_mutex);

  g_cond_clear (&renderer->stop_rendering_cond);
  g_mutex_clear (&renderer->inhibition_mutex);
  g_mutex_clear (&renderer->surface_renderers_mutex);

  g_clear_pointer (&renderer->surface_renderer_table, g_hash_table_unref);

  G_OBJECT_CLASS (grd_rdp_renderer_parent_class)->finalize (object);
}

static void
release_acquired_resource (GrdRdpRenderer      *renderer,
                           GrdRdpRenderContext *render_context,
                           gpointer             resource)
{
  GHashTable *acquired_resources = NULL;

  if (!g_hash_table_lookup_extended (renderer->render_resource_mappings,
                                     render_context,
                                     NULL, (gpointer *) &acquired_resources))
    g_assert_not_reached ();

  if (!g_hash_table_remove (acquired_resources, resource))
    g_assert_not_reached ();
}

static GList *
fetch_rendered_frames (GrdRdpRenderer *renderer)
{
  g_autoptr (GMutexLocker) locker = NULL;
  GrdRdpFrame *rdp_frame = NULL;
  GHashTableIter iter;
  GList *rendered_frames;

  locker = g_mutex_locker_new (&renderer->frame_encodings_mutex);
  g_hash_table_iter_init (&iter, renderer->finished_frame_encodings);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &rdp_frame))
    {
      GrdRdpRenderContext *render_context =
        grd_rdp_frame_get_render_context (rdp_frame);
      GrdEncodeSession *encode_session =
        grd_rdp_render_context_get_encode_session (render_context);

      release_acquired_resource (renderer, render_context, encode_session);

      if (!grd_rdp_frame_get_bitstreams (rdp_frame))
        g_hash_table_iter_remove (&iter);
    }

  rendered_frames =
    g_hash_table_get_values (renderer->finished_frame_encodings);
  g_hash_table_steal_all (renderer->finished_frame_encodings);

  return rendered_frames;
}

static void
on_bitstream_locked (GrdEncodeSession *encode_session,
                     GrdBitstream     *bitstream,
                     gpointer          user_data,
                     GError           *error)
{
  GrdRdpFrame *rdp_frame = user_data;
  GrdRdpRenderer *renderer = grd_rdp_frame_get_renderer (rdp_frame);

  if (bitstream)
    {
      GList *bitstreams = grd_rdp_frame_get_bitstreams (rdp_frame);

      bitstreams = g_list_append (bitstreams, bitstream);
      grd_rdp_frame_set_bitstreams (rdp_frame, bitstreams);
    }
  else
    {
      g_warning ("[RDP] Failed to lock bitstream: %s", error->message);
    }

  g_mutex_lock (&renderer->frame_encodings_mutex);
  if (!grd_encode_session_has_pending_frames (encode_session))
    g_hash_table_add (renderer->finished_frame_encodings, rdp_frame);

  if (!bitstream)
    renderer->graphics_subsystem_failed = TRUE;
  g_mutex_unlock (&renderer->frame_encodings_mutex);

  g_source_set_ready_time (renderer->surface_render_source, 0);
}

static gboolean
encode_image_views (GrdRdpRenderer *renderer,
                    GrdRdpFrame    *rdp_frame)
{
  g_autoptr (GMutexLocker) locker = NULL;
  gboolean submitted_frame = FALSE;
  g_autoptr (GError) error = NULL;
  GrdImageView *image_view;

  locker = g_mutex_locker_new (&renderer->frame_encodings_mutex);
  while ((image_view = grd_rdp_frame_pop_image_view (rdp_frame)))
    {
      GrdRdpRenderContext *render_context =
        grd_rdp_frame_get_render_context (rdp_frame);
      GrdEncodeSession *encode_session =
        grd_rdp_render_context_get_encode_session (render_context);
      GrdEncodeContext *encode_context =
        grd_rdp_frame_get_encode_context (rdp_frame);

      if (!grd_encode_session_encode_frame (encode_session, encode_context,
                                            image_view, &error))
        {
          g_warning ("[RDP] Failed to encode frame: %s", error->message);

          if (!submitted_frame)
            {
              release_acquired_resource (renderer, render_context,
                                         encode_session);
              grd_rdp_frame_free (rdp_frame);
            }

          return FALSE;
        }
      submitted_frame = TRUE;

      grd_encode_session_lock_bitstream (encode_session, image_view,
                                         on_bitstream_locked, rdp_frame);
    }

  return TRUE;
}

static gboolean
maybe_start_encodings (GrdRdpRenderer *renderer)
{
  g_autoptr (GMutexLocker) locker = NULL;
  GrdRdpFrame *rdp_frame = NULL;
  GHashTableIter iter;

  if (renderer->stop_rendering)
    return TRUE;

  locker = g_mutex_locker_new (&renderer->view_creations_mutex);
  if (renderer->graphics_subsystem_failed)
    return FALSE;

  g_hash_table_iter_init (&iter, renderer->finished_view_creations);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &rdp_frame))
    {
      GHashTable *acquired_resources = NULL;
      GrdRdpRenderContext *render_context;
      GrdRdpViewCreator *view_creator;
      GrdEncodeSession *encode_session;

      g_assert (grd_rdp_frame_has_valid_view (rdp_frame));

      render_context = grd_rdp_frame_get_render_context (rdp_frame);
      view_creator = grd_rdp_render_context_get_view_creator (render_context);

      if (!grd_rdp_frame_is_surface_damaged (rdp_frame))
        {
          release_acquired_resource (renderer, render_context, view_creator);
          g_hash_table_iter_remove (&iter);
          continue;
        }

      if (!g_hash_table_lookup_extended (renderer->render_resource_mappings,
                                         render_context,
                                         NULL, (gpointer *) &acquired_resources))
        g_assert_not_reached ();

      encode_session =
        grd_rdp_render_context_get_encode_session (render_context);
      if (g_hash_table_contains (acquired_resources, encode_session))
        continue;

      release_acquired_resource (renderer, render_context, view_creator);
      g_hash_table_insert (acquired_resources, encode_session, rdp_frame);
      g_hash_table_iter_steal (&iter);

      if (!encode_image_views (renderer, rdp_frame))
        return FALSE;
    }

  return TRUE;
}

static void
on_view_created (GrdRdpFrame *rdp_frame,
                 GError      *error)
{
  GrdRdpRenderer *renderer = grd_rdp_frame_get_renderer (rdp_frame);

  if (!grd_rdp_frame_has_valid_view (rdp_frame))
    g_warning ("[RDP] Failed to create image view: %s", error->message);

  g_mutex_lock (&renderer->view_creations_mutex);
  g_hash_table_add (renderer->finished_view_creations, rdp_frame);

  if (!grd_rdp_frame_has_valid_view (rdp_frame))
    renderer->graphics_subsystem_failed = TRUE;
  g_mutex_unlock (&renderer->view_creations_mutex);

  g_source_set_ready_time (renderer->surface_render_source, 0);
}

static gboolean
maybe_start_view_creations (GrdRdpRenderer *renderer)
{
  GrdRdpFrame *rdp_frame = NULL;
  GHashTableIter iter;

  if (renderer->stop_rendering)
    return TRUE;

  g_hash_table_iter_init (&iter, renderer->queued_frames);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &rdp_frame))
    {
      GHashTable *acquired_resources = NULL;
      GrdRdpRenderContext *render_context;
      GrdRdpViewCreator *view_creator;
      g_autoptr (GError) error = NULL;

      render_context = grd_rdp_frame_get_render_context (rdp_frame);
      if (!g_hash_table_lookup_extended (renderer->render_resource_mappings,
                                         render_context,
                                         NULL, (gpointer *) &acquired_resources))
        g_assert_not_reached ();

      view_creator = grd_rdp_render_context_get_view_creator (render_context);
      if (g_hash_table_contains (acquired_resources, view_creator))
        continue;

      grd_rdp_frame_notify_picked_up (rdp_frame);

      /* There is no resource to release here (no predecessor) */
      g_hash_table_insert (acquired_resources, view_creator, rdp_frame);
      g_hash_table_iter_steal (&iter);

      if (!grd_rdp_view_creator_create_view (view_creator, rdp_frame,
                                             on_view_created, &error))
        {
          g_warning ("[RDP] Failed to create view: %s", error->message);
          grd_rdp_frame_free (rdp_frame);
          return FALSE;
        }
    }

  return TRUE;
}

static void
submit_rendered_frames (GrdRdpRenderer *renderer,
                        GList          *frames)
{
  GList *l;

  for (l = frames; l; l = l->next)
    {
      GrdRdpFrame *rdp_frame = l->data;

      grd_rdp_graphics_pipeline_submit_frame (renderer->graphics_pipeline,
                                              rdp_frame);
      grd_rdp_frame_notify_frame_submission (rdp_frame);
    }
}

static void
release_bitstreams (gpointer data,
                    gpointer user_data)
{
  GrdRdpRenderer *renderer = user_data;
  GrdRdpFrame *rdp_frame = data;
  GList *bitstreams = grd_rdp_frame_get_bitstreams (rdp_frame);
  g_autoptr (GError) error = NULL;
  GList *l;

  for (l = bitstreams; l; l = l->next)
    {
      GrdRdpRenderContext *render_context =
        grd_rdp_frame_get_render_context (rdp_frame);
      GrdEncodeSession *encode_session =
        grd_rdp_render_context_get_encode_session (render_context);
      GrdBitstream *bitstream = l->data;

      if (!grd_encode_session_unlock_bitstream (encode_session, bitstream,
                                                &error))
        {
          g_warning ("[RDP] Renderer: Failed to unlock bitstream %s",
                     error->message);
          handle_graphics_subsystem_failure (renderer);
        }
    }
  g_list_free (bitstreams);

  grd_rdp_frame_set_bitstreams (rdp_frame, NULL);
}

static void
clear_pending_frames (GrdRdpRenderer *renderer)
{
  GrdRdpFrame *rdp_frame = NULL;
  GHashTableIter iter;

  g_hash_table_remove_all (renderer->queued_frames);

  g_mutex_lock (&renderer->view_creations_mutex);
  g_hash_table_iter_init (&iter, renderer->finished_view_creations);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &rdp_frame))
    {
      GrdRdpRenderContext *render_context =
        grd_rdp_frame_get_render_context (rdp_frame);
      GrdRdpViewCreator *view_creator =
        grd_rdp_render_context_get_view_creator (render_context);

      release_acquired_resource (renderer, render_context, view_creator);
      g_hash_table_iter_remove (&iter);
    }
  g_mutex_unlock (&renderer->view_creations_mutex);

  g_mutex_lock (&renderer->frame_encodings_mutex);
  g_hash_table_iter_init (&iter, renderer->finished_frame_encodings);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &rdp_frame))
    {
      GrdRdpRenderContext *render_context =
        grd_rdp_frame_get_render_context (rdp_frame);
      GrdEncodeSession *encode_session =
        grd_rdp_render_context_get_encode_session (render_context);

      release_acquired_resource (renderer, render_context, encode_session);
      release_bitstreams (rdp_frame, renderer);
      g_hash_table_iter_remove (&iter);
    }
  g_mutex_unlock (&renderer->frame_encodings_mutex);
}

static gboolean
render_surfaces (gpointer user_data)
{
  GrdRdpRenderer *renderer = user_data;
  GList *finished_frames;

  finished_frames = fetch_rendered_frames (renderer);

  if (!maybe_start_encodings (renderer) ||
      !maybe_start_view_creations (renderer))
    handle_graphics_subsystem_failure (renderer);

  if (!renderer->stop_rendering)
    submit_rendered_frames (renderer, finished_frames);
  else
    clear_pending_frames (renderer);

  g_list_foreach (finished_frames, release_bitstreams, renderer);
  g_clear_list (&finished_frames, (GDestroyNotify) grd_rdp_frame_free);

  return G_SOURCE_CONTINUE;
}

static gboolean
source_dispatch (GSource     *source,
                 GSourceFunc  callback,
                 gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs source_funcs =
{
  .dispatch = source_dispatch,
};

static void
grd_rdp_renderer_init (GrdRdpRenderer *renderer)
{
  GSource *surface_disposal_source;
  GSource *surface_render_source;

  renderer->surface_renderer_table = g_hash_table_new (NULL, NULL);
  renderer->render_context_table = g_hash_table_new_full (NULL, NULL,
                                                          NULL, g_object_unref);
  renderer->acquired_render_contexts = g_hash_table_new_full (NULL, NULL,
                                                              NULL, g_free);
  renderer->render_resource_mappings =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) g_hash_table_unref);
  renderer->disposal_queue = g_async_queue_new ();

  renderer->queued_frames =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) grd_rdp_frame_free);
  renderer->finished_view_creations =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) grd_rdp_frame_free);
  renderer->finished_frame_encodings =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) grd_rdp_frame_free);

  g_mutex_init (&renderer->surface_renderers_mutex);
  g_mutex_init (&renderer->inhibition_mutex);
  g_cond_init (&renderer->stop_rendering_cond);

  g_mutex_init (&renderer->view_creations_mutex);
  g_mutex_init (&renderer->frame_encodings_mutex);

  renderer->graphics_context = g_main_context_new ();

  surface_disposal_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (surface_disposal_source, dispose_surfaces,
                         renderer, NULL);
  g_source_set_ready_time (surface_disposal_source, -1);
  g_source_attach (surface_disposal_source, renderer->graphics_context);
  renderer->surface_disposal_source = surface_disposal_source;

  surface_render_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (surface_render_source, render_surfaces,
                         renderer, NULL);
  g_source_set_ready_time (surface_render_source, -1);
  g_source_attach (surface_render_source, renderer->graphics_context);
  renderer->surface_render_source = surface_render_source;
}

static void
grd_rdp_renderer_class_init (GrdRdpRendererClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_renderer_dispose;
  object_class->finalize = grd_rdp_renderer_finalize;

  signals[INHIBITION_DONE] = g_signal_new ("inhibition-done",
                                           G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST,
                                           0,
                                           NULL, NULL, NULL,
                                           G_TYPE_NONE, 0);
  signals[GFX_INITABLE] = g_signal_new ("gfx-initable",
                                        G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST,
                                        0,
                                        NULL, NULL, NULL,
                                        G_TYPE_NONE, 0);
}
