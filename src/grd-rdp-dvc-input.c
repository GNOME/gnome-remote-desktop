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

#include "grd-rdp-dvc-input.h"

#include <freerdp/server/rdpei.h>

#include "grd-rdp-layout-manager.h"

#define MAX_TOUCH_CONTACTS 256

typedef enum
{
  INPUT_EVENT_TYPE_TOUCH,
  INPUT_EVENT_TYPE_DISMISS_HOVERING_TOUCH_CONTACT,
  INPUT_EVENT_TYPE_PEN,
} InputEventType;

/*
 * Contact states based on Touch Contact State Transitions
 * ([MS-RDPEI] 3.1.1.1)
 */
typedef enum
{
  CONTACT_STATE_OUT_OF_RANGE = 0,
  CONTACT_STATE_HOVERING,
  CONTACT_STATE_ENGAGED,
} ContactState;

typedef struct
{
  InputEventType event_type;

  /* INPUT_EVENT_TYPE_TOUCH */
  RDPINPUT_TOUCH_FRAME touch_frame;

  /* INPUT_EVENT_TYPE_DISMISS_HOVERING_TOUCH_CONTACT */
  uint8_t touch_contact_to_dismiss;

  /* INPUT_EVENT_TYPE_PEN */
  /* Missing libei-API to submit (multi-)pen events */
} InputEvent;

typedef struct
{
  ContactState contact_state;

  GrdTouchContact *touch_contact;
  gboolean ignore_contact;
} TouchContext;

struct _GrdRdpDvcInput
{
  GrdRdpDvc parent;

  RdpeiServerContext *rdpei_context;
  gboolean channel_opened;

  GrdRdpLayoutManager *layout_manager;
  GrdSession *session;

  GSource *event_source;
  GAsyncQueue *event_queue;

  gboolean pending_touch_device_frame;
  GList *touch_contacts_to_dispose;

  TouchContext touch_contexts[MAX_TOUCH_CONTACTS];
};

G_DEFINE_TYPE (GrdRdpDvcInput, grd_rdp_dvc_input,
               GRD_TYPE_RDP_DVC)

static void
grd_rdp_dvc_input_maybe_init (GrdRdpDvc *dvc)
{
  GrdRdpDvcInput *input = GRD_RDP_DVC_INPUT (dvc);
  RdpeiServerContext *rdpei_context;

  if (input->channel_opened)
    return;

  rdpei_context = input->rdpei_context;
  if (rdpei_context->Open (rdpei_context))
    {
      g_warning ("[RDP.INPUT] Failed to open channel. "
                 "Terminating protocol");
      grd_rdp_dvc_queue_channel_tear_down (GRD_RDP_DVC (input));
      return;
    }
  input->channel_opened = TRUE;
}

static void
dvc_creation_status (gpointer user_data,
                     int32_t  creation_status)
{
  GrdRdpDvcInput *input = user_data;
  RdpeiServerContext *rdpei_context = input->rdpei_context;

  if (creation_status < 0)
    {
      g_debug ("[RDP.INPUT] Failed to open channel (CreationStatus %i). "
               "Terminating protocol", creation_status);
      grd_rdp_dvc_queue_channel_tear_down (GRD_RDP_DVC (input));
      return;
    }

  rdpei_server_send_sc_ready (rdpei_context, RDPINPUT_PROTOCOL_V300,
                              SC_READY_MULTIPEN_INJECTION_SUPPORTED);
}

static BOOL
input_channel_id_assigned (RdpeiServerContext *rdpei_context,
                           uint32_t            channel_id)
{
  GrdRdpDvcInput *input = rdpei_context->user_data;
  GrdRdpDvc *dvc = GRD_RDP_DVC (input);

  g_debug ("[RDP.INPUT] DVC channel id assigned to id %u", channel_id);

  grd_rdp_dvc_subscribe_creation_status (dvc, channel_id,
                                         dvc_creation_status,
                                         input);

  return TRUE;
}

static uint32_t
input_client_ready (RdpeiServerContext *rdpei_context)
{
  g_message ("[RDP.INPUT] Client version: 0x%08X, Flags: 0x%08X, "
             "Maximum simultaneous touch contacts: %u",
             rdpei_context->clientVersion, rdpei_context->protocolFlags,
             rdpei_context->maxTouchPoints);

  return CHANNEL_RC_OK;
}

