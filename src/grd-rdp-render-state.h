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

#include <glib.h>
#include <stdint.h>

#include "grd-types.h"

GrdRdpRenderState *grd_rdp_render_state_new (uint32_t *damage_buffer,
                                             uint32_t *chroma_state_buffer,
                                             uint32_t  state_buffer_length);

void grd_rdp_render_state_free (GrdRdpRenderState *render_state);

uint32_t *grd_rdp_render_state_get_damage_buffer (GrdRdpRenderState *render_state);

uint32_t *grd_rdp_render_state_get_chroma_state_buffer (GrdRdpRenderState *render_state);

uint32_t grd_rdp_render_state_get_state_buffer_length (GrdRdpRenderState *render_state);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdRdpRenderState, grd_rdp_render_state_free)
