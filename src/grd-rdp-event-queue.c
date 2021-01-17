/*
 * Copyright (C) 2021 Pascal Nowack
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

#include "grd-rdp-event-queue.h"

typedef enum _RdpEventType
{
  RDP_EVENT_TYPE_NONE,
  RDP_EVENT_TYPE_INPUT_KBD_KEYSYM,
  RDP_EVENT_TYPE_INPUT_PTR_MOTION_ABS,
  RDP_EVENT_TYPE_INPUT_PTR_BUTTON,
  RDP_EVENT_TYPE_INPUT_PTR_AXIS,
} RdpEventType;

typedef struct _RdpEvent
{
  RdpEventType type;

  /* RDP_EVENT_TYPE_INPUT_KBD_KEYSYM */
  struct
  {
    uint32_t keysym;
    GrdKeyState state;
  } input_kbd_keysym;

  /* RDP_EVENT_TYPE_INPUT_PTR_MOTION_ABS */
  struct
  {
    double x;
    double y;
  } input_ptr_motion_abs;

  /* RDP_EVENT_TYPE_INPUT_PTR_BUTTON */
  struct
  {
    int32_t button;
    GrdButtonState state;
  } input_ptr_button;

  /* RDP_EVENT_TYPE_INPUT_PTR_AXIS */
  struct
  {
    double dx;
    double dy;
    GrdPointerAxisFlags flags;
  } input_ptr_axis;
} RdpEvent;

struct _GrdRdpEventQueue
{
  GObject parent;

  GrdSessionRdp *session_rdp;
  GSource *flush_source;

  GMutex queue_mutex;
  GQueue *queue;
};

G_DEFINE_TYPE (GrdRdpEventQueue, grd_rdp_event_queue, G_TYPE_OBJECT);

static void
queue_rdp_event (GrdRdpEventQueue *rdp_event_queue,
                 RdpEvent         *rdp_event)
{
  g_mutex_lock (&rdp_event_queue->queue_mutex);
  g_queue_push_tail (rdp_event_queue->queue, rdp_event);
  g_mutex_unlock (&rdp_event_queue->queue_mutex);

  g_source_set_ready_time (rdp_event_queue->flush_source, 0);
}

void
grd_rdp_event_queue_add_input_event_keyboard_keysym (GrdRdpEventQueue *rdp_event_queue,
                                                     uint32_t          keysym,
                                                     GrdKeyState       state)
{
  RdpEvent *rdp_event;

  rdp_event = g_malloc0 (sizeof (RdpEvent));
  rdp_event->type = RDP_EVENT_TYPE_INPUT_KBD_KEYSYM;
  rdp_event->input_kbd_keysym.keysym = keysym;
  rdp_event->input_kbd_keysym.state = state;

  queue_rdp_event (rdp_event_queue, rdp_event);
}

void
grd_rdp_event_queue_add_input_event_pointer_motion_abs (GrdRdpEventQueue *rdp_event_queue,
                                                        double            x,
                                                        double            y)
{
  RdpEvent *rdp_event;

  rdp_event = g_malloc0 (sizeof (RdpEvent));
  rdp_event->type = RDP_EVENT_TYPE_INPUT_PTR_MOTION_ABS;
  rdp_event->input_ptr_motion_abs.x = x;
  rdp_event->input_ptr_motion_abs.y = y;

  queue_rdp_event (rdp_event_queue, rdp_event);
}

void
grd_rdp_event_queue_add_input_event_pointer_button (GrdRdpEventQueue *rdp_event_queue,
                                                    int32_t           button,
                                                    GrdButtonState    state)
{
  RdpEvent *rdp_event;

  rdp_event = g_malloc0 (sizeof (RdpEvent));
  rdp_event->type = RDP_EVENT_TYPE_INPUT_PTR_BUTTON;
  rdp_event->input_ptr_button.button = button;
  rdp_event->input_ptr_button.state = state;

  queue_rdp_event (rdp_event_queue, rdp_event);
}