static uint32_t
input_touch (RdpeiServerContext         *rdpei_context,
             const RDPINPUT_TOUCH_EVENT *touch_event)
{
  GrdRdpDvcInput *input = rdpei_context->user_data;
  uint16_t i;

  for (i = 0; i < touch_event->frameCount; ++i)
    {
      RDPINPUT_TOUCH_FRAME *touch_frame = &touch_event->frames[i];
      InputEvent *input_event;

      if (touch_frame->contactCount == 0)
        continue;

      input_event = g_new0 (InputEvent, 1);
      input_event->event_type = INPUT_EVENT_TYPE_TOUCH;
      input_event->touch_frame = *touch_frame;
      input_event->touch_frame.contacts =
        g_memdup2 (touch_frame->contacts, touch_frame->contactCount *
                                          sizeof (RDPINPUT_CONTACT_DATA));

      g_async_queue_push (input->event_queue, input_event);
      g_source_set_ready_time (input->event_source, 0);
    }

  return CHANNEL_RC_OK;
}

static uint32_t
input_dismiss_hovering_touch_contact (RdpeiServerContext *rdpei_context,
                                      uint8_t             contact_id)
{
  GrdRdpDvcInput *input = rdpei_context->user_data;
  InputEvent *input_event;

  input_event = g_new0 (InputEvent, 1);
  input_event->event_type = INPUT_EVENT_TYPE_DISMISS_HOVERING_TOUCH_CONTACT;
  input_event->touch_contact_to_dismiss = contact_id;

  g_async_queue_push (input->event_queue, input_event);
  g_source_set_ready_time (input->event_source, 0);

  return CHANNEL_RC_OK;
}

static uint32_t
input_pen (RdpeiServerContext       *rdpei_context,
           const RDPINPUT_PEN_EVENT *pen_event)
{
  /* Missing libei-API to submit (multi-)pen events */

  return CHANNEL_RC_OK;
}

GrdRdpDvcInput *
grd_rdp_dvc_input_new (GrdRdpLayoutManager *layout_manager,
                       GrdSessionRdp       *session_rdp,
                       GrdRdpDvcHandler    *dvc_handler,
                       HANDLE               vcm)
{
  GrdRdpDvcInput *input;
  RdpeiServerContext *rdpei_context;

  input = g_object_new (GRD_TYPE_RDP_DVC_INPUT, NULL);
  rdpei_context = rdpei_server_context_new (vcm);
  if (!rdpei_context)
    g_error ("[RDP.INPUT] Failed to allocate server context (OOM)");

  input->rdpei_context = rdpei_context;
  input->layout_manager = layout_manager;
  input->session = GRD_SESSION (session_rdp);

  grd_rdp_dvc_initialize_base (GRD_RDP_DVC (input),
                               dvc_handler, session_rdp,
                               GRD_RDP_CHANNEL_INPUT);

  rdpei_context->onChannelIdAssigned = input_channel_id_assigned;
  rdpei_context->onClientReady = input_client_ready;
  rdpei_context->onTouchEvent = input_touch;
  rdpei_context->onTouchReleased = input_dismiss_hovering_touch_contact;
  rdpei_context->onPenEvent = input_pen;
  rdpei_context->user_data = input;

  return input;
}

static void
ensure_queued_touch_device_frame (GrdRdpDvcInput *input)
{
  input->pending_touch_device_frame = TRUE;
}

static void
queue_touch_contact_disposal (GrdRdpDvcInput *input,
                              TouchContext   *touch_context)
{
  GrdTouchContact *touch_contact;

  g_assert (touch_context->touch_contact);

  touch_contact = g_steal_pointer (&touch_context->touch_contact);
  input->touch_contacts_to_dispose =
    g_list_prepend (input->touch_contacts_to_dispose, touch_contact);

  touch_context->ignore_contact = FALSE;
}

/*
 * Automata based on Touch Contact State Transitions
 * ([MS-RDPEI] 3.1.1.1)
 */
