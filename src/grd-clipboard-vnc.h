/*
 * Copyright (C) 2020 Pascal Nowack
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

#ifndef GRD_CLIPBOARD_VNC_H
#define GRD_CLIPBOARD_VNC_H

#include "grd-clipboard.h"

#define GRD_TYPE_CLIPBOARD_VNC (grd_clipboard_vnc_get_type ())
G_DECLARE_FINAL_TYPE (GrdClipboardVnc,
                      grd_clipboard_vnc,
                      GRD, CLIPBOARD_VNC,
                      GrdClipboard);

GrdClipboardVnc *grd_clipboard_vnc_new (GrdSessionVnc *session_vnc);

void grd_clipboard_vnc_maybe_enable_clipboard (GrdClipboardVnc *clipboard_vnc);

void grd_clipboard_vnc_set_clipboard_text (GrdClipboardVnc *clipboard_vnc,
                                           char            *text,
                                           int              text_length);

#endif /* GRD_CLIPBOARD_VNC_H */
