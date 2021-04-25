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

#ifndef GRD_CLIPBOARD_H
#define GRD_CLIPBOARD_H

#include <glib-object.h>
#include <stdint.h>

#include "grd-mime-type.h"
#include "grd-types.h"

#define GRD_TYPE_CLIPBOARD (grd_clipboard_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdClipboard, grd_clipboard, GRD, CLIPBOARD, GObject);

struct _GrdClipboardClass
{
  GObjectClass parent_class;

  void (*update_client_mime_type_list) (GrdClipboard *clipboard,
                                        GList        *mime_type_list);
  uint8_t *(*request_client_content_for_mime_type) (GrdClipboard     *clipboard,
                                                    GrdMimeTypeTable *mime_type_table,
                                                    uint32_t         *size);
  void (*submit_requested_server_content) (GrdClipboard *clipboard,
                                           uint8_t      *data,
                                           uint32_t      size);
};

void grd_clipboard_update_server_mime_type_list (GrdClipboard *clipboard,
                                                 GList        *mime_type_tables);

void grd_clipboard_request_server_content_for_mime_type_async (GrdClipboard *clipboard,
                                                               GrdMimeType   mime_type);

void grd_clipboard_initialize (GrdClipboard *clipboard,
                               GrdSession   *session);

void grd_clipboard_maybe_enable_clipboard (GrdClipboard *clipboard);

void grd_clipboard_disable_clipboard (GrdClipboard *clipboard);

void grd_clipboard_update_client_mime_type_list (GrdClipboard *clipboard,
                                                 GList        *mime_type_list);

uint8_t *grd_clipboard_request_client_content_for_mime_type (GrdClipboard *clipboard,
                                                             GrdMimeType   mime_type,
                                                             uint32_t     *size);

#endif /* GRD_CLIPBOARD_H */
