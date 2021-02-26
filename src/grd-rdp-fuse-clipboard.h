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

#ifndef GRD_RDP_FUSE_CLIPBOARD_H
#define GRD_RDP_FUSE_CLIPBOARD_H

#include <glib-object.h>
#include <winpr2/winpr/shell.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_FUSE_CLIPBOARD (grd_rdp_fuse_clipboard_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpFuseClipboard, grd_rdp_fuse_clipboard,
                      GRD, RDP_FUSE_CLIPBOARD, GObject);

GrdRdpFuseClipboard *grd_rdp_fuse_clipboard_new (GrdClipboardRdp *clipboard_rdp,
                                                 const char      *mount_path);

void grd_rdp_fuse_clipboard_dismiss_all_requests (GrdRdpFuseClipboard *rdp_fuse_clipboard);

void grd_rdp_fuse_clipboard_clear_selection (GrdRdpFuseClipboard *rdp_fuse_clipboard);

#ifdef HAVE_FREERDP_2_3
gboolean grd_rdp_fuse_clipboard_set_selection (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                                               FILEDESCRIPTORW     *files,
                                               uint32_t             n_files);
#else
gboolean grd_rdp_fuse_clipboard_set_selection (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                                               FILEDESCRIPTOR      *files,
                                               uint32_t             n_files);
#endif /* HAVE_FREERDP_2_3 */

void grd_rdp_fuse_clipboard_submit_file_contents_response (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                                                           uint32_t             stream_id,
                                                           gboolean             response_ok,
                                                           const uint8_t       *data,
                                                           uint32_t             size);

#endif /* GRD_RDP_FUSE_CLIPBOARD_H */
