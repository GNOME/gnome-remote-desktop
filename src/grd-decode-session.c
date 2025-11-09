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

#include "grd-decode-session.h"

typedef struct
{
  GrdSampleBuffer *sample_buffer;

  GrdDecodeSessionOnFrameReadyFunc callback;
  gpointer callback_user_data;
} GrdDecodeFrameTask;

typedef struct
{
  gboolean in_shutdown;

  GThread *decode_thread;
  GMainContext *decode_context;

  GSource *frame_decode_source;
  GAsyncQueue *task_queue;
} GrdDecodeSessionPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GrdDecodeSession, grd_decode_session,
                                     G_TYPE_OBJECT)

void
grd_decode_session_get_drm_format_modifiers (GrdDecodeSession  *decode_session,
                                             uint32_t           drm_format,
                                             uint32_t          *out_n_modifiers,
                                             uint64_t         **out_modifiers)
{
  GrdDecodeSessionClass *klass = GRD_DECODE_SESSION_GET_CLASS (decode_session);

  klass->get_drm_format_modifiers (decode_session, drm_format,
                                   out_n_modifiers, out_modifiers);
}

gboolean
grd_decode_session_reset (GrdDecodeSession  *decode_session,
                          uint32_t           surface_width,
                          uint32_t           surface_height,
                          uint64_t           drm_format_modifier,
                          GError           **error)
{
  GrdDecodeSessionClass *klass = GRD_DECODE_SESSION_GET_CLASS (decode_session);

  return klass->reset (decode_session, surface_width, surface_height,
                       drm_format_modifier, error);
}

gboolean
grd_decode_session_register_buffer (GrdDecodeSession  *decode_session,
                                    struct pw_buffer  *pw_buffer,
                                    GError           **error)
{
  GrdDecodeSessionClass *klass = GRD_DECODE_SESSION_GET_CLASS (decode_session);

  return klass->register_buffer (decode_session, pw_buffer, error);
}

void
grd_decode_session_unregister_buffer (GrdDecodeSession *decode_session,
                                      struct pw_buffer *pw_buffer)
{
  GrdDecodeSessionClass *klass = GRD_DECODE_SESSION_GET_CLASS (decode_session);

  return klass->unregister_buffer (decode_session, pw_buffer);
}

GrdSampleBuffer *
grd_decode_session_get_sample_buffer (GrdDecodeSession *decode_session,
                                      struct pw_buffer *pw_buffer)
{
  GrdDecodeSessionClass *klass = GRD_DECODE_SESSION_GET_CLASS (decode_session);

  return klass->get_sample_buffer (decode_session, pw_buffer);
}

uint32_t
grd_decode_session_get_n_pending_frames (GrdDecodeSession *decode_session)
{
  GrdDecodeSessionClass *klass = GRD_DECODE_SESSION_GET_CLASS (decode_session);

  return klass->get_n_pending_frames (decode_session);
}

gboolean
grd_decode_session_decode_frame (GrdDecodeSession                  *decode_session,
                                 GrdSampleBuffer                   *sample_buffer,
                                 GrdDecodeSessionOnFrameReadyFunc   on_frame_ready,
                                 gpointer                           user_data,
                                 GError                           **error)
{
  GrdDecodeSessionClass *klass = GRD_DECODE_SESSION_GET_CLASS (decode_session);
  GrdDecodeSessionPrivate *priv =
    grd_decode_session_get_instance_private (decode_session);
  GrdDecodeFrameTask *task;

  if (!klass->submit_sample (decode_session, sample_buffer, error))
    return FALSE;

  task = g_new0 (GrdDecodeFrameTask, 1);
  task->sample_buffer = sample_buffer;
  task->callback = on_frame_ready;
  task->callback_user_data = user_data;

  g_async_queue_push (priv->task_queue, task);
  g_source_set_ready_time (priv->frame_decode_source, 0);

  return TRUE;
}

static void
stop_decode_thread (GrdDecodeSession *decode_session)
{
  GrdDecodeSessionPrivate *priv =
    grd_decode_session_get_instance_private (decode_session);

  g_assert (priv->decode_context);
  g_assert (priv->decode_thread);

  priv->in_shutdown = TRUE;

  g_main_context_wakeup (priv->decode_context);
  g_clear_pointer (&priv->decode_thread, g_thread_join);
}

static void
grd_decode_session_dispose (GObject *object)
{
  GrdDecodeSession *decode_session = GRD_DECODE_SESSION (object);
  GrdDecodeSessionPrivate *priv =
    grd_decode_session_get_instance_private (decode_session);

  g_assert (g_async_queue_try_pop (priv->task_queue) == NULL);

  if (priv->decode_thread)
    stop_decode_thread (decode_session);

  if (priv->frame_decode_source)
    {
      g_source_destroy (priv->frame_decode_source);
      g_clear_pointer (&priv->frame_decode_source, g_source_unref);
    }

  g_clear_pointer (&priv->decode_context, g_main_context_unref);

  G_OBJECT_CLASS (grd_decode_session_parent_class)->dispose (object);
}

static void
grd_decode_session_finalize (GObject *object)
{
  GrdDecodeSession *decode_session = GRD_DECODE_SESSION (object);
  GrdDecodeSessionPrivate *priv =
    grd_decode_session_get_instance_private (decode_session);

  g_clear_pointer (&priv->task_queue, g_async_queue_unref);

  G_OBJECT_CLASS (grd_decode_session_parent_class)->finalize (object);
}

static gpointer
decode_thread_func (gpointer data)
{
  GrdDecodeSession *decode_session = data;
  GrdDecodeSessionPrivate *priv =
    grd_decode_session_get_instance_private (decode_session);

  while (!priv->in_shutdown)
    g_main_context_iteration (priv->decode_context, TRUE);

  return NULL;
}

static gboolean
decode_frames (gpointer user_data)
{
  GrdDecodeSession *decode_session = user_data;
  GrdDecodeSessionClass *klass = GRD_DECODE_SESSION_GET_CLASS (decode_session);
  GrdDecodeSessionPrivate *priv =
    grd_decode_session_get_instance_private (decode_session);
  GrdDecodeFrameTask *task;

  while ((task = g_async_queue_try_pop (priv->task_queue)))
    {
      gboolean success;
      g_autoptr (GError) error = NULL;

      success = klass->decode_frame (decode_session, task->sample_buffer,
                                     &error);
      g_assert (success == !error);

      task->callback (decode_session, task->sample_buffer,
                      task->callback_user_data, error);

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
grd_decode_session_init (GrdDecodeSession *decode_session)
{
  GrdDecodeSessionPrivate *priv =
    grd_decode_session_get_instance_private (decode_session);
  GSource *frame_decode_source;

  priv->task_queue = g_async_queue_new ();

  priv->decode_context = g_main_context_new ();
  priv->decode_thread = g_thread_new ("Decode thread",
                                      decode_thread_func,
                                      decode_session);

  frame_decode_source = g_source_new (&source_funcs, sizeof (GSource));
  g_source_set_callback (frame_decode_source, decode_frames,
                         decode_session, NULL);
  g_source_set_ready_time (frame_decode_source, -1);
  g_source_attach (frame_decode_source, priv->decode_context);
  priv->frame_decode_source = frame_decode_source;
}

static void
grd_decode_session_class_init (GrdDecodeSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_decode_session_dispose;
  object_class->finalize = grd_decode_session_finalize;
}
