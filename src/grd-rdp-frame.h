/*
 * Copyright (C) 2024 Pascal Nowack
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

#include <cairo/cairo.h>
#include <glib.h>

#include "grd-types.h"

typedef enum
{
  GRD_RDP_FRAME_VIEW_TYPE_DUAL,
  GRD_RDP_FRAME_VIEW_TYPE_MAIN,
  GRD_RDP_FRAME_VIEW_TYPE_AUX,
} GrdRdpFrameViewType;

typedef void (* GrdRdpFrameCallback) (GrdRdpFrame *rdp_frame,
                                      gpointer     user_data);

GrdRdpFrame *grd_rdp_frame_new (GrdRdpRenderContext *render_context,
                                GrdRdpBuffer        *src_buffer_new,
                                GrdRdpBuffer        *src_buffer_old,
                                GrdRdpFrameCallback  frame_picked_up,
                                GrdRdpFrameCallback  view_finalized,
                                GrdRdpFrameCallback  frame_submitted,
                                GrdRdpFrameCallback  frame_finalized,
                                gpointer             callback_user_data,
                                GDestroyNotify       user_data_destroy);

void grd_rdp_frame_free (GrdRdpFrame *rdp_frame);

GrdRdpRenderer *grd_rdp_frame_get_renderer (GrdRdpFrame *rdp_frame);

GrdRdpRenderContext *grd_rdp_frame_get_render_context (GrdRdpFrame *rdp_frame);

GrdEncodeContext *grd_rdp_frame_get_encode_context (GrdRdpFrame *rdp_frame);

GList *grd_rdp_frame_get_image_views (GrdRdpFrame *rdp_frame);

GrdRdpBuffer *grd_rdp_frame_get_source_buffer (GrdRdpFrame *rdp_frame);

GrdRdpBuffer *grd_rdp_frame_get_last_source_buffer (GrdRdpFrame *rdp_frame);

GrdRdpFrameViewType grd_rdp_frame_get_avc_view_type (GrdRdpFrame *rdp_frame);

cairo_region_t *grd_rdp_frame_get_damage_region (GrdRdpFrame *rdp_frame);

GList *grd_rdp_frame_get_bitstreams (GrdRdpFrame *rdp_frame);

gboolean grd_rdp_frame_has_valid_view (GrdRdpFrame *rdp_frame);

gboolean grd_rdp_frame_is_surface_damaged (GrdRdpFrame *rdp_frame);

void grd_rdp_frame_set_renderer (GrdRdpFrame    *rdp_frame,
                                 GrdRdpRenderer *renderer);

void grd_rdp_frame_set_avc_view_type (GrdRdpFrame         *rdp_frame,
                                      GrdRdpFrameViewType  view_type);

void grd_rdp_frame_set_damage_region (GrdRdpFrame    *rdp_frame,
                                      cairo_region_t *damage_region);

void grd_rdp_frame_set_bitstreams (GrdRdpFrame *rdp_frame,
                                   GList       *bitstreams);

void grd_rdp_frame_notify_picked_up (GrdRdpFrame *rdp_frame);

void grd_rdp_frame_notify_frame_submission (GrdRdpFrame *rdp_frame);

GrdImageView *grd_rdp_frame_pop_image_view (GrdRdpFrame *rdp_frame);
