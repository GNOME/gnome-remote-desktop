/*
 * Copyright (C) 2015,2022 Red Hat Inc.
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
 */

#pragma once

typedef enum _GrdDebugFlags
{
  GRD_DEBUG_NONE = 0,
  GRD_DEBUG_VNC = 1 << 0,
  GRD_DEBUG_TPM = 1 << 1,
  GRD_DEBUG_VK_VALIDATION = 1 << 2,
  GRD_DEBUG_VK_TIMES = 1 << 3,
  GRD_DEBUG_VA_TIMES = 1 << 4,
  GRD_DEBUG_VKVA = 1 << 5,
} GrdDebugFlags;

GrdDebugFlags grd_get_debug_flags (void);
