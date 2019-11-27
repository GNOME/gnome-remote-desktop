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

#ifndef GRD_SESSION_VNC_H
#define GRD_SESSION_VNC_H

#include <gio/gio.h>
#include <glib-object.h>
#include <rfb/rfb.h>

#include "grd-session.h"
#include "grd-types.h"

#define GRD_TYPE_SESSION_VNC (grd_session_vnc_get_type ())
G_DECLARE_FINAL_TYPE (GrdSessionVnc,
                      grd_session_vnc,
                      GRD, SESSION_VNC,
                      GrdSession);

typedef gboolean (* GrdVncSocketGrabFunc) (GrdSessionVnc  *session_vnc,
                                           GError        **error);

GrdSessionVnc *grd_session_vnc_new (GrdVncServer      *vnc_server,
                                    GSocketConnection *connection);

void grd_session_vnc_queue_resize_framebuffer (GrdSessionVnc *session_vnc,
                                               int            width,
                                               int            height);

void grd_session_vnc_take_buffer (GrdSessionVnc *session_vnc,
                                  void          *data);

void grd_session_vnc_set_cursor (GrdSessionVnc *session_vnc,
                                 rfbCursorPtr   rfb_cursor);

void grd_session_vnc_move_cursor (GrdSessionVnc *session_vnc,
                                  int            x,
                                  int            y);

int grd_session_vnc_get_fd (GrdSessionVnc *session_vnc);

int grd_session_vnc_get_framebuffer_stride (GrdSessionVnc *session_vnc);

rfbClientPtr grd_session_vnc_get_rfb_client (GrdSessionVnc *session_vnc);

void grd_session_vnc_grab_socket (GrdSessionVnc        *session_vnc,
                                  GrdVncSocketGrabFunc  grab_func);

void grd_session_vnc_ungrab_socket (GrdSessionVnc        *session_vnc,
                                    GrdVncSocketGrabFunc  grab_func);

gboolean grd_session_vnc_is_paused (GrdSessionVnc *session_vnc);

void grd_session_vnc_dispatch (GrdSessionVnc *session_vnc);

GrdVncServer * grd_session_vnc_get_vnc_server (GrdSessionVnc *session_vnc);

#endif /* GRD_SESSION_VNC_H */
