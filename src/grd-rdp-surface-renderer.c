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
#include "grd-rdp-frame.h"
#include "grd-rdp-legacy-buffer.h"
#include "grd-rdp-pw-buffer.h"
#include "grd-rdp-render-context.h"
#include "grd-rdp-renderer.h"
#include "grd-rdp-session-metrics.h"
#include "grd-rdp-surface.h"
#include "grd-session-rdp.h"

#define FRAME_UPGRADE_DELAY_US (60 * 1000)
#define UNLIMITED_FRAME_SLOTS (UINT32_MAX)
/*
 * The value of the transition time is based upon testing. During times where
 * a lot of frames are submitted, the frame-controller could quickly alternate
 * between throttling and no-throttling. In these situations, the client is
 * ready for a frame containing only a main view, but not for a dual view.
 */
#define TRANSITION_TIME_US (200 * 1000)

typedef struct
{
  GrdRdpSurfaceRenderer *surface_renderer;

  GrdRdpBuffer *current_buffer;
  GrdRdpBuffer *last_buffer;
} GrdRdpFrameContext;

struct _GrdRdpSurfaceRenderer
{
  GObject parent;

  GrdRdpSurface *rdp_surface;
  GrdRdpRenderer *renderer;
  GrdSessionRdp *session_rdp;
  GrdVkDevice *vk_device;

  uint32_t refresh_rate;

  GSource *buffer_unref_source;
  GAsyncQueue *unref_queue;

  GSource *render_source;
  gboolean pending_render_context_reset;

  GSource *frame_upgrade_source;
  gboolean pending_frame_upgrade;

  GSource *trigger_frame_upgrade_source;

  gboolean graphics_subsystem_failed;

  uint32_t total_frame_slots;
  GHashTable *assigned_frame_slots;
  int64_t unthrottled_since_us;

  GMutex render_mutex;
  GrdRdpBuffer *pending_buffer;
  GrdRdpBuffer *last_buffer;
  GHashTable *acquired_buffers;

  GHashTable *registered_buffers;
  GrdRdpBufferInfo *rdp_buffer_info;
};

G_DEFINE_TYPE (GrdRdpSurfaceRenderer, grd_rdp_surface_renderer, G_TYPE_OBJECT)

uint32_t
grd_rdp_surface_renderer_get_refresh_rate (GrdRdpSurfaceRenderer *surface_renderer)
{
  return surface_renderer->refresh_rate;
}

GrdRdpBufferInfo *
grd_rdp_surface_renderer_get_buffer_info (GrdRdpSurfaceRenderer *surface_renderer)
{
  return surface_renderer->rdp_buffer_info;
}

uint32_t
grd_rdp_surface_renderer_get_total_frame_slots (GrdRdpSurfaceRenderer *surface_renderer)
{
  return surface_renderer->total_frame_slots;
}

static void
trigger_frame_upgrade_schedule (GrdRdpSurfaceRenderer *surface_renderer)
{
  g_source_set_ready_time (surface_renderer->trigger_frame_upgrade_source, 0);
}

void
grd_rdp_surface_renderer_update_total_frame_slots (GrdRdpSurfaceRenderer *surface_renderer,
                                                   uint32_t               total_frame_slots)
{
  uint32_t old_slot_count = surface_renderer->total_frame_slots;

  if (old_slot_count < UNLIMITED_FRAME_SLOTS &&
      total_frame_slots == UNLIMITED_FRAME_SLOTS)
    surface_renderer->unthrottled_since_us = g_get_monotonic_time ();

  surface_renderer->total_frame_slots = total_frame_slots;
  if (old_slot_count < surface_renderer->total_frame_slots)
    grd_rdp_surface_renderer_trigger_render_source (surface_renderer);

  if (old_slot_count < UNLIMITED_FRAME_SLOTS &&
      surface_renderer->total_frame_slots == UNLIMITED_FRAME_SLOTS)
    trigger_frame_upgrade_schedule (surface_renderer);
}

