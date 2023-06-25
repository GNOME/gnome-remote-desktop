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

#include "grd-rdp-layout-manager.h"

#include "grd-rdp-pipewire-stream.h"
#include "grd-rdp-surface.h"
#include "grd-session-rdp.h"
#include "grd-stream.h"

#define LAYOUT_RECREATION_TIMEOUT_MS 50

typedef enum
{
  UPDATE_STATE_FATAL_ERROR,
  UPDATE_STATE_AWAIT_CONFIG,
  UPDATE_STATE_PREPARE_SURFACES,
  UPDATE_STATE_AWAIT_STREAMS,
  UPDATE_STATE_START_RENDERING,
} UpdateState;

typedef struct
{
  GrdRdpLayoutManager *layout_manager;

  GrdRdpSurface *rdp_surface;
  uint32_t output_origin_x;
  uint32_t output_origin_y;

  GrdRdpVirtualMonitor *virtual_monitor;
  const char *connector;

  GrdStream *stream;
  GrdRdpPipeWireStream *pipewire_stream;
} SurfaceContext;

struct _GrdRdpLayoutManager
{
  GrdRdpStreamOwner parent;

  gboolean has_graphics_pipeline;
  gboolean session_started;

  GMutex state_mutex;
  UpdateState state;

  GrdSessionRdp *session_rdp;
  GrdHwAccelNvidia *hwaccel_nvidia;
  GMainContext *render_context;

  GSource *layout_update_source;
  GHashTable *surface_table;

  GHashTable *pending_streams;

  GSource *surface_disposal_source;
  GAsyncQueue *disposal_queue;

  GMutex monitor_config_mutex;
  GrdRdpMonitorConfig *current_monitor_config;
  GrdRdpMonitorConfig *pending_monitor_config;

  unsigned int layout_destruction_id;
  unsigned int layout_recreation_id;
};

G_DEFINE_TYPE (GrdRdpLayoutManager, grd_rdp_layout_manager,
               GRD_TYPE_RDP_STREAM_OWNER)

static SurfaceContext *
surface_context_new (GrdRdpLayoutManager  *layout_manager,
                     GError              **error)
{
  g_autofree SurfaceContext *surface_context = NULL;

  surface_context = g_new0 (SurfaceContext, 1);
  surface_context->layout_manager = layout_manager;

  surface_context->rdp_surface =
    grd_rdp_surface_new (layout_manager->session_rdp,
                         layout_manager->hwaccel_nvidia,
                         layout_manager->render_context,
                         layout_manager->has_graphics_pipeline ? 60 : 30);
  if (!surface_context->rdp_surface)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create RDP surface");
      return NULL;
    }

  return g_steal_pointer (&surface_context);
}

static void
surface_context_free (SurfaceContext *surface_context)
{
  GrdRdpLayoutManager *layout_manager = surface_context->layout_manager;

  g_clear_object (&surface_context->pipewire_stream);
  if (surface_context->stream)
    {
      uint32_t stream_id = grd_stream_get_stream_id (surface_context->stream);

      grd_stream_disconnect_proxy_signals (surface_context->stream);
      g_clear_object (&surface_context->stream);

      grd_session_rdp_release_stream_id (layout_manager->session_rdp,
                                         stream_id);
    }

  g_async_queue_push (layout_manager->disposal_queue,
                      surface_context->rdp_surface);
  g_source_set_ready_time (layout_manager->surface_disposal_source, 0);

  g_clear_pointer (&surface_context->virtual_monitor, g_free);
  g_free (surface_context);
}

static const char *
update_state_to_string (UpdateState state)
{
  switch (state)
    {
    case UPDATE_STATE_FATAL_ERROR:
      return "FATAL_ERROR";
    case UPDATE_STATE_AWAIT_CONFIG:
      return "AWAIT_CONFIG";
    case UPDATE_STATE_PREPARE_SURFACES:
      return "PREPARE_SURFACES";
    case UPDATE_STATE_AWAIT_STREAMS:
      return "AWAIT_STREAMS";
    case UPDATE_STATE_START_RENDERING:
      return "START_RENDERING";
    }

  g_assert_not_reached ();
}