static void
handle_touch_state_out_of_range (GrdRdpDvcInput        *input,
                                 RDPINPUT_CONTACT_DATA *touch_contact_data)
{
  uint8_t contact_id = touch_contact_data->contactId;
  TouchContext *touch_context = &input->touch_contexts[contact_id];
  GrdSession *session = input->session;

  g_assert (!touch_context->touch_contact);
  g_assert (!touch_context->ignore_contact);

  if (touch_contact_data->contactFlags == (RDPINPUT_CONTACT_FLAG_DOWN |
                                           RDPINPUT_CONTACT_FLAG_INRANGE |
                                           RDPINPUT_CONTACT_FLAG_INCONTACT))
    {
      GrdRdpLayoutManager *layout_manager = input->layout_manager;
      GrdEventMotionAbs motion_abs = {};
      GrdStream *stream = NULL;

      touch_context->touch_contact =
        grd_session_acquire_touch_contact (session);

      if (grd_rdp_layout_manager_transform_position (layout_manager,
                                                     touch_contact_data->x,
                                                     touch_contact_data->y,
                                                     &stream, &motion_abs))
        {
          grd_session_notify_touch_down (session, touch_context->touch_contact,
                                         stream, &motion_abs);
          ensure_queued_touch_device_frame (input);
        }
      else
        {
          /*
           * The client does not know, when a specific touch contact is useless
           * due to its position not being transformable. As a result, it will
           * deem it as any other touch contact. So ignore all assigned actions
           * and just let the touch contact run through all automata states.
           *
           * See also 3.2.5.3 Processing an RDPINPUT_TOUCH_EVENT_PDU Message
           * ([MS-RDPEI])
           */
          touch_context->ignore_contact = TRUE;
        }

      touch_context->contact_state = CONTACT_STATE_ENGAGED;
    }
  else if (touch_contact_data->contactFlags == (RDPINPUT_CONTACT_FLAG_UPDATE |
                                                RDPINPUT_CONTACT_FLAG_INRANGE))
    {
      touch_context->touch_contact =
        grd_session_acquire_touch_contact (session);

      touch_context->contact_state = CONTACT_STATE_HOVERING;
    }
  else
    {
      g_warning ("[RDP.INPUT] Protocol violation: Client sent invalid contact "
                 "flags 0x%08X in state 'Out of Range' for contact %u",
                 touch_contact_data->contactFlags, contact_id);
    }
}

/*
 * Automata based on Touch Contact State Transitions
 * ([MS-RDPEI] 3.1.1.1)
 */
static void
handle_touch_state_hovering (GrdRdpDvcInput        *input,
                             RDPINPUT_CONTACT_DATA *touch_contact_data)
{
  uint8_t contact_id = touch_contact_data->contactId;
  TouchContext *touch_context = &input->touch_contexts[contact_id];

  g_assert (touch_context->touch_contact);

  if (touch_contact_data->contactFlags == (RDPINPUT_CONTACT_FLAG_DOWN |
                                           RDPINPUT_CONTACT_FLAG_INRANGE |
                                           RDPINPUT_CONTACT_FLAG_INCONTACT))
    {
      GrdRdpLayoutManager *layout_manager = input->layout_manager;
      GrdSession *session = input->session;
      GrdEventMotionAbs motion_abs = {};
      GrdStream *stream = NULL;

      if (grd_rdp_layout_manager_transform_position (layout_manager,
                                                     touch_contact_data->x,
                                                     touch_contact_data->y,
                                                     &stream, &motion_abs))
        {
          grd_session_notify_touch_down (session, touch_context->touch_contact,
                                         stream, &motion_abs);
          ensure_queued_touch_device_frame (input);
        }
      else
        {
          /*
           * The client does not know, when a specific touch contact is useless
           * due to its position not being transformable. As a result, it will
           * deem it as any other touch contact. So ignore all assigned actions
           * and just let the touch contact run through all automata states.
           *
           * See also 3.2.5.3 Processing an RDPINPUT_TOUCH_EVENT_PDU Message
           * ([MS-RDPEI])
           */
          touch_context->ignore_contact = TRUE;
        }

      touch_context->contact_state = CONTACT_STATE_ENGAGED;
    }
  else if (touch_contact_data->contactFlags == (RDPINPUT_CONTACT_FLAG_UPDATE |
                                                RDPINPUT_CONTACT_FLAG_INRANGE))
    {
      touch_context->contact_state = CONTACT_STATE_HOVERING;
    }
  else if (touch_contact_data->contactFlags == (RDPINPUT_CONTACT_FLAG_UPDATE |
                                                RDPINPUT_CONTACT_FLAG_CANCELED))
    {
      queue_touch_contact_disposal (input, touch_context);

      touch_context->contact_state = CONTACT_STATE_OUT_OF_RANGE;
    }
  else if (touch_contact_data->contactFlags == RDPINPUT_CONTACT_FLAG_UPDATE)
    {
      queue_touch_contact_disposal (input, touch_context);

      touch_context->contact_state = CONTACT_STATE_OUT_OF_RANGE;
    }
  else
    {
      g_warning ("[RDP.INPUT] Protocol violation: Client sent invalid contact "
                 "flags 0x%08X in state 'Hovering' for contact %u",
                 touch_contact_data->contactFlags, contact_id);
    }
}

/*
 * Automata based on Touch Contact State Transitions
 * ([MS-RDPEI] 3.1.1.1)
 */
