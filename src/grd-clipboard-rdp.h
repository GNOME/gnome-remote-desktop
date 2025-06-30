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

#pragma once

#include <freerdp/server/cliprdr.h>

#include "grd-clipboard.h"

#define GRD_TYPE_CLIPBOARD_RDP (grd_clipboard_rdp_get_type ())
G_DECLARE_FINAL_TYPE (GrdClipboardRdp,
                      grd_clipboard_rdp,
                      GRD, CLIPBOARD_RDP,
                      GrdClipboard)

GrdClipboardRdp *grd_clipboard_rdp_new (GrdSessionRdp *session_rdp,
                                        HANDLE         vcm,
                                        gboolean       relieve_filename_restriction);

void grd_clipboard_rdp_lock_remote_clipboard_data (GrdClipboardRdp *clipboard_rdp,
                                                   uint32_t         clip_data_id);

void grd_clipboard_rdp_unlock_remote_clipboard_data (GrdClipboardRdp *clipboard_rdp,
                                                     uint32_t         clip_data_id);

void grd_clipboard_rdp_request_remote_file_size_async (GrdClipboardRdp *clipboard_rdp,
                                                       uint32_t         stream_id,
                                                       uint32_t         list_index,
                                                       gboolean         has_clip_data_id,
                                                       uint32_t         clip_data_id);

void grd_clipboard_rdp_request_remote_file_range_async (GrdClipboardRdp *clipboard_rdp,
                                                        uint32_t         stream_id,
                                                        uint32_t         list_index,
                                                        uint64_t         offset,
                                                        uint32_t         requested_size,
                                                        gboolean         has_clip_data_id,
                                                        uint32_t         clip_data_id);
