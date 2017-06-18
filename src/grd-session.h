/*
 * Copyright (C) 2015 Red Hat Inc.
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
 *
 * Written by:
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#ifndef GRD_SESSION_H
#define GRD_SESSION_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_SESSION (grd_session_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdSession, grd_session, GRD, SESSION, GObject);

typedef enum _GrdKeyState
{
  GRD_KEY_STATE_RELEASED,
  GRD_KEY_STATE_PRESSED
} GrdKeyState;

typedef enum _GrdButtonState
{
  GRD_BUTTON_STATE_RELEASED,
  GRD_BUTTON_STATE_PRESSED
} GrdButtonState;

typedef enum _GrdPointerAxis
{
  GRD_POINTER_AXIS_VERTICAL,
  GRD_POINTER_AXIS_HORIZONTAL
} GrdPointerAxis;

struct _GrdSessionClass
{
  GObjectClass parent_class;

  void (*stream_ready) (GrdSession *session,
                        GrdStream  *stream);
  void (*stop) (GrdSession *session);
};

GrdContext *grd_session_get_context (GrdSession *session);

void grd_session_notify_keyboard_keysym (GrdSession *session,
                                         uint32_t    keysym,
                                         GrdKeyState state);

void grd_session_notify_pointer_button (GrdSession    *session,
                                        int32_t        button,
                                        GrdButtonState state);

void grd_session_notify_pointer_axis_discrete (GrdSession    *session,
                                               GrdPointerAxis axis,
                                               int            steps);

void grd_session_notify_pointer_motion_absolute (GrdSession *session,
                                                 double      x,
                                                 double      y);

void grd_session_stop (GrdSession *session);

#endif /* GRD_SESSION_H */