static void
handle_touch_state_engaged (GrdRdpDvcInput        *input,
                            RDPINPUT_CONTACT_DATA *touch_contact_data)
{
  uint8_t contact_id = touch_contact_data->contactId;
  TouchContext *touch_context = &input->touch_contexts[contact_id];
  GrdSession *session = input->session;

  g_assert (touch_context->touch_contact);

  if (touch_contact_data->contactFlags == (RDPINPUT_CONTACT_FLAG_UPDATE |
                                           RDPINPUT_CONTACT_FLAG_INRANGE |
                                           RDPINPUT_CONTACT_FLAG_INCONTACT))
    {
      GrdRdpLayoutManager *layout_manager = input->layout_manager;
      GrdEventMotionAbs motion_abs = {};
      GrdStream *stream = NULL;

      if (touch_context->ignore_contact ||
          !grd_rdp_layout_manager_transform_position (layout_manager,
                                                      touch_contact_data->x,
                                                      touch_contact_data->y,
                                                      &stream, &motion_abs))
        return;

      grd_session_notify_touch_motion (session, touch_context->touch_contact,
                                       stream, &motion_abs);
      ensure_queued_touch_device_frame (input);

      touch_context->contact_state = CONTACT_STATE_ENGAGED;
    }
  else if (touch_contact_data->contactFlags == (RDPINPUT_CONTACT_FLAG_UP |
                                                RDPINPUT_CONTACT_FLAG_INRANGE))
    {
      if (!touch_context->ignore_contact)
        {
          grd_session_notify_touch_up (session, touch_context->touch_contact);
          ensure_queued_touch_device_frame (input);
        }

      queue_touch_contact_disposal (input, touch_context);
      touch_context->touch_contact =
        grd_session_acquire_touch_contact (session);

      touch_context->contact_state = CONTACT_STATE_HOVERING;
    }
  else if (touch_contact_data->contactFlags == (RDPINPUT_CONTACT_FLAG_UP |
                                                RDPINPUT_CONTACT_FLAG_CANCELED))
    {
      if (!touch_context->ignore_contact)
        {
          grd_session_notify_touch_cancel (session, touch_context->touch_contact);
          ensure_queued_touch_device_frame (input);
        }
      queue_touch_contact_disposal (input, touch_context);

      touch_context->contact_state = CONTACT_STATE_OUT_OF_RANGE;
    }
  else if (touch_contact_data->contactFlags == RDPINPUT_CONTACT_FLAG_UP)
    {
      if (!touch_context->ignore_contact)
        {
          grd_session_notify_touch_up (session, touch_context->touch_contact);
          ensure_queued_touch_device_frame (input);
        }
      queue_touch_contact_disposal (input, touch_context);

      touch_context->contact_state = CONTACT_STATE_OUT_OF_RANGE;
    }
  else
    {
      g_warning ("[RDP.INPUT] Protocol violation: Client sent invalid contact "
                 "flags 0x%08X in state 'Engaged' for contact %u",
                 touch_contact_data->contactFlags, contact_id);
    }
}

static void
process_touch_contact (GrdRdpDvcInput        *input,
                       RDPINPUT_CONTACT_DATA *touch_contact_data)
{
  uint8_t contact_id = touch_contact_data->contactId;
  TouchContext *touch_context = &input->touch_contexts[contact_id];

  /*
   * The optional contact-rect, orientation, and pressure fields are unhandled
   * due to missing libei-API to submit them
   */

  switch (touch_context->contact_state)
    {
    case CONTACT_STATE_OUT_OF_RANGE:
      handle_touch_state_out_of_range (input, touch_contact_data);
      break;
    case CONTACT_STATE_HOVERING:
      handle_touch_state_hovering (input, touch_contact_data);
      break;
    case CONTACT_STATE_ENGAGED:
      handle_touch_state_engaged (input, touch_contact_data);
      break;
    }
}

static void
maybe_notify_touch_device_frame (GrdRdpDvcInput *input)
{
  GrdSession *session = input->session;

  if (!input->pending_touch_device_frame)
    return;

  grd_session_notify_touch_device_frame (session);
  input->pending_touch_device_frame = FALSE;
}

static void
dispose_touch_contacts (GrdRdpDvcInput *input)
{
  GrdSession *session = input->session;
  GList *l;

  for (l = input->touch_contacts_to_dispose; l; l = l->next)
    {
      GrdTouchContact *touch_contact = l->data;

      grd_session_release_touch_contact (session, touch_contact);
    }
  g_clear_pointer (&input->touch_contacts_to_dispose, g_list_free);
}