static void
disconnect_proxy_signals (GrdRdpLayoutManager *layout_manager)
{
  SurfaceContext *surface_context = NULL;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, layout_manager->surface_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &surface_context))
    {
      if (!surface_context->stream)
        continue;

      grd_stream_disconnect_proxy_signals (surface_context->stream);
    }
}

static void
transition_to_state (GrdRdpLayoutManager *layout_manager,
                     UpdateState          state)
{
  g_mutex_lock (&layout_manager->state_mutex);
  g_debug ("[RDP] Layout manager: Updating state from %s to %s",
           update_state_to_string (layout_manager->state),
           update_state_to_string (state));

  g_assert (layout_manager->state != state);
  layout_manager->state = state;
  g_mutex_unlock (&layout_manager->state_mutex);

  switch (state)
    {
    case UPDATE_STATE_FATAL_ERROR:
      disconnect_proxy_signals (layout_manager);
      grd_session_rdp_notify_error (layout_manager->session_rdp,
                                    GRD_SESSION_RDP_ERROR_CLOSE_STACK_ON_DRIVER_FAILURE);
      break;
    case UPDATE_STATE_AWAIT_CONFIG:
      g_debug ("[RDP] Layout manager: Finished applying new monitor config");
      grd_rdp_layout_manager_maybe_trigger_render_sources (layout_manager);
      g_source_set_ready_time (layout_manager->layout_update_source, 0);
      break;
    case UPDATE_STATE_PREPARE_SURFACES:
    case UPDATE_STATE_AWAIT_STREAMS:
      break;
    case UPDATE_STATE_START_RENDERING:
      g_source_set_ready_time (layout_manager->layout_update_source, 0);
      break;
    }
}

static void
on_pipewire_stream_error (GrdRdpPipeWireStream *pipewire_stream,
                          GrdRdpLayoutManager  *layout_manager)
{
  if (layout_manager->state == UPDATE_STATE_FATAL_ERROR)
    return;

  g_warning ("[RDP] PipeWire stream closed due to error. Terminating session");
  transition_to_state (layout_manager, UPDATE_STATE_FATAL_ERROR);
}

static gboolean
is_virtual_stream (GrdRdpLayoutManager  *layout_manager,
                   GrdRdpPipeWireStream *pipewire_stream)
{
  SurfaceContext *surface_context = NULL;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, layout_manager->surface_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &surface_context))
    {
      if (surface_context->pipewire_stream == pipewire_stream)
        return surface_context->virtual_monitor != NULL;
    }

  g_assert_not_reached ();
}

static gboolean
maybe_invoke_layout_recreation (gpointer user_data)
{
  GrdRdpLayoutManager *layout_manager = user_data;

  g_mutex_lock (&layout_manager->monitor_config_mutex);
  if (!layout_manager->pending_monitor_config)
    {
      g_debug ("[RDP] Layout manager: Trying to restore last config.");

      layout_manager->pending_monitor_config =
        g_steal_pointer (&layout_manager->current_monitor_config);

      g_source_set_ready_time (layout_manager->layout_update_source, 0);
    }
  g_mutex_unlock (&layout_manager->monitor_config_mutex);

  layout_manager->layout_recreation_id = 0;

  return G_SOURCE_REMOVE;
}

static gboolean
destroy_layout (gpointer user_data)
{
  GrdRdpLayoutManager *layout_manager = user_data;

  g_debug ("[RDP] Layout manager: Monitor stream closed. "
           "Setting timeout for recreation");

  g_hash_table_remove_all (layout_manager->surface_table);

  g_assert (!layout_manager->layout_recreation_id);
  layout_manager->layout_recreation_id =
    g_timeout_add (LAYOUT_RECREATION_TIMEOUT_MS, maybe_invoke_layout_recreation,
                   layout_manager);

  layout_manager->layout_destruction_id = 0;

  return G_SOURCE_REMOVE;
}

static void
on_pipewire_stream_closed (GrdRdpPipeWireStream *pipewire_stream,
                           GrdRdpLayoutManager  *layout_manager)
{
  if (layout_manager->state == UPDATE_STATE_FATAL_ERROR)
    return;

  if (is_virtual_stream (layout_manager, pipewire_stream))
    return;

  if (layout_manager->layout_destruction_id)
    return;

  layout_manager->layout_destruction_id = g_idle_add (destroy_layout,
                                                      layout_manager);
}

