/*
 * Copyright (C) 2010 Intel Corp.
 * Copyright (C) 2014 Jonas Ådahl
 * Copyright (C) 2016-2022 Red Hat Inc.
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

typedef struct _GrdFdSource
{
  GSource source;

  GSourceFunc prepare;
  GSourceFunc dispatch;
  gpointer user_data;

  GPollFD poll_fd;
} GrdFdSource;

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

static gboolean
grd_fd_source_prepare (GSource *source,
                       int     *timeout_ms)
{
  GrdFdSource *fd_source = (GrdFdSource *) source;

  *timeout_ms = -1;

  return fd_source->prepare (fd_source->user_data);
}

static gboolean
grd_fd_source_check (GSource *source)
{
  GrdFdSource *fd_source = (GrdFdSource *) source;

  return !!(fd_source->poll_fd.revents & G_IO_IN);
}

static gboolean
grd_fd_source_dispatch (GSource     *source,
                        GSourceFunc  callback,
                        gpointer     user_data)
{
  GrdFdSource *fd_source = (GrdFdSource *) source;

  return fd_source->dispatch (fd_source->user_data);
}

static void
grd_fd_source_finalize (GSource *source)
{
  GrdFdSource *fd_source = (GrdFdSource *) source;

  close (fd_source->poll_fd.fd);
}

static GSourceFuncs fd_source_funcs =
{
  .prepare = grd_fd_source_prepare,
  .check = grd_fd_source_check,
  .dispatch = grd_fd_source_dispatch,
  .finalize = grd_fd_source_finalize,
};

GSource *
grd_create_fd_source (int             fd,
                      const char     *name,
                      GSourceFunc     prepare,
                      GSourceFunc     dispatch,
                      gpointer        user_data,
                      GDestroyNotify  notify)
{
  GSource *source;
  GrdFdSource *fd_source;

  source = g_source_new (&fd_source_funcs, sizeof (GrdFdSource));
  g_source_set_name (source, name);
  fd_source = (GrdFdSource *) source;

  fd_source->poll_fd.fd = fd;
  fd_source->poll_fd.events = G_IO_IN;

  fd_source->prepare = prepare;
  fd_source->dispatch = dispatch;
  fd_source->user_data = user_data;

  g_source_set_callback (source, dispatch, user_data, notify);
  g_source_add_poll (source, &fd_source->poll_fd);

  return source;
}