void
grd_rdp_event_queue_add_input_event_pointer_axis (GrdRdpEventQueue    *rdp_event_queue,
                                                  double               dx,
                                                  double               dy,
                                                  GrdPointerAxisFlags  flags)
{
  RdpEvent *rdp_event;

  rdp_event = g_malloc0 (sizeof (RdpEvent));
  rdp_event->type = RDP_EVENT_TYPE_INPUT_PTR_AXIS;
  rdp_event->input_ptr_axis.dx = dx;
  rdp_event->input_ptr_axis.dy = dy;
  rdp_event->input_ptr_axis.flags = flags;

  queue_rdp_event (rdp_event_queue, rdp_event);
}

static void
process_rdp_events (GrdRdpEventQueue *rdp_event_queue)
{
  GrdSession *session = GRD_SESSION (rdp_event_queue->session_rdp);
  RdpEvent *rdp_event;

  while ((rdp_event = g_queue_pop_head (rdp_event_queue->queue)))
    {
      switch (rdp_event->type)
        {
        case RDP_EVENT_TYPE_NONE:
          break;
        case RDP_EVENT_TYPE_INPUT_KBD_KEYSYM:
          grd_session_notify_keyboard_keysym (session,
                                              rdp_event->input_kbd_keysym.keysym,
                                              rdp_event->input_kbd_keysym.state);
          break;
        case RDP_EVENT_TYPE_INPUT_PTR_MOTION_ABS:
          grd_session_notify_pointer_motion_absolute (
            session, rdp_event->input_ptr_motion_abs.x,
            rdp_event->input_ptr_motion_abs.y);
          break;
        case RDP_EVENT_TYPE_INPUT_PTR_BUTTON:
          grd_session_notify_pointer_button (session,
                                             rdp_event->input_ptr_button.button,
                                             rdp_event->input_ptr_button.state);
          break;
        case RDP_EVENT_TYPE_INPUT_PTR_AXIS:
          grd_session_notify_pointer_axis (session,
                                           rdp_event->input_ptr_axis.dx,
                                           rdp_event->input_ptr_axis.dy,
                                           rdp_event->input_ptr_axis.flags);
          break;
        }

      g_free (rdp_event);
    }
}

static gboolean
flush_rdp_events (gpointer user_data)
{
  GrdRdpEventQueue *rdp_event_queue = user_data;

  g_mutex_lock (&rdp_event_queue->queue_mutex);
  process_rdp_events (rdp_event_queue);
  g_mutex_unlock (&rdp_event_queue->queue_mutex);

  return G_SOURCE_CONTINUE;
}

static gboolean
flush_source_dispatch (GSource     *source,
                       GSourceFunc  callback,
                       gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs flush_source_funcs =
{
  .dispatch = flush_source_dispatch,
};

GrdRdpEventQueue *
grd_rdp_event_queue_new (GrdSessionRdp *session_rdp)
{
  GrdRdpEventQueue *rdp_event_queue;
  GSource *flush_source;

  rdp_event_queue = g_object_new (GRD_TYPE_RDP_EVENT_QUEUE, NULL);
  rdp_event_queue->session_rdp = session_rdp;

  flush_source = g_source_new (&flush_source_funcs, sizeof (GSource));
  g_source_set_callback (flush_source, flush_rdp_events, rdp_event_queue, NULL);
  g_source_set_ready_time (flush_source, -1);
  g_source_attach (flush_source, g_main_context_get_thread_default ());
  rdp_event_queue->flush_source = flush_source;

  return rdp_event_queue;
}

void
free_rdp_event (gpointer data)
{
  RdpEvent *rdp_event = data;

  g_free (rdp_event);
}

static void
grd_rdp_event_queue_dispose (GObject *object)
{
  GrdRdpEventQueue *rdp_event_queue = GRD_RDP_EVENT_QUEUE (object);

  /**
   * Process all events to ensure that remaining keysym-released events are sent
   */
  process_rdp_events (rdp_event_queue);
  g_queue_free_full (rdp_event_queue->queue, free_rdp_event);

  g_source_destroy (rdp_event_queue->flush_source);
  g_clear_pointer (&rdp_event_queue->flush_source, g_source_unref);

  G_OBJECT_CLASS (grd_rdp_event_queue_parent_class)->dispose (object);
}

static void
grd_rdp_event_queue_init (GrdRdpEventQueue *rdp_event_queue)
{
  rdp_event_queue->queue = g_queue_new ();

  g_mutex_init (&rdp_event_queue->queue_mutex);
}

static void
grd_rdp_event_queue_class_init (GrdRdpEventQueueClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_event_queue_dispose;
}