static void
on_stream_ready (GrdStream           *stream,
                 GrdRdpLayoutManager *layout_manager)
{
  uint32_t stream_id = grd_stream_get_stream_id (stream);
  uint32_t pipewire_node_id = grd_stream_get_pipewire_node_id (stream);
  SurfaceContext *surface_context = NULL;
  g_autoptr (GError) error = NULL;

  g_assert (layout_manager->state == UPDATE_STATE_FATAL_ERROR ||
            layout_manager->state == UPDATE_STATE_AWAIT_STREAMS);

  if (layout_manager->state == UPDATE_STATE_FATAL_ERROR)
    return;

  if (!g_hash_table_lookup_extended (layout_manager->surface_table,
                                     GUINT_TO_POINTER (stream_id),
                                     NULL, (gpointer *) &surface_context))
    g_assert_not_reached ();

  g_debug ("[RDP] Layout manager: Creating PipeWire stream for node id %u",
           pipewire_node_id);
  surface_context->pipewire_stream =
    grd_rdp_pipewire_stream_new (layout_manager->session_rdp,
                                 layout_manager->hwaccel_nvidia,
                                 layout_manager->render_context,
                                 surface_context->rdp_surface,
                                 surface_context->virtual_monitor,
                                 pipewire_node_id,
                                 &error);
  if (!surface_context->pipewire_stream)
    {
      g_warning ("[RDP] Failed to establish PipeWire stream: %s",
                 error->message);
      transition_to_state (layout_manager, UPDATE_STATE_FATAL_ERROR);
      return;
    }

  g_signal_connect (surface_context->pipewire_stream, "error",
                    G_CALLBACK (on_pipewire_stream_error),
                    layout_manager);
  g_signal_connect (surface_context->pipewire_stream, "closed",
                    G_CALLBACK (on_pipewire_stream_closed),
                    layout_manager);

  g_hash_table_remove (layout_manager->pending_streams,
                       GUINT_TO_POINTER (stream_id));
  if (g_hash_table_size (layout_manager->pending_streams) == 0)
    transition_to_state (layout_manager, UPDATE_STATE_START_RENDERING);
}

static void
grd_rdp_layout_manager_on_stream_created (GrdRdpStreamOwner *stream_owner,
                                          uint32_t           stream_id,
                                          GrdStream         *stream)
{
  GrdRdpLayoutManager *layout_manager = GRD_RDP_LAYOUT_MANAGER (stream_owner);
  SurfaceContext *surface_context = NULL;

  g_assert (layout_manager->state == UPDATE_STATE_FATAL_ERROR ||
            layout_manager->state == UPDATE_STATE_AWAIT_STREAMS);

  if (!g_hash_table_lookup_extended (layout_manager->surface_table,
                                     GUINT_TO_POINTER (stream_id),
                                     NULL, (gpointer *) &surface_context))
    g_assert_not_reached ();

  g_assert (!surface_context->stream);
  surface_context->stream = stream;

  if (layout_manager->state == UPDATE_STATE_FATAL_ERROR)
    return;

  g_signal_connect (stream, "ready", G_CALLBACK (on_stream_ready),
                    layout_manager);
}

void
grd_rdp_layout_manager_get_current_layout (GrdRdpLayoutManager  *layout_manager,
                                           MONITOR_DEF         **monitors,
                                           uint32_t             *n_monitors)
{
  SurfaceContext *surface_context = NULL;
  GHashTableIter iter;
  uint32_t i = 0;

  g_assert (layout_manager->state == UPDATE_STATE_AWAIT_CONFIG);

  *n_monitors = g_hash_table_size (layout_manager->surface_table);
  g_assert (*n_monitors > 0);
  *monitors = g_new0 (MONITOR_DEF, *n_monitors);

  g_hash_table_iter_init (&iter, layout_manager->surface_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &surface_context))
    {
      GrdRdpSurface *rdp_surface = surface_context->rdp_surface;
      int32_t surface_width = grd_rdp_surface_get_width (rdp_surface);
      int32_t surface_height = grd_rdp_surface_get_height (rdp_surface);

      if (surface_context->virtual_monitor)
        {
          (*monitors)[i].left = surface_context->virtual_monitor->pos_x;
          (*monitors)[i].top = surface_context->virtual_monitor->pos_y;
        }
      if (surface_context->connector)
        {
          g_assert (*n_monitors == 1);

          (*monitors)[i].left = 0;
          (*monitors)[i].top = 0;
        }

      (*monitors)[i].right = (*monitors)[i].left + surface_width - 1;
      (*monitors)[i].bottom = (*monitors)[i].top + surface_height - 1;

      if (surface_context->virtual_monitor &&
          surface_context->virtual_monitor->is_primary)
        (*monitors)[i].flags = MONITOR_PRIMARY;
      if (surface_context->connector)
        {
          g_assert (*n_monitors == 1);
          (*monitors)[i].flags = MONITOR_PRIMARY;
        }

      ++i;
    }
  g_assert (i == *n_monitors);
}

