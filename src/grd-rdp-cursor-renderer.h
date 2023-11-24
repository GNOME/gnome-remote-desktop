/*
 * Copyright (C) 2023 Pascal Nowack
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

#ifndef GRD_RDP_CURSOR_RENDERER_H
#define GRD_RDP_CURSOR_RENDERER_H

#include <freerdp/freerdp.h>
#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_CURSOR_RENDERER (grd_rdp_cursor_renderer_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpCursorRenderer, grd_rdp_cursor_renderer,
                      GRD, RDP_CURSOR_RENDERER, GObject)

typedef enum
{
  GRD_RDP_CURSOR_UPDATE_TYPE_DEFAULT = 0,
  GRD_RDP_CURSOR_UPDATE_TYPE_HIDDEN,
  GRD_RDP_CURSOR_UPDATE_TYPE_NORMAL,
} GrdRdpCursorUpdateType;

typedef struct
{
  GrdRdpCursorUpdateType update_type;

  uint16_t hotspot_x;
  uint16_t hotspot_y;
  uint16_t width;
  uint16_t height;
  uint8_t *bitmap;
} GrdRdpCursorUpdate;

GrdRdpCursorRenderer *grd_rdp_cursor_renderer_new (GrdRdpRenderer *renderer,
                                                   rdpContext     *rdp_context);

void grd_rdp_cursor_renderer_notify_session_ready (GrdRdpCursorRenderer *cursor_renderer);

void grd_rdp_cursor_renderer_submit_cursor_update (GrdRdpCursorRenderer *cursor_renderer,
                                                   GrdRdpCursorUpdate   *cursor_update);

#endif /* GRD_RDP_CURSOR_RENDERER_H */
