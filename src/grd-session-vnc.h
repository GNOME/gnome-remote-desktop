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
                      GrdSession)

GrdSessionVnc *grd_session_vnc_new (GrdVncServer      *vnc_server,
                                    GSocketConnection *connection);

void grd_session_vnc_queue_resize_framebuffer (GrdSessionVnc *session_vnc,
                                               int            width,
                                               int            height);

void grd_session_vnc_take_buffer (GrdSessionVnc *session_vnc,
                                  void          *data);

void grd_session_vnc_flush (GrdSessionVnc *session_vnc);

void grd_session_vnc_set_cursor (GrdSessionVnc *session_vnc,
                                 rfbCursorPtr   rfb_cursor);

void grd_session_vnc_move_cursor (GrdSessionVnc *session_vnc,
                                  int            x,
                                  int            y);

void grd_session_vnc_set_client_clipboard_text (GrdSessionVnc *session_vnc,
                                                char          *text,
                                                int            text_length);

int grd_session_vnc_get_stride_for_width (GrdSessionVnc *session_vnc,
                                          int            width);

gboolean grd_session_vnc_is_client_gone (GrdSessionVnc *session_vnc);

#endif /* GRD_SESSION_VNC_H */
