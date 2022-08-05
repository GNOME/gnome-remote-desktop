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

#ifndef GRD_DEBUG_H
#define GRD_DEBUG_H

typedef enum _GrdDebugFlags
{
  GRD_DEBUG_NONE = 0,
  GRD_DEBUG_VNC = 1 << 0,
} GrdDebugFlags;

GrdDebugFlags grd_get_debug_flags (void);

#endif /* GRD_DEBUG_H */