void
grd_rdp_surface_renderer_notify_frame_upgrade_state (GrdRdpSurfaceRenderer *surface_renderer,
                                                     gboolean               can_upgrade_frame)
{
  surface_renderer->pending_frame_upgrade = can_upgrade_frame;
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
                     uint32_t        drm_format,
                     uint64_t        drm_format_modifier)
{
  GrdRdpBufferType buffer_type =
    grd_rdp_pw_buffer_get_buffer_type (rdp_pw_buffer);
  GrdRdpBufferInfo *rdp_buffer_info;

  rdp_buffer_info = g_new0 (GrdRdpBufferInfo, 1);
  rdp_buffer_info->buffer_type = buffer_type;
  rdp_buffer_info->drm_format = drm_format;
  rdp_buffer_info->drm_format_modifier = drm_format_modifier;

  return rdp_buffer_info;
}

gboolean
grd_rdp_surface_renderer_register_pw_buffer (GrdRdpSurfaceRenderer  *surface_renderer,
                                             GrdRdpPwBuffer         *rdp_pw_buffer,
                                             uint32_t                drm_format,
                                             uint64_t                drm_format_modifier,
                                             GError                **error)
{
  GrdRdpSurface *rdp_surface = surface_renderer->rdp_surface;
  g_autofree GrdRdpBufferInfo *rdp_buffer_info = NULL;
  GrdRdpBuffer *rdp_buffer;

  if (!is_buffer_combination_valid (surface_renderer, rdp_pw_buffer, error))
    return FALSE;

  rdp_buffer_info = rdp_buffer_info_new (rdp_pw_buffer, drm_format,
                                         drm_format_modifier);

  rdp_buffer = grd_rdp_buffer_new (rdp_pw_buffer, rdp_buffer_info, rdp_surface,
                                   surface_renderer->vk_device, error);
  if (!rdp_buffer)
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
    surface_renderer->rdp_buffer_info = g_steal_pointer (&rdp_buffer_info);

  g_hash_table_insert (surface_renderer->registered_buffers,
                       rdp_pw_buffer, rdp_buffer);

  return TRUE;
}

static void
release_pw_buffer (GrdRdpBuffer *rdp_buffer)
{
  GrdRdpPwBuffer *rdp_pw_buffer = grd_rdp_buffer_get_rdp_pw_buffer (rdp_buffer);

  grd_rdp_pw_buffer_queue_pw_buffer (rdp_pw_buffer);
}

void
grd_rdp_surface_renderer_unregister_pw_buffer (GrdRdpSurfaceRenderer *surface_renderer,
                                               GrdRdpPwBuffer        *rdp_pw_buffer)
{
  GrdRdpBuffer *rdp_buffer = NULL;
  GrdRdpBuffer *buffer_to_release = NULL;

  if (!g_hash_table_lookup_extended (surface_renderer->registered_buffers,
                                     rdp_pw_buffer,
                                     NULL, (gpointer *) &rdp_buffer))
    g_assert_not_reached ();

  /* Ensure buffer cannot be acquired anymore */
  g_mutex_lock (&surface_renderer->render_mutex);
  if (surface_renderer->pending_buffer || surface_renderer->last_buffer)
    g_assert (surface_renderer->pending_buffer != surface_renderer->last_buffer);

  if (surface_renderer->pending_buffer == rdp_buffer)
    buffer_to_release = g_steal_pointer (&surface_renderer->pending_buffer);
  if (surface_renderer->last_buffer == rdp_buffer)
    buffer_to_release = g_steal_pointer (&surface_renderer->last_buffer);

  grd_rdp_buffer_mark_for_removal (rdp_buffer);
  g_mutex_unlock (&surface_renderer->render_mutex);

  /* Ensure buffer lock is released and PipeWire buffer queued again */
  grd_rdp_pw_buffer_ensure_unlocked (rdp_pw_buffer);
  g_clear_pointer (&buffer_to_release, release_pw_buffer);

  g_hash_table_remove (surface_renderer->registered_buffers, rdp_pw_buffer);
}

void
grd_rdp_surface_renderer_submit_buffer (GrdRdpSurfaceRenderer *surface_renderer,
                                        GrdRdpPwBuffer        *rdp_pw_buffer)
{
  GrdRdpBuffer *rdp_buffer = NULL;

  if (!g_hash_table_lookup_extended (surface_renderer->registered_buffers,
                                     rdp_pw_buffer,
                                     NULL, (gpointer *) &rdp_buffer))
    g_assert_not_reached ();

  g_mutex_lock (&surface_renderer->render_mutex);
  g_clear_pointer (&surface_renderer->pending_buffer, release_pw_buffer);

  surface_renderer->pending_buffer = rdp_buffer;
  g_mutex_unlock (&surface_renderer->render_mutex);

  grd_rdp_surface_renderer_trigger_render_source (surface_renderer);
}

