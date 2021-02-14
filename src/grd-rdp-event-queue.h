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

#ifndef GRD_RDP_EVENT_QUEUE_H
#define GRD_RDP_EVENT_QUEUE_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-session.h"
#include "grd-types.h"

#define GRD_TYPE_RDP_EVENT_QUEUE (grd_rdp_event_queue_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpEventQueue, grd_rdp_event_queue,
                      GRD, RDP_EVENT_QUEUE, GObject);

GrdRdpEventQueue *grd_rdp_event_queue_new (GrdSessionRdp *session_rdp);

void grd_rdp_event_queue_add_input_event_keyboard_keycode (GrdRdpEventQueue *rdp_event_queue,
                                                           uint32_t          keycode,
                                                           GrdKeyState       state);

void grd_rdp_event_queue_add_input_event_keyboard_keysym (GrdRdpEventQueue *rdp_event_queue,
                                                          uint32_t          keysym,
                                                          GrdKeyState       state);

void grd_rdp_event_queue_add_input_event_pointer_motion_abs (GrdRdpEventQueue *rdp_event_queue,
                                                             double            x,
                                                             double            y);

void grd_rdp_event_queue_add_input_event_pointer_button (GrdRdpEventQueue *rdp_event_queue,
                                                         int32_t           button,
                                                         GrdButtonState    state);

void grd_rdp_event_queue_add_input_event_pointer_axis (GrdRdpEventQueue    *rdp_event_queue,
                                                       double               dx,
                                                       double               dy,
                                                       GrdPointerAxisFlags  flags);

#endif /* GRD_RDP_EVENT_QUEUE_H */