void
grd_rdp_layout_manager_notify_session_started (GrdRdpLayoutManager *layout_manager,
                                               gboolean             has_graphics_pipeline)
{
  layout_manager->has_graphics_pipeline = has_graphics_pipeline;
  layout_manager->session_started = TRUE;

  g_source_set_ready_time (layout_manager->layout_update_source, 0);
}

void
grd_rdp_layout_manager_submit_new_monitor_config (GrdRdpLayoutManager *layout_manager,
                                                  GrdRdpMonitorConfig *monitor_config)
{
  g_assert (monitor_config);
  if (!monitor_config->is_virtual)
    g_assert (monitor_config->monitor_count == 1);

  g_mutex_lock (&layout_manager->monitor_config_mutex);
  g_clear_pointer (&layout_manager->pending_monitor_config, grd_rdp_monitor_config_free);

  layout_manager->pending_monitor_config = monitor_config;
  g_mutex_unlock (&layout_manager->monitor_config_mutex);

  g_source_set_ready_time (layout_manager->layout_update_source, 0);
}

void
grd_rdp_layout_manager_maybe_trigger_render_sources (GrdRdpLayoutManager *layout_manager)
{
  g_autoptr (GMutexLocker) locker = NULL;
  SurfaceContext *surface_context = NULL;
  GHashTableIter iter;

  locker = g_mutex_locker_new (&layout_manager->state_mutex);
  if (layout_manager->state != UPDATE_STATE_AWAIT_CONFIG)
    return;

  g_hash_table_iter_init (&iter, layout_manager->surface_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &surface_context))
    grd_rdp_surface_trigger_render_source (surface_context->rdp_surface);
}

gboolean
grd_rdp_layout_manager_transform_position (GrdRdpLayoutManager  *layout_manager,
                                           uint32_t              x,
                                           uint32_t              y,
                                           const char          **stream_path,
                                           double               *stream_x,
                                           double               *stream_y)
{
  g_autoptr (GMutexLocker) locker = NULL;
  SurfaceContext *surface_context = NULL;
  GHashTableIter iter;

  locker = g_mutex_locker_new (&layout_manager->state_mutex);
  if (layout_manager->state != UPDATE_STATE_AWAIT_CONFIG)
    return FALSE;

  g_hash_table_iter_init (&iter, layout_manager->surface_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &surface_context))
    {
      GrdRdpSurface *rdp_surface = surface_context->rdp_surface;
      uint32_t surface_width = grd_rdp_surface_get_width (rdp_surface);
      uint32_t surface_height = grd_rdp_surface_get_height (rdp_surface);

      if (!surface_context->stream)
        continue;

      if (x < surface_context->output_origin_x ||
          y < surface_context->output_origin_y ||
          x >= surface_context->output_origin_x + surface_width ||
          y >= surface_context->output_origin_y + surface_height)
        continue;

      *stream_path = grd_stream_get_object_path (surface_context->stream);
      *stream_x = x - surface_context->output_origin_x;
      *stream_y = y - surface_context->output_origin_y;
      return TRUE;
    }

  return FALSE;
}