void
grd_rdp_surface_renderer_submit_legacy_buffer (GrdRdpSurfaceRenderer *surface_renderer,
                                               GrdRdpLegacyBuffer    *buffer)
{
  GrdRdpSurface *rdp_surface = surface_renderer->rdp_surface;

  g_mutex_lock (&surface_renderer->render_mutex);
  g_clear_pointer (&rdp_surface->pending_framebuffer, grd_rdp_legacy_buffer_release);

  rdp_surface->pending_framebuffer = buffer;
  g_mutex_unlock (&surface_renderer->render_mutex);

  grd_rdp_surface_renderer_trigger_render_source (surface_renderer);
}

void
grd_rdp_surface_renderer_invalidate_surface (GrdRdpSurfaceRenderer *surface_renderer)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&surface_renderer->render_mutex);
  grd_rdp_surface_renderer_invalidate_surface_unlocked (surface_renderer);
}

void
grd_rdp_surface_renderer_invalidate_surface_unlocked (GrdRdpSurfaceRenderer *surface_renderer)
{
  g_assert (g_hash_table_size (surface_renderer->acquired_buffers) == 0);

  if (!surface_renderer->pending_buffer)
    {
      surface_renderer->pending_buffer =
        g_steal_pointer (&surface_renderer->last_buffer);
    }
  g_clear_pointer (&surface_renderer->last_buffer, release_pw_buffer);
}

void
grd_rdp_surface_renderer_trigger_render_source (GrdRdpSurfaceRenderer *surface_renderer)
{
  g_source_set_ready_time (surface_renderer->render_source, 0);

  trigger_frame_upgrade_schedule (surface_renderer);
}

void
grd_rdp_surface_renderer_reset (GrdRdpSurfaceRenderer *surface_renderer)
{
  GrdRdpSurface *rdp_surface = surface_renderer->rdp_surface;

  g_mutex_lock (&surface_renderer->render_mutex);
  g_clear_pointer (&rdp_surface->pending_framebuffer, grd_rdp_legacy_buffer_release);
  g_mutex_unlock (&surface_renderer->render_mutex);
}

static GrdRdpBuffer *
rdp_buffer_ref (GrdRdpSurfaceRenderer *surface_renderer,
                GrdRdpBuffer          *rdp_buffer)
{
  GrdRdpPwBuffer *rdp_pw_buffer = grd_rdp_buffer_get_rdp_pw_buffer (rdp_buffer);
  uint32_t *ref_count = NULL;

  if (g_hash_table_lookup_extended (surface_renderer->acquired_buffers,
                                    rdp_buffer,
                                    NULL, (gpointer *) &ref_count))
    {
      g_assert (*ref_count > 0);
      ++(*ref_count);

      return rdp_buffer;
    }

  ref_count = g_new0 (uint32_t, 1);
  *ref_count = 1;

  grd_rdp_pw_buffer_acquire_lock (rdp_pw_buffer);
  g_hash_table_insert (surface_renderer->acquired_buffers,
                       rdp_buffer, ref_count);

  return rdp_buffer;
}

static void
rdp_buffer_unref (GrdRdpSurfaceRenderer *surface_renderer,
                  GrdRdpBuffer          *rdp_buffer)
{
  GrdRdpPwBuffer *rdp_pw_buffer = grd_rdp_buffer_get_rdp_pw_buffer (rdp_buffer);
  uint32_t *ref_count = NULL;

  if (!g_hash_table_lookup_extended (surface_renderer->acquired_buffers,
                                     rdp_buffer,
                                     NULL, (gpointer *) &ref_count))
    g_assert_not_reached ();

  g_assert (*ref_count > 0);
  --(*ref_count);

  if (*ref_count == 0)
    {
      g_assert (surface_renderer->pending_buffer != rdp_buffer);

      /*
       * unregister_pw_buffer () already takes care of the PW buffer release
       * here. In such case, the second condition
       * (surface_renderer->last_buffer != rdp_buffer) is always true.
       */
      if (!grd_rdp_buffer_is_marked_for_removal (rdp_buffer) &&
          surface_renderer->last_buffer != rdp_buffer)
        release_pw_buffer (rdp_buffer);

      g_hash_table_remove (surface_renderer->acquired_buffers, rdp_buffer);
      grd_rdp_pw_buffer_maybe_release_lock (rdp_pw_buffer);
    }
}

