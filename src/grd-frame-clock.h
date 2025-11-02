/*
 * Copyright (C) 2025 Pascal Nowack
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

#include <glib-object.h>
#include <stdint.h>

#define GRD_TYPE_FRAME_CLOCK (grd_frame_clock_get_type ())
G_DECLARE_FINAL_TYPE (GrdFrameClock, grd_frame_clock,
                      GRD, FRAME_CLOCK, GObject)

typedef void (* GrdFrameClockCallback) (gpointer user_data);

GrdFrameClock *grd_frame_clock_new (GMainContext           *main_context,
                                    GrdFrameClockCallback   on_trigger,
                                    gpointer                on_trigger_user_data,
                                    GError                **error);

gboolean grd_frame_clock_is_armed (GrdFrameClock *frame_clock);

void grd_frame_clock_arm_timer (GrdFrameClock *frame_clock,
                                uint64_t       clock_rate_num,
                                uint64_t       clock_rate_denom);

void grd_frame_clock_disarm_timer (GrdFrameClock *frame_clock);
