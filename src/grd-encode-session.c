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

#include "grd-encode-session.h"

typedef struct
{
  GrdImageView *image_view;

  GrdEncodeSessionOnBitstreamLockedFunc callback;
  gpointer callback_user_data;
} GrdLockBitstreamTask;

typedef struct
{
  gboolean in_shutdown;

  GThread *encode_thread;
  GMainContext *encode_context;

  GSource *bitstream_lock_source;
  GAsyncQueue *task_queue;
} GrdEncodeSessionPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GrdEncodeSession, grd_encode_session,
                                     G_TYPE_OBJECT)

void
grd_encode_session_get_surface_size (GrdEncodeSession *encode_session,
                                     uint32_t         *surface_width,
                                     uint32_t         *surface_height)
{
  GrdEncodeSessionClass *klass = GRD_ENCODE_SESSION_GET_CLASS (encode_session);

  return klass->get_surface_size (encode_session, surface_width, surface_height);
}

GList *
grd_encode_session_get_image_views (GrdEncodeSession *encode_session)
{
  GrdEncodeSessionClass *klass = GRD_ENCODE_SESSION_GET_CLASS (encode_session);

  return klass->get_image_views (encode_session);
}

gboolean
grd_encode_session_has_pending_frames (GrdEncodeSession *encode_session)
{
  GrdEncodeSessionClass *klass = GRD_ENCODE_SESSION_GET_CLASS (encode_session);

  return klass->has_pending_frames (encode_session);
}

gboolean
grd_encode_session_encode_frame (GrdEncodeSession  *encode_session,
                                 GrdImageView      *image_view,
                                 GError           **error)
{
  GrdEncodeSessionClass *klass = GRD_ENCODE_SESSION_GET_CLASS (encode_session);

  return klass->encode_frame (encode_session, image_view, error);
}

void
grd_encode_session_lock_bitstream (GrdEncodeSession                      *encode_session,
                                   GrdImageView                          *image_view,
                                   GrdEncodeSessionOnBitstreamLockedFunc  on_bitstream_locked,
                                   gpointer                               user_data)
{
  GrdEncodeSessionPrivate *priv =
    grd_encode_session_get_instance_private (encode_session);
  GrdLockBitstreamTask *task;

  task = g_new0 (GrdLockBitstreamTask, 1);
  task->image_view = image_view;
  task->callback = on_bitstream_locked;
  task->callback_user_data = user_data;

  g_async_queue_push (priv->task_queue, task);
  g_source_set_ready_time (priv->bitstream_lock_source, 0);
}

gboolean
grd_encode_session_unlock_bitstream (GrdEncodeSession  *encode_session,
                                     GrdBitstream      *bitstream,
                                     GError           **error)
{
  GrdEncodeSessionClass *klass = GRD_ENCODE_SESSION_GET_CLASS (encode_session);

  return klass->unlock_bitstream (encode_session, bitstream, error);
}

static void
stop_encode_thread (GrdEncodeSession *encode_session)
{
  GrdEncodeSessionPrivate *priv =
    grd_encode_session_get_instance_private (encode_session);

  g_assert (priv->encode_context);
  g_assert (priv->encode_thread);

  priv->in_shutdown = TRUE;

  g_main_context_wakeup (priv->encode_context);
  g_clear_pointer (&priv->encode_thread, g_thread_join);
}

static void
grd_encode_session_dispose (GObject *object)
{
  GrdEncodeSession *encode_session = GRD_ENCODE_SESSION (object);
  GrdEncodeSessionPrivate *priv =
    grd_encode_session_get_instance_private (encode_session);

  g_assert (g_async_queue_try_pop (priv->task_queue) == NULL);

  if (priv->encode_thread)
    stop_encode_thread (encode_session);

  if (priv->bitstream_lock_source)
    {
      g_source_destroy (priv->bitstream_lock_source);
      g_clear_pointer (&priv->bitstream_lock_source, g_source_unref);
    }

  g_clear_pointer (&priv->encode_context, g_main_context_unref);

  G_OBJECT_CLASS (grd_encode_session_parent_class)->dispose (object);
}

static void
grd_encode_session_finalize (GObject *object)
{
  GrdEncodeSession *encode_session = GRD_ENCODE_SESSION (object);
  GrdEncodeSessionPrivate *priv =
    grd_encode_session_get_instance_private (encode_session);

  g_clear_pointer (&priv->task_queue, g_async_queue_unref);

  G_OBJECT_CLASS (grd_encode_session_parent_class)->finalize (object);
}

static gpointer
encode_thread_func (gpointer data)
{
  GrdEncodeSession *encode_session = data;
  GrdEncodeSessionPrivate *priv =
    grd_encode_session_get_instance_private (encode_session);

  while (!priv->in_shutdown)
    g_main_context_iteration (priv->encode_context, TRUE);

  return NULL;
}

static gboolean
lock_bitstreams (gpointer user_data)
{
  GrdEncodeSession *encode_session = user_data;
  GrdEncodeSessionClass *klass = GRD_ENCODE_SESSION_GET_CLASS (encode_session);
  GrdEncodeSessionPrivate *priv =
    grd_encode_session_get_instance_private (encode_session);
  GrdLockBitstreamTask *task;

  while ((task = g_async_queue_try_pop (priv->task_queue)))
    {
      GrdBitstream *bitstream;
      g_autoptr (GError) error = NULL;

      bitstream = klass->lock_bitstream (encode_session, task->image_view,
                                         &error);
      task->callback (encode_session, bitstream, task->callback_user_data,
                      error);

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
grd_encode_session_init (GrdEncodeSession *encode_session)
{
  GrdEncodeSessionPrivate *priv =
    grd_encode_session_get_instance_private (encode_session);
  GSource *bitstream_lock_source;

  priv->task_queue = g_async_queue_new ();

  priv->encode_context = g_main_context_new ();
  priv->encode_thread = g_thread_new ("Encode thread",
                                      encode_thread_func,
                                      encode_session);

  bitstream_lock_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (bitstream_lock_source, lock_bitstreams,
                         encode_session, NULL);
  g_source_set_ready_time (bitstream_lock_source, -1);
  g_source_attach (bitstream_lock_source, priv->encode_context);
  priv->bitstream_lock_source = bitstream_lock_source;
}

static void
grd_encode_session_class_init (GrdEncodeSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_encode_session_dispose;
  object_class->finalize = grd_encode_session_finalize;
}
