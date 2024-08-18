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

#include "grd-rdp-view-creator.h"

#include "grd-rdp-frame.h"

typedef struct
{
  GrdRdpViewCreatorOnViewCreatedFunc callback;
  GrdRdpFrame *rdp_frame;
} GrdViewCreationTask;

typedef struct
{
  gboolean in_shutdown;

  GThread *view_creation_thread;
  GMainContext *view_creation_context;

  GSource *view_creation_source;
  GAsyncQueue *task_queue;
} GrdRdpViewCreatorPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GrdRdpViewCreator, grd_rdp_view_creator,
                                     G_TYPE_OBJECT)

gboolean
grd_rdp_view_creator_create_view (GrdRdpViewCreator                   *view_creator,
                                  GrdRdpFrame                         *rdp_frame,
                                  GrdRdpViewCreatorOnViewCreatedFunc   on_view_created,
                                  GError                             **error)
{
  GrdRdpViewCreatorClass *klass = GRD_RDP_VIEW_CREATOR_GET_CLASS (view_creator);
  GrdRdpViewCreatorPrivate *priv =
    grd_rdp_view_creator_get_instance_private (view_creator);
  GList *image_views = grd_rdp_frame_get_image_views (rdp_frame);
  GrdRdpBuffer *src_buffer_new = grd_rdp_frame_get_source_buffer (rdp_frame);
  GrdRdpBuffer *src_buffer_old = grd_rdp_frame_get_last_source_buffer (rdp_frame);
  GrdViewCreationTask *task;

  if (!klass->create_view (view_creator, image_views,
                           src_buffer_new, src_buffer_old, error))
    return FALSE;

  task = g_new0 (GrdViewCreationTask, 1);
  task->callback = on_view_created;
  task->rdp_frame = rdp_frame;

  g_async_queue_push (priv->task_queue, task);
  g_source_set_ready_time (priv->view_creation_source, 0);

  return TRUE;
}

static void
stop_view_creation_thread (GrdRdpViewCreator *view_creator)
{
  GrdRdpViewCreatorPrivate *priv =
    grd_rdp_view_creator_get_instance_private (view_creator);

  g_assert (priv->view_creation_context);
  g_assert (priv->view_creation_thread);

  priv->in_shutdown = TRUE;

  g_main_context_wakeup (priv->view_creation_context);
  g_clear_pointer (&priv->view_creation_thread, g_thread_join);
}

static void
grd_rdp_view_creator_dispose (GObject *object)
{
  GrdRdpViewCreator *view_creator = GRD_RDP_VIEW_CREATOR (object);
  GrdRdpViewCreatorPrivate *priv =
    grd_rdp_view_creator_get_instance_private (view_creator);

  g_assert (g_async_queue_try_pop (priv->task_queue) == NULL);

  if (priv->view_creation_thread)
    stop_view_creation_thread (view_creator);

  if (priv->view_creation_source)
    {
      g_source_destroy (priv->view_creation_source);
      g_clear_pointer (&priv->view_creation_source, g_source_unref);
    }

  g_clear_pointer (&priv->view_creation_context, g_main_context_unref);

  G_OBJECT_CLASS (grd_rdp_view_creator_parent_class)->dispose (object);
}

static void
grd_rdp_view_creator_finalize (GObject *object)
{
  GrdRdpViewCreator *view_creator = GRD_RDP_VIEW_CREATOR (object);
  GrdRdpViewCreatorPrivate *priv =
    grd_rdp_view_creator_get_instance_private (view_creator);

  g_clear_pointer (&priv->task_queue, g_async_queue_unref);

  G_OBJECT_CLASS (grd_rdp_view_creator_parent_class)->finalize (object);
}

static gpointer
view_creation_thread_func (gpointer data)
{
  GrdRdpViewCreator *view_creator = data;
  GrdRdpViewCreatorPrivate *priv =
    grd_rdp_view_creator_get_instance_private (view_creator);

  while (!priv->in_shutdown)
    g_main_context_iteration (priv->view_creation_context, TRUE);

  return NULL;
}

static gboolean
finish_views (gpointer user_data)
{
  GrdRdpViewCreator *view_creator = user_data;
  GrdRdpViewCreatorClass *klass = GRD_RDP_VIEW_CREATOR_GET_CLASS (view_creator);
  GrdRdpViewCreatorPrivate *priv =
    grd_rdp_view_creator_get_instance_private (view_creator);
  GrdViewCreationTask *task;

  while ((task = g_async_queue_try_pop (priv->task_queue)))
    {
      cairo_region_t *damage_region;
      g_autoptr (GError) error = NULL;

      damage_region = klass->finish_view (view_creator, &error);
      grd_rdp_frame_set_damage_region (task->rdp_frame, damage_region);

      task->callback (task->rdp_frame, error);

      g_free (task);
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

static void
grd_rdp_view_creator_init (GrdRdpViewCreator *view_creator)
{
  GrdRdpViewCreatorPrivate *priv =
    grd_rdp_view_creator_get_instance_private (view_creator);
  GSource *view_creation_source;

  priv->task_queue = g_async_queue_new ();

  priv->view_creation_context = g_main_context_new ();
  priv->view_creation_thread = g_thread_new ("View creation thread",
                                             view_creation_thread_func,
                                             view_creator);

  view_creation_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (view_creation_source, finish_views,
                         view_creator, NULL);
  g_source_set_ready_time (view_creation_source, -1);
  g_source_attach (view_creation_source, priv->view_creation_context);
  priv->view_creation_source = view_creation_source;
}

static void
grd_rdp_view_creator_class_init (GrdRdpViewCreatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_view_creator_dispose;
  object_class->finalize = grd_rdp_view_creator_finalize;
}