static gboolean
unref_buffers (gpointer user_data)
{
  GrdRdpSurfaceRenderer *surface_renderer = user_data;
  GrdRdpBuffer *rdp_buffer;

  while ((rdp_buffer = g_async_queue_try_pop (surface_renderer->unref_queue)))
    {
      g_mutex_lock (&surface_renderer->render_mutex);
      g_assert (surface_renderer->pending_buffer != rdp_buffer);

      rdp_buffer_unref (surface_renderer, rdp_buffer);
      g_mutex_unlock (&surface_renderer->render_mutex);
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
can_prepare_new_frame (GrdRdpSurfaceRenderer *surface_renderer)
{
  uint32_t total_frame_slots = surface_renderer->total_frame_slots;
  uint32_t used_frame_slots;

  used_frame_slots = g_hash_table_size (surface_renderer->assigned_frame_slots);

  return total_frame_slots > used_frame_slots;
}

static void
on_frame_picked_up (GrdRdpFrame *rdp_frame,
                    gpointer     user_data)
{
  GrdRdpFrameContext *frame_context = user_data;
  GrdRdpSurfaceRenderer *surface_renderer = frame_context->surface_renderer;
  GrdRdpBuffer *current_buffer = frame_context->current_buffer;

  g_hash_table_add (surface_renderer->assigned_frame_slots, rdp_frame);

  if (!current_buffer)
    return;

  g_mutex_lock (&surface_renderer->render_mutex);
  if (!grd_rdp_buffer_is_marked_for_removal (current_buffer))
    surface_renderer->last_buffer = current_buffer;
  else
    surface_renderer->last_buffer = NULL;
  g_mutex_unlock (&surface_renderer->render_mutex);
}

static void
on_view_finalization (GrdRdpFrame *rdp_frame,
                      gpointer     user_data)
{
  GrdRdpFrameContext *frame_context = user_data;
  GrdRdpSurfaceRenderer *surface_renderer = frame_context->surface_renderer;
  GrdRdpBuffer *current_buffer = frame_context->current_buffer;
  GrdRdpBuffer *last_buffer = frame_context->last_buffer;

  g_async_queue_push (surface_renderer->unref_queue, current_buffer);
  if (last_buffer)
    g_async_queue_push (surface_renderer->unref_queue, last_buffer);

  g_source_set_ready_time (surface_renderer->buffer_unref_source, 0);
}

static void
on_frame_submission (GrdRdpFrame *rdp_frame,
                     gpointer     user_data)
{
  GrdRdpFrameContext *frame_context = user_data;
  GrdRdpSurfaceRenderer *surface_renderer = frame_context->surface_renderer;
  GrdRdpSurface *rdp_surface = surface_renderer->rdp_surface;
  GrdRdpSessionMetrics *session_metrics =
    grd_session_rdp_get_session_metrics (surface_renderer->session_rdp);

  grd_rdp_session_metrics_notify_frame_transmission (session_metrics,
                                                     rdp_surface);
}

static void
reset_frame_upgrade_schedule (GrdRdpSurfaceRenderer *surface_renderer)
{
  g_source_set_ready_time (surface_renderer->frame_upgrade_source, -1);

  trigger_frame_upgrade_schedule (surface_renderer);
}

static void
on_frame_finalization (GrdRdpFrame *rdp_frame,
                       gpointer     user_data)
{
  GrdRdpFrameContext *frame_context = user_data;
  GrdRdpSurfaceRenderer *surface_renderer = frame_context->surface_renderer;

  g_hash_table_remove (surface_renderer->assigned_frame_slots, rdp_frame);
  grd_rdp_surface_renderer_trigger_render_source (surface_renderer);

  reset_frame_upgrade_schedule (surface_renderer);
}

static gboolean
should_avoid_auxiliary_frame (GrdRdpSurfaceRenderer *surface_renderer)
{
  int64_t current_time_us;

  current_time_us = g_get_monotonic_time ();

  return g_hash_table_size (surface_renderer->assigned_frame_slots) > 0 ||
         surface_renderer->total_frame_slots < UNLIMITED_FRAME_SLOTS ||
         current_time_us - surface_renderer->unthrottled_since_us < TRANSITION_TIME_US;
}

static void
maybe_downgrade_view_type (GrdRdpSurfaceRenderer *surface_renderer,
                           GrdRdpRenderContext   *render_context,
                           GrdRdpFrame           *rdp_frame)
{
  GrdRdpFrameViewType view_type;

  view_type = grd_rdp_frame_get_avc_view_type (rdp_frame);
  if (view_type != GRD_RDP_FRAME_VIEW_TYPE_DUAL)
    return;

  if (should_avoid_auxiliary_frame (surface_renderer) ||
      grd_rdp_render_context_should_avoid_dual_frame (render_context))
    grd_rdp_frame_set_avc_view_type (rdp_frame, GRD_RDP_FRAME_VIEW_TYPE_MAIN);
}

static void
handle_pending_buffer (GrdRdpSurfaceRenderer *surface_renderer,
                       GrdRdpRenderContext   *render_context)
{
  GrdRdpRenderer *renderer = surface_renderer->renderer;
  GrdRdpFrameContext *frame_context;
  GrdRdpBuffer *current_buffer = NULL;
  GrdRdpBuffer *last_buffer = NULL;
  GrdRdpFrame *rdp_frame;

  frame_context = g_new0 (GrdRdpFrameContext, 1);
  frame_context->surface_renderer = surface_renderer;

  current_buffer = g_steal_pointer (&surface_renderer->pending_buffer);
  frame_context->current_buffer = rdp_buffer_ref (surface_renderer,
                                                  current_buffer);

  if (surface_renderer->last_buffer)
    {
      last_buffer = surface_renderer->last_buffer;
      frame_context->last_buffer = rdp_buffer_ref (surface_renderer,
                                                   last_buffer);
    }

  rdp_frame = grd_rdp_frame_new (render_context,
                                 current_buffer, last_buffer,
                                 on_frame_picked_up, on_view_finalization,
                                 on_frame_submission, on_frame_finalization,
                                 frame_context, g_free);
  maybe_downgrade_view_type (surface_renderer, render_context, rdp_frame);

  grd_rdp_renderer_submit_frame (renderer, render_context, rdp_frame);
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
  GrdRdpLegacyBuffer *buffer;

  buffer = g_steal_pointer (&rdp_surface->pending_framebuffer);
  if (!grd_rdp_damage_detector_submit_new_framebuffer (rdp_surface->detector,
                                                       buffer))
    {
      grd_rdp_legacy_buffer_release (buffer);
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
  if (!surface_renderer->pending_buffer &&
      !rdp_surface->pending_framebuffer)
    return G_SOURCE_CONTINUE;

  g_assert (!surface_renderer->pending_buffer ||
            !rdp_surface->pending_framebuffer);

  g_source_set_ready_time (surface_renderer->frame_upgrade_source, -1);

  if (!can_prepare_new_frame (surface_renderer))
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

  if (surface_renderer->pending_buffer)
    {
      handle_pending_buffer (surface_renderer, render_context);
    }
  else if (rdp_surface->pending_framebuffer) /* Legacy render handling */
    {
      maybe_encode_pending_frame (surface_renderer, render_context);
      grd_rdp_renderer_release_render_context (renderer, render_context);
    }
  else
    {
      g_assert_not_reached ();
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
should_retry_frame_upgrade (GrdRdpSurfaceRenderer *surface_renderer)
{
  /*
   * should_avoid_auxiliary_frame() returned true. If the following conditions
   * are true too, then the transition period from throttled to unthrottled is
   * not over yet.
   */
  return g_hash_table_size (surface_renderer->assigned_frame_slots) == 0 &&
         surface_renderer->total_frame_slots == UNLIMITED_FRAME_SLOTS;
}

static void
upgrade_frame (GrdRdpSurfaceRenderer *surface_renderer,
               GrdRdpRenderContext   *render_context)
{
  GrdRdpRenderer *renderer = surface_renderer->renderer;
  GrdRdpFrameContext *frame_context;
  GrdRdpFrame *rdp_frame;

  frame_context = g_new0 (GrdRdpFrameContext, 1);
  frame_context->surface_renderer = surface_renderer;

  rdp_frame = grd_rdp_frame_new (render_context, NULL, NULL,
                                 on_frame_picked_up, NULL,
                                 on_frame_submission, on_frame_finalization,
                                 frame_context, g_free);
  grd_rdp_renderer_submit_frame (renderer, render_context, rdp_frame);
}

static gboolean
maybe_upgrade_frame (gpointer user_data)
{
  GrdRdpSurfaceRenderer *surface_renderer = user_data;
  GrdRdpRenderer *renderer = surface_renderer->renderer;
  GrdRdpSurface *rdp_surface = surface_renderer->rdp_surface;
  GrdRdpAcquireContextFlags acquire_flags;
  GrdRdpRenderContext *render_context;
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&surface_renderer->render_mutex);
  if (surface_renderer->pending_buffer)
    return G_SOURCE_CONTINUE;

  g_clear_pointer (&locker, g_mutex_locker_free);

  if (!surface_renderer->pending_frame_upgrade)
    return G_SOURCE_CONTINUE;

  if (!can_prepare_new_frame (surface_renderer))
    return G_SOURCE_CONTINUE;

  if (should_avoid_auxiliary_frame (surface_renderer))
    {
      if (should_retry_frame_upgrade (surface_renderer))
        trigger_frame_upgrade_schedule (surface_renderer);

      return G_SOURCE_CONTINUE;
    }

  acquire_flags = GRD_RDP_ACQUIRE_CONTEXT_FLAG_RETAIN_OR_NULL;

  render_context =
    grd_rdp_renderer_try_acquire_render_context (renderer, rdp_surface,
                                                 acquire_flags);
  if (!render_context)
    return G_SOURCE_CONTINUE;

  upgrade_frame (surface_renderer, render_context);

  return G_SOURCE_CONTINUE;
}

static gboolean
trigger_frame_upgrade (gpointer user_data)
{
  GrdRdpSurfaceRenderer *surface_renderer = user_data;
  int64_t current_time_us;

  if (g_source_get_ready_time (surface_renderer->frame_upgrade_source) != -1)
    return G_SOURCE_CONTINUE;

  current_time_us = g_source_get_time (surface_renderer->frame_upgrade_source);
  g_source_set_ready_time (surface_renderer->frame_upgrade_source,
                           current_time_us + FRAME_UPGRADE_DELAY_US);

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

GrdRdpSurfaceRenderer *
grd_rdp_surface_renderer_new (GrdRdpSurface  *rdp_surface,
                              GrdRdpRenderer *renderer,
                              GrdSessionRdp  *session_rdp,
                              GrdVkDevice    *vk_device,
                              uint32_t        refresh_rate)
{
  GMainContext *graphics_context =
    grd_rdp_renderer_get_graphics_context (renderer);
  GrdRdpSurfaceRenderer *surface_renderer;
  GSource *buffer_unref_source;
  GSource *render_source;
  GSource *frame_upgrade_source;
  GSource *trigger_frame_upgrade_source;

  surface_renderer = g_object_new (GRD_TYPE_RDP_SURFACE_RENDERER, NULL);
  surface_renderer->rdp_surface = rdp_surface;
  surface_renderer->renderer = renderer;
  surface_renderer->session_rdp = session_rdp;
  surface_renderer->vk_device = vk_device;
  surface_renderer->refresh_rate = refresh_rate;

  buffer_unref_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (buffer_unref_source, unref_buffers,
                         surface_renderer, NULL);
  g_source_set_ready_time (buffer_unref_source, -1);
  g_source_attach (buffer_unref_source, graphics_context);
  surface_renderer->buffer_unref_source = buffer_unref_source;

  render_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (render_source, maybe_render_frame,
                         surface_renderer, NULL);
  g_source_set_ready_time (render_source, -1);
  g_source_attach (render_source, graphics_context);
  surface_renderer->render_source = render_source;

  frame_upgrade_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (frame_upgrade_source, maybe_upgrade_frame,
                         surface_renderer, NULL);
  g_source_set_ready_time (frame_upgrade_source, -1);
  g_source_attach (frame_upgrade_source, graphics_context);
  surface_renderer->frame_upgrade_source = frame_upgrade_source;

  trigger_frame_upgrade_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (trigger_frame_upgrade_source, trigger_frame_upgrade,
                         surface_renderer, NULL);
  g_source_set_ready_time (trigger_frame_upgrade_source, -1);
  g_source_attach (trigger_frame_upgrade_source, graphics_context);
  surface_renderer->trigger_frame_upgrade_source = trigger_frame_upgrade_source;

  return surface_renderer;
}

static void
grd_rdp_surface_renderer_dispose (GObject *object)
{
  GrdRdpSurfaceRenderer *surface_renderer = GRD_RDP_SURFACE_RENDERER (object);

  /*
   * No need to lock the render mutex here, the surface renderer outlives the
   * PipeWire stream.
   */
  if (surface_renderer->acquired_buffers)
    g_assert (g_hash_table_size (surface_renderer->acquired_buffers) == 0);
  if (surface_renderer->assigned_frame_slots)
    g_assert (g_hash_table_size (surface_renderer->assigned_frame_slots) == 0);

  g_assert (g_async_queue_try_pop (surface_renderer->unref_queue) == NULL);

  if (surface_renderer->trigger_frame_upgrade_source)
    {
      g_source_destroy (surface_renderer->trigger_frame_upgrade_source);
      g_clear_pointer (&surface_renderer->trigger_frame_upgrade_source, g_source_unref);
    }
  if (surface_renderer->frame_upgrade_source)
    {
      g_source_destroy (surface_renderer->frame_upgrade_source);
      g_clear_pointer (&surface_renderer->frame_upgrade_source, g_source_unref);
    }
  if (surface_renderer->render_source)
    {
      g_source_destroy (surface_renderer->render_source);
      g_clear_pointer (&surface_renderer->render_source, g_source_unref);
    }
  if (surface_renderer->buffer_unref_source)
    {
      g_source_destroy (surface_renderer->buffer_unref_source);
      g_clear_pointer (&surface_renderer->buffer_unref_source, g_source_unref);
    }

  g_clear_pointer (&surface_renderer->assigned_frame_slots, g_hash_table_unref);
  g_clear_pointer (&surface_renderer->acquired_buffers, g_hash_table_unref);
  g_clear_pointer (&surface_renderer->registered_buffers, g_hash_table_unref);
  g_clear_pointer (&surface_renderer->rdp_buffer_info, g_free);

  G_OBJECT_CLASS (grd_rdp_surface_renderer_parent_class)->dispose (object);
}

static void
grd_rdp_surface_renderer_finalize (GObject *object)
{
  GrdRdpSurfaceRenderer *surface_renderer = GRD_RDP_SURFACE_RENDERER (object);

  g_clear_pointer (&surface_renderer->unref_queue, g_async_queue_unref);

  g_mutex_clear (&surface_renderer->render_mutex);

  G_OBJECT_CLASS (grd_rdp_surface_renderer_parent_class)->finalize (object);
}

static void
grd_rdp_surface_renderer_init (GrdRdpSurfaceRenderer *surface_renderer)
{
  surface_renderer->total_frame_slots = UNLIMITED_FRAME_SLOTS;

  surface_renderer->registered_buffers =
    g_hash_table_new_full (NULL, NULL,
                           NULL, g_object_unref);
  surface_renderer->acquired_buffers =
    g_hash_table_new_full (NULL, NULL,
                           NULL, g_free);
  surface_renderer->assigned_frame_slots =
    g_hash_table_new (NULL, NULL);
  surface_renderer->unref_queue = g_async_queue_new ();

  g_mutex_init (&surface_renderer->render_mutex);
}

static void
grd_rdp_surface_renderer_class_init (GrdRdpSurfaceRendererClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_surface_renderer_dispose;
  object_class->finalize = grd_rdp_surface_renderer_finalize;
}