static gboolean
dispose_surfaces (gpointer user_data)
{
  GrdRdpLayoutManager *layout_manager = user_data;
  GrdRdpSurface *rdp_surface;

  while ((rdp_surface = g_async_queue_try_pop (layout_manager->disposal_queue)))
    {
      g_clear_object (&rdp_surface->gfx_surface);
      grd_rdp_surface_free (rdp_surface);
    }

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

GrdRdpLayoutManager *
grd_rdp_layout_manager_new (GrdSessionRdp    *session_rdp,
                            GrdHwAccelNvidia *hwaccel_nvidia,
                            GMainContext     *render_context)
{
  GrdRdpLayoutManager *layout_manager;
  GSource *surface_disposal_source;

  layout_manager = g_object_new (GRD_TYPE_RDP_LAYOUT_MANAGER, NULL);
  layout_manager->session_rdp = session_rdp;
  layout_manager->hwaccel_nvidia = hwaccel_nvidia;
  layout_manager->render_context = render_context;

  surface_disposal_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (surface_disposal_source, dispose_surfaces,
                         layout_manager, NULL);
  g_source_set_ready_time (surface_disposal_source, -1);
  g_source_attach (surface_disposal_source, render_context);
  layout_manager->surface_disposal_source = surface_disposal_source;

  return layout_manager;
}

static void
grd_rdp_layout_manager_dispose (GObject *object)
{
  GrdRdpLayoutManager *layout_manager = GRD_RDP_LAYOUT_MANAGER (object);

  if (layout_manager->surface_table)
    g_hash_table_remove_all (layout_manager->surface_table);

  g_clear_handle_id (&layout_manager->layout_destruction_id, g_source_remove);
  g_clear_handle_id (&layout_manager->layout_recreation_id, g_source_remove);

  if (layout_manager->surface_disposal_source)
    {
      g_source_destroy (layout_manager->surface_disposal_source);
      g_clear_pointer (&layout_manager->surface_disposal_source, g_source_unref);
    }
  if (layout_manager->layout_update_source)
    {
      g_source_destroy (layout_manager->layout_update_source);
      g_clear_pointer (&layout_manager->layout_update_source, g_source_unref);
    }

  g_clear_pointer (&layout_manager->current_monitor_config,
                   grd_rdp_monitor_config_free);
  g_clear_pointer (&layout_manager->pending_monitor_config,
                   grd_rdp_monitor_config_free);

  g_clear_pointer (&layout_manager->pending_streams, g_hash_table_unref);
  g_clear_pointer (&layout_manager->surface_table, g_hash_table_unref);

  dispose_surfaces (layout_manager);
  g_clear_pointer (&layout_manager->disposal_queue, g_async_queue_unref);

  G_OBJECT_CLASS (grd_rdp_layout_manager_parent_class)->dispose (object);
}

static void
grd_rdp_layout_manager_finalize (GObject *object)
{
  GrdRdpLayoutManager *layout_manager = GRD_RDP_LAYOUT_MANAGER (object);

  g_mutex_clear (&layout_manager->monitor_config_mutex);
  g_mutex_clear (&layout_manager->state_mutex);

  G_OBJECT_CLASS (grd_rdp_layout_manager_parent_class)->finalize (object);
}

static gboolean
maybe_pick_up_queued_monitor_config (GrdRdpLayoutManager *layout_manager)
{
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&layout_manager->monitor_config_mutex);
  if (!layout_manager->pending_monitor_config)
    return FALSE;

  if (layout_manager->current_monitor_config)
    {
      g_assert (layout_manager->current_monitor_config->is_virtual ==
                layout_manager->pending_monitor_config->is_virtual);
    }

  g_clear_pointer (&layout_manager->current_monitor_config,
                   grd_rdp_monitor_config_free);
  layout_manager->current_monitor_config =
    g_steal_pointer (&layout_manager->pending_monitor_config);

  g_assert (!layout_manager->layout_destruction_id);
  g_clear_handle_id (&layout_manager->layout_recreation_id, g_source_remove);

  return TRUE;
}

static void
inhibit_surface_rendering (GrdRdpLayoutManager *layout_manager)
{
  SurfaceContext *surface_context = NULL;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, layout_manager->surface_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &surface_context))
    grd_rdp_surface_inhibit_rendering (surface_context->rdp_surface);
}

static void
uninhibit_surface_rendering (GrdRdpLayoutManager *layout_manager)
{
  SurfaceContext *surface_context = NULL;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, layout_manager->surface_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &surface_context))
    grd_rdp_surface_uninhibit_rendering (surface_context->rdp_surface);
}

