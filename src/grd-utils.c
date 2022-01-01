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

#include "config.h"

#include "grd-utils.h"

void
grd_sync_point_init (GrdSyncPoint *sync_point)
{
  g_cond_init (&sync_point->sync_cond);
  g_mutex_init (&sync_point->sync_mutex);
}

void
grd_sync_point_clear (GrdSyncPoint *sync_point)
{
  g_cond_clear (&sync_point->sync_cond);
  g_mutex_clear (&sync_point->sync_mutex);
}

void
grd_sync_point_complete (GrdSyncPoint *sync_point,
                         gboolean      success)
{
  g_return_if_fail (!sync_point->completed);

  g_mutex_lock (&sync_point->sync_mutex);
  sync_point->success = success;

  sync_point->completed = TRUE;
  g_cond_signal (&sync_point->sync_cond);
  g_mutex_unlock (&sync_point->sync_mutex);
}

gboolean
grd_sync_point_wait_for_completion (GrdSyncPoint *sync_point)
{
  gboolean success;

  g_mutex_lock (&sync_point->sync_mutex);
  while (!sync_point->completed)
    g_cond_wait (&sync_point->sync_cond, &sync_point->sync_mutex);

  success = sync_point->success;
  g_mutex_unlock (&sync_point->sync_mutex);

  return success;
}