static void
process_touch_frame (GrdRdpDvcInput       *input,
                     RDPINPUT_TOUCH_FRAME *touch_frame)
{
  uint32_t i;

  for (i = 0; i < touch_frame->contactCount; ++i)
    process_touch_contact (input, &touch_frame->contacts[i]);

  maybe_notify_touch_device_frame (input);
  dispose_touch_contacts (input);
}

static void
dismiss_touch_contact (GrdRdpDvcInput *input,
                       uint8_t         contact_id)
{
  TouchContext *touch_context = &input->touch_contexts[contact_id];

  if (!touch_context->touch_contact)
    g_assert (touch_context->contact_state == CONTACT_STATE_OUT_OF_RANGE);

  /* Client did not keep track of touch contact properly */
  if (!touch_context->touch_contact)
    return;

  g_assert (touch_context->contact_state != CONTACT_STATE_OUT_OF_RANGE);

  if (touch_context->contact_state == CONTACT_STATE_ENGAGED)
    {
      GrdSession *session = input->session;

      grd_session_notify_touch_cancel (session, touch_context->touch_contact);
      ensure_queued_touch_device_frame (input);
    }
  queue_touch_contact_disposal (input, touch_context);

  maybe_notify_touch_device_frame (input);
  dispose_touch_contacts (input);

  touch_context->contact_state = CONTACT_STATE_OUT_OF_RANGE;
}

static void
input_event_free (InputEvent *input_event)
{
  g_clear_pointer (&input_event->touch_frame.contacts, g_free);

  g_free (input_event);
}

static gboolean
handle_input_events (gpointer user_data)
{
  GrdRdpDvcInput *input = user_data;
  InputEvent *input_event;

  if (!grd_session_has_touch_device (input->session))
    return G_SOURCE_CONTINUE;

  while ((input_event = g_async_queue_try_pop (input->event_queue)))
    {
      switch (input_event->event_type)
        {
        case INPUT_EVENT_TYPE_TOUCH:
          process_touch_frame (input, &input_event->touch_frame);
          break;
        case INPUT_EVENT_TYPE_DISMISS_HOVERING_TOUCH_CONTACT:
          dismiss_touch_contact (input, input_event->touch_contact_to_dismiss);
          break;
        case INPUT_EVENT_TYPE_PEN:
          /* Missing libei-API to submit (multi-)pen events */
          g_assert_not_reached ();
          break;
        }

      input_event_free (input_event);
    }

  return G_SOURCE_CONTINUE;
}

static void
grd_rdp_dvc_input_dispose (GObject *object)
{
  GrdRdpDvcInput *input = GRD_RDP_DVC_INPUT (object);
  GrdRdpDvc *dvc = GRD_RDP_DVC (input);
  uint16_t i;

  if (input->channel_opened)
    {
      input->rdpei_context->Close (input->rdpei_context);
      input->channel_opened = FALSE;
    }
  grd_rdp_dvc_maybe_unsubscribe_creation_status (dvc);

  if (input->event_source)
    {
      g_source_destroy (input->event_source);
      g_clear_pointer (&input->event_source, g_source_unref);
    }

  handle_input_events (input);
  g_clear_pointer (&input->event_queue, g_async_queue_unref);

  g_assert (!input->touch_contacts_to_dispose);

  for (i = 0; i < MAX_TOUCH_CONTACTS; ++i)
    {
      TouchContext *touch_context = &input->touch_contexts[i];
      GrdTouchContact *touch_contact;

      touch_contact = g_steal_pointer (&touch_context->touch_contact);
      if (touch_contact)
        grd_session_release_touch_contact (input->session, touch_contact);
    }

  g_clear_pointer (&input->rdpei_context,
                   rdpei_server_context_free);

  G_OBJECT_CLASS (grd_rdp_dvc_input_parent_class)->dispose (object);
}

static gboolean
event_source_dispatch (GSource     *source,
                       GSourceFunc  callback,
                       gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs event_source_funcs =
{
  .dispatch = event_source_dispatch,
};

static void
grd_rdp_dvc_input_init (GrdRdpDvcInput *input)
{
  GSource *event_source;

  input->event_queue = g_async_queue_new ();

  event_source = g_source_new (&event_source_funcs, sizeof (GSource));
  g_source_set_callback (event_source, handle_input_events,
                         input, NULL);
  g_source_set_ready_time (event_source, -1);
  g_source_attach (event_source, NULL);
  input->event_source = event_source;
}

static void
grd_rdp_dvc_input_class_init (GrdRdpDvcInputClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdRdpDvcClass *dvc_class = GRD_RDP_DVC_CLASS (klass);

  object_class->dispose = grd_rdp_dvc_input_dispose;

  dvc_class->maybe_init = grd_rdp_dvc_input_maybe_init;
}