static void
dispose_unneeded_surfaces (GrdRdpLayoutManager *layout_manager)
{
  GrdRdpMonitorConfig *monitor_config = layout_manager->current_monitor_config;
  uint32_t n_target_surfaces = monitor_config->monitor_count;
  uint32_t n_surfaces_to_dispose;
  SurfaceContext *surface_context = NULL;
  GHashTableIter iter;

  if (g_hash_table_size (layout_manager->surface_table) <= n_target_surfaces)
    return;

  n_surfaces_to_dispose = g_hash_table_size (layout_manager->surface_table) -
                          n_target_surfaces;

  g_hash_table_iter_init (&iter, layout_manager->surface_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &surface_context) &&
         n_surfaces_to_dispose > 0)
    {
      g_hash_table_iter_remove (&iter);
      --n_surfaces_to_dispose;
    }
  g_assert (g_hash_table_size (layout_manager->surface_table) <= n_target_surfaces);
  g_assert (n_surfaces_to_dispose == 0);
}

static gboolean
prepare_surface_contexts (GrdRdpLayoutManager  *layout_manager,
                          GError              **error)
{
  GrdRdpMonitorConfig *monitor_config = layout_manager->current_monitor_config;
  uint32_t n_target_surfaces = monitor_config->monitor_count;
  SurfaceContext *surface_context = NULL;
  GHashTableIter iter;
  uint32_t i = 0;

  while (g_hash_table_size (layout_manager->surface_table) < n_target_surfaces)
    {
      GrdRdpStreamOwner *stream_owner = GRD_RDP_STREAM_OWNER (layout_manager);
      uint32_t stream_id;

      surface_context = surface_context_new (layout_manager, error);
      if (!surface_context)
        return FALSE;

      grd_rdp_surface_inhibit_rendering (surface_context->rdp_surface);

      stream_id = grd_session_rdp_acquire_stream_id (layout_manager->session_rdp,
                                                     stream_owner);
      g_hash_table_insert (layout_manager->surface_table,
                           GUINT_TO_POINTER (stream_id), surface_context);
    }

  g_hash_table_iter_init (&iter, layout_manager->surface_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &surface_context))
    {
      g_autofree GrdRdpSurfaceMapping *surface_mapping = NULL;

      grd_rdp_surface_set_size (surface_context->rdp_surface, 0, 0);
      g_clear_pointer (&surface_context->virtual_monitor, g_free);

      if (monitor_config->is_virtual)
        {
          GrdRdpVirtualMonitor *virtual_monitor;
          int32_t surface_pos_x;
          int32_t surface_pos_y;

          virtual_monitor = &monitor_config->virtual_monitors[i++];
          surface_pos_x = virtual_monitor->pos_x - monitor_config->layout_offset_x;
          surface_pos_y = virtual_monitor->pos_y - monitor_config->layout_offset_y;
          g_assert (surface_pos_x >= 0);
          g_assert (surface_pos_y >= 0);

          surface_context->output_origin_x = surface_pos_x;
          surface_context->output_origin_y = surface_pos_y;

          surface_context->virtual_monitor =
            g_memdup2 (virtual_monitor, sizeof (GrdRdpVirtualMonitor));
        }
      else
        {
          surface_context->connector = monitor_config->connectors[i++];
        }

      surface_mapping = g_new0 (GrdRdpSurfaceMapping, 1);
      surface_mapping->mapping_type = GRD_RDP_SURFACE_MAPPING_TYPE_MAP_TO_OUTPUT;
      surface_mapping->output_origin_x = surface_context->output_origin_x;
      surface_mapping->output_origin_y = surface_context->output_origin_y;

      grd_rdp_surface_set_mapping (surface_context->rdp_surface,
                                   g_steal_pointer (&surface_mapping));
    }

  return TRUE;
}

static void
update_stream_params (SurfaceContext *surface_context)
{
  g_assert (surface_context->stream);
  g_assert (surface_context->pipewire_stream);

  grd_rdp_pipewire_stream_resize (surface_context->pipewire_stream,
                                  surface_context->virtual_monitor);
}

