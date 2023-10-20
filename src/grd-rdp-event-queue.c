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

#include <xkbcommon/xkbcommon-keysyms.h>

typedef enum _RdpEventType
{
  RDP_EVENT_TYPE_NONE,
  RDP_EVENT_TYPE_INPUT_KBD_KEYCODE,
  RDP_EVENT_TYPE_INPUT_KBD_KEYSYM,
  RDP_EVENT_TYPE_INPUT_PTR_MOTION_ABS,
  RDP_EVENT_TYPE_INPUT_PTR_BUTTON,
  RDP_EVENT_TYPE_INPUT_PTR_AXIS,
} RdpEventType;

typedef struct _RdpEvent
{
  RdpEventType type;

  /* RDP_EVENT_TYPE_INPUT_KBD_KEYCODE */
  struct
  {
    uint32_t keycode;
    GrdKeyState state;
  } input_kbd_keycode;

  /* RDP_EVENT_TYPE_INPUT_KBD_KEYSYM */
  struct
  {
    uint32_t keysym;
    GrdKeyState state;
  } input_kbd_keysym;

  /* RDP_EVENT_TYPE_INPUT_PTR_MOTION_ABS */
  struct
  {
    GrdStream *stream;
    GrdEventPointerMotionAbs motion_abs;
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

typedef struct _RdpSynchronizationEvent
{
  gboolean caps_lock_state;
  gboolean num_lock_state;
} RdpSynchronizationEvent;

struct _GrdRdpEventQueue
{
  GObject parent;

  GrdSessionRdp *session_rdp;
  GSource *flush_source;

  GMutex event_mutex;
  GQueue *queue;
  RdpSynchronizationEvent *rdp_sync_event;

  gboolean pending_sync_caps_lock;
  gboolean pending_sync_num_lock;
  gboolean caps_lock_state;
  gboolean num_lock_state;
};

G_DEFINE_TYPE (GrdRdpEventQueue, grd_rdp_event_queue, G_TYPE_OBJECT)

void
free_rdp_event (gpointer data)
{
  RdpEvent *rdp_event = data;

  g_clear_object (&rdp_event->input_ptr_motion_abs.stream);
  g_free (rdp_event);
}

static void
handle_synchronization_event (GrdRdpEventQueue *rdp_event_queue)
{
  GrdSession *session = GRD_SESSION (rdp_event_queue->session_rdp);
  RdpSynchronizationEvent *rdp_sync_event;

  rdp_sync_event = g_steal_pointer (&rdp_event_queue->rdp_sync_event);

  if (rdp_sync_event->caps_lock_state != rdp_event_queue->caps_lock_state)
    {
      g_debug ("Synchronizing caps lock state to be %s",
               rdp_sync_event->caps_lock_state ? "locked": "unlocked");

      grd_session_notify_keyboard_keysym (session, XKB_KEY_Caps_Lock,
                                          GRD_KEY_STATE_PRESSED);
      grd_session_notify_keyboard_keysym (session, XKB_KEY_Caps_Lock,
                                          GRD_KEY_STATE_RELEASED);

      rdp_event_queue->pending_sync_caps_lock = TRUE;
    }
  if (rdp_sync_event->num_lock_state != rdp_event_queue->num_lock_state)
    {
      g_debug ("Synchronizing num lock state to be %s",
               rdp_sync_event->num_lock_state ? "locked": "unlocked");

      grd_session_notify_keyboard_keysym (session, XKB_KEY_Num_Lock,
                                          GRD_KEY_STATE_PRESSED);
      grd_session_notify_keyboard_keysym (session, XKB_KEY_Num_Lock,
                                          GRD_KEY_STATE_RELEASED);

      rdp_event_queue->pending_sync_num_lock = TRUE;
    }

  g_free (rdp_sync_event);
}

static void
process_rdp_events (GrdRdpEventQueue *rdp_event_queue)
{
  GrdSession *session = GRD_SESSION (rdp_event_queue->session_rdp);
  RdpEvent *rdp_event;

  if (rdp_event_queue->rdp_sync_event &&
      !rdp_event_queue->pending_sync_caps_lock &&
      !rdp_event_queue->pending_sync_num_lock)
    handle_synchronization_event (rdp_event_queue);

  while ((rdp_event = g_queue_pop_head (rdp_event_queue->queue)))
    {
      switch (rdp_event->type)
        {
        case RDP_EVENT_TYPE_NONE:
          break;
        case RDP_EVENT_TYPE_INPUT_KBD_KEYCODE:
          grd_session_notify_keyboard_keycode (
            session, rdp_event->input_kbd_keycode.keycode,
            rdp_event->input_kbd_keycode.state);
          break;
        case RDP_EVENT_TYPE_INPUT_KBD_KEYSYM:
          grd_session_notify_keyboard_keysym (session,
                                              rdp_event->input_kbd_keysym.keysym,
                                              rdp_event->input_kbd_keysym.state);
          break;
        case RDP_EVENT_TYPE_INPUT_PTR_MOTION_ABS:
          grd_session_notify_pointer_motion_absolute (
            session, rdp_event->input_ptr_motion_abs.stream,
            &rdp_event->input_ptr_motion_abs.motion_abs);
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

      free_rdp_event (rdp_event);
    }
}

void
grd_rdp_event_queue_flush (GrdRdpEventQueue *rdp_event_queue)
{
  g_mutex_lock (&rdp_event_queue->event_mutex);
  process_rdp_events (rdp_event_queue);
  g_mutex_unlock (&rdp_event_queue->event_mutex);
}

static void
queue_rdp_event (GrdRdpEventQueue *rdp_event_queue,
                 RdpEvent         *rdp_event)
{
  g_mutex_lock (&rdp_event_queue->event_mutex);
  g_queue_push_tail (rdp_event_queue->queue, rdp_event);
  g_mutex_unlock (&rdp_event_queue->event_mutex);

  g_source_set_ready_time (rdp_event_queue->flush_source, 0);
}

void
grd_rdp_event_queue_add_input_event_keyboard_keycode (GrdRdpEventQueue *rdp_event_queue,
                                                      uint32_t          keycode,
                                                      GrdKeyState       state)
{
  RdpEvent *rdp_event;

  rdp_event = g_malloc0 (sizeof (RdpEvent));
  rdp_event->type = RDP_EVENT_TYPE_INPUT_KBD_KEYCODE;
  rdp_event->input_kbd_keycode.keycode = keycode;
  rdp_event->input_kbd_keycode.state = state;

  queue_rdp_event (rdp_event_queue, rdp_event);
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
grd_rdp_event_queue_add_input_event_pointer_motion_abs (GrdRdpEventQueue               *rdp_event_queue,
                                                        GrdStream                      *stream,
                                                        const GrdEventPointerMotionAbs *motion_abs)
{
  RdpEvent *rdp_event;

  rdp_event = g_malloc0 (sizeof (RdpEvent));
  rdp_event->type = RDP_EVENT_TYPE_INPUT_PTR_MOTION_ABS;
  rdp_event->input_ptr_motion_abs.stream = stream;
  rdp_event->input_ptr_motion_abs.motion_abs = *motion_abs;

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

void
grd_rdp_event_queue_update_caps_lock_state (GrdRdpEventQueue *rdp_event_queue,
                                            gboolean          caps_lock_state)
{
  rdp_event_queue->caps_lock_state = caps_lock_state;
  rdp_event_queue->pending_sync_caps_lock = FALSE;

  if (rdp_event_queue->pending_sync_num_lock)
    return;

  g_source_set_ready_time (rdp_event_queue->flush_source, 0);
}

void
grd_rdp_event_queue_update_num_lock_state (GrdRdpEventQueue *rdp_event_queue,
                                           gboolean          num_lock_state)
{
  rdp_event_queue->num_lock_state = num_lock_state;
  rdp_event_queue->pending_sync_num_lock = FALSE;

  if (rdp_event_queue->pending_sync_caps_lock)
    return;

  g_source_set_ready_time (rdp_event_queue->flush_source, 0);
}

void
grd_rdp_event_queue_add_synchronization_event (GrdRdpEventQueue *rdp_event_queue,
                                               gboolean          caps_lock_state,
                                               gboolean          num_lock_state)
{
  RdpSynchronizationEvent *rdp_sync_event;

  rdp_sync_event = g_malloc0 (sizeof (RdpSynchronizationEvent));
  rdp_sync_event->caps_lock_state = caps_lock_state;
  rdp_sync_event->num_lock_state = num_lock_state;

  g_mutex_lock (&rdp_event_queue->event_mutex);
  g_clear_pointer (&rdp_event_queue->rdp_sync_event, g_free);
  rdp_event_queue->rdp_sync_event = rdp_sync_event;
  g_mutex_unlock (&rdp_event_queue->event_mutex);

  g_source_set_ready_time (rdp_event_queue->flush_source, 0);
}

static gboolean
flush_rdp_events (gpointer user_data)
{
  GrdRdpEventQueue *rdp_event_queue = user_data;

  g_mutex_lock (&rdp_event_queue->event_mutex);
  process_rdp_events (rdp_event_queue);
  g_mutex_unlock (&rdp_event_queue->event_mutex);

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
  g_source_attach (flush_source, NULL);
  rdp_event_queue->flush_source = flush_source;

  return rdp_event_queue;
}

static void
grd_rdp_event_queue_dispose (GObject *object)
{
  GrdRdpEventQueue *rdp_event_queue = GRD_RDP_EVENT_QUEUE (object);

  g_clear_pointer (&rdp_event_queue->rdp_sync_event, g_free);

  if (rdp_event_queue->flush_source)
    {
      g_source_destroy (rdp_event_queue->flush_source);
      g_clear_pointer (&rdp_event_queue->flush_source, g_source_unref);
    }

  G_OBJECT_CLASS (grd_rdp_event_queue_parent_class)->dispose (object);
}

static void
grd_rdp_event_queue_finalize (GObject *object)
{
  GrdRdpEventQueue *rdp_event_queue = GRD_RDP_EVENT_QUEUE (object);

  g_mutex_clear (&rdp_event_queue->event_mutex);

  g_queue_free_full (rdp_event_queue->queue, free_rdp_event);

  G_OBJECT_CLASS (grd_rdp_event_queue_parent_class)->finalize (object);
}

static void
grd_rdp_event_queue_init (GrdRdpEventQueue *rdp_event_queue)
{
  rdp_event_queue->queue = g_queue_new ();

  g_mutex_init (&rdp_event_queue->event_mutex);

  rdp_event_queue->pending_sync_caps_lock = TRUE;
  rdp_event_queue->pending_sync_num_lock = TRUE;
}

static void
grd_rdp_event_queue_class_init (GrdRdpEventQueueClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_event_queue_dispose;
  object_class->finalize = grd_rdp_event_queue_finalize;
}
