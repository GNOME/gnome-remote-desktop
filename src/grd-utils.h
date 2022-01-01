/*
 * Copyright (C) 2022 Pascal Nowack
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

#ifndef GRD_UTILS_H
#define GRD_UTILS_H

#include <gio/gio.h>

typedef struct _GrdSyncPoint
{
  GCond sync_cond;
  GMutex sync_mutex;
  gboolean completed;

  gboolean success;
} GrdSyncPoint;

void grd_sync_point_init (GrdSyncPoint *sync_point);

void grd_sync_point_clear (GrdSyncPoint *sync_point);

void grd_sync_point_complete (GrdSyncPoint *sync_point,
                              gboolean      success);

gboolean grd_sync_point_wait_for_completion (GrdSyncPoint *sync_point);

#endif /* GRD_UTILS_H */