static void
create_stream (SurfaceContext *surface_context,
               uint32_t        stream_id)
{
  GrdRdpLayoutManager *layout_manager = surface_context->layout_manager;
  GrdSession *session = GRD_SESSION (layout_manager->session_rdp);

  g_assert (surface_context->virtual_monitor || surface_context->connector);

  g_hash_table_add (layout_manager->pending_streams,
                    GUINT_TO_POINTER (stream_id));

  if (surface_context->virtual_monitor)
    {
      grd_session_record_virtual (session, stream_id,
                                  GRD_SCREEN_CAST_CURSOR_MODE_METADATA,
                                  TRUE);
    }
  else
    {
      grd_session_record_monitor (session, stream_id,
                                  surface_context->connector,
                                  GRD_SCREEN_CAST_CURSOR_MODE_METADATA);
    }
}

static UpdateState
create_or_update_streams (GrdRdpLayoutManager *layout_manager)
{
  SurfaceContext *surface_context = NULL;
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, layout_manager->surface_table);
  while (g_hash_table_iter_next (&iter, &key, (gpointer *) &surface_context))
    {
      uint32_t stream_id = GPOINTER_TO_UINT (key);

      if (surface_context->stream)
        update_stream_params (surface_context);
      else
        create_stream (surface_context, stream_id);
    }

  if (g_hash_table_size (layout_manager->pending_streams) > 0)
    return UPDATE_STATE_AWAIT_STREAMS;

  return UPDATE_STATE_START_RENDERING;
}

static gboolean
update_monitor_layout (gpointer user_data)
{
  GrdRdpLayoutManager *layout_manager = user_data;
  g_autoptr (GError) error = NULL;

  if (!layout_manager->session_started)
    return G_SOURCE_CONTINUE;

  switch (layout_manager->state)
    {
    case UPDATE_STATE_FATAL_ERROR:
      break;
    case UPDATE_STATE_AWAIT_CONFIG:
      if (maybe_pick_up_queued_monitor_config (layout_manager))
        {
          inhibit_surface_rendering (layout_manager);

          transition_to_state (layout_manager, UPDATE_STATE_PREPARE_SURFACES);
          return update_monitor_layout (layout_manager);
        }
      break;
    case UPDATE_STATE_PREPARE_SURFACES:
      dispose_unneeded_surfaces (layout_manager);

      if (!prepare_surface_contexts (layout_manager, &error))
        {
          g_warning ("[RDP] Layout manager: Failed to prepare surface contexts: %s",
                     error->message);
          transition_to_state (layout_manager, UPDATE_STATE_FATAL_ERROR);
          return G_SOURCE_CONTINUE;
        }

      transition_to_state (layout_manager,
                           create_or_update_streams (layout_manager));
      break;
    case UPDATE_STATE_AWAIT_STREAMS:
      break;
    case UPDATE_STATE_START_RENDERING:
      uninhibit_surface_rendering (layout_manager);

      transition_to_state (layout_manager, UPDATE_STATE_AWAIT_CONFIG);
      break;
    }

  return G_SOURCE_CONTINUE;
}

static void
grd_rdp_layout_manager_init (GrdRdpLayoutManager *layout_manager)
{
  GSource *layout_update_source;

  layout_manager->state = UPDATE_STATE_AWAIT_CONFIG;

  layout_manager->surface_table =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) surface_context_free);
  layout_manager->pending_streams = g_hash_table_new (NULL, NULL);
  layout_manager->disposal_queue = g_async_queue_new ();

  g_mutex_init (&layout_manager->state_mutex);
  g_mutex_init (&layout_manager->monitor_config_mutex);

  layout_update_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (layout_update_source, update_monitor_layout,
                         layout_manager, NULL);
  g_source_set_ready_time (layout_update_source, -1);
  g_source_attach (layout_update_source, NULL);
  layout_manager->layout_update_source = layout_update_source;
}

static void
grd_rdp_layout_manager_class_init (GrdRdpLayoutManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdRdpStreamOwnerClass *stream_owner_class =
    GRD_RDP_STREAM_OWNER_CLASS (klass);

  object_class->dispose = grd_rdp_layout_manager_dispose;
  object_class->finalize = grd_rdp_layout_manager_finalize;

  stream_owner_class->on_stream_created =
    grd_rdp_layout_manager_on_stream_created;
}
