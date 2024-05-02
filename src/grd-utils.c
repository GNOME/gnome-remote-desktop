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

#include <gio/gunixfdlist.h>
#include <gio/gunixinputstream.h>
#include <glib/gstdio.h>

#define GRD_SERVER_PORT_RANGE 10

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

gboolean
grd_bind_socket (GSocketListener  *server,
                 uint16_t          port,
                 uint16_t         *selected_port,
                 gboolean          negotiate_port,
                 GError          **error)
{
  gboolean is_bound = FALSE;
  int i;

  if (!negotiate_port)
    {
      is_bound = g_socket_listener_add_inet_port (server,
                                                  port,
                                                  NULL,
                                                  error);
      goto out;
    }

  for (i = 0; i < GRD_SERVER_PORT_RANGE; i++, port++)
    {
      g_autoptr (GError) local_error = NULL;

      g_assert (port < G_MAXUINT16);

      is_bound = g_socket_listener_add_inet_port (server,
                                                  port,
                                                  NULL,
                                                  &local_error);
      if (local_error)
        {
          g_debug ("\tServer could not be bound to TCP port %hu: %s",
                   port, local_error->message);
        }

      if (is_bound)
        break;
    }

  if (!is_bound)
    port = g_socket_listener_add_any_inet_port (server, NULL, error);

  is_bound = port != 0;

out:
  if (is_bound)
    {
      g_debug ("\tServer bound to TCP port %hu", port);
      *selected_port = port;
    }

  return is_bound;
}

void
grd_rewrite_path_to_user_data_dir (char       **path,
                                   const char  *subdir,
                                   const char  *fallback_path)
{
  const char *input_path = NULL;
  g_autofree char *basename = NULL;
  g_autofree char *output_path = NULL;

  g_assert (path);
  g_assert (subdir);
  g_assert (fallback_path);
  g_assert (strstr (subdir, "..") == NULL);

  input_path = *path;

  if (!input_path ||
      input_path[0] == '\0' ||
      g_str_equal (input_path, ".") ||
      G_IS_DIR_SEPARATOR (input_path[strlen (input_path) - 1]))
    input_path = fallback_path;

  basename = g_path_get_basename (input_path);

  g_assert (!G_IS_DIR_SEPARATOR (basename[0]));

  output_path =  g_build_filename (g_get_user_data_dir (),
                                   "gnome-remote-desktop",
                                   subdir,
                                   basename,
                                   NULL);

  g_free (*path);
  *path = g_steal_pointer (&output_path);
}

gboolean
grd_write_fd_to_file (int            fd,
                      const char    *filename,
                      GCancellable  *cancellable,
                      GError       **error)
{
  g_autoptr (GInputStream) input_stream = NULL;
  g_autoptr (GFileOutputStream) output_stream = NULL;
  g_autoptr (GFile) file = NULL;
  g_autofree char *dir = g_path_get_dirname (filename);

  g_mkdir_with_parents (dir, 0755);

  input_stream = G_INPUT_STREAM (g_unix_input_stream_new (fd, FALSE));

  file = g_file_new_for_path (filename);
  output_stream = g_file_replace (file, NULL, TRUE, G_FILE_CREATE_PRIVATE,
                                  NULL, error);
  if (!output_stream)
    return FALSE;

  return g_output_stream_splice (G_OUTPUT_STREAM(output_stream), input_stream,
                                 G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                 G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                 cancellable,
                                 error);
}

gboolean
grd_test_fd (int         fd,
             ssize_t     max_size,
             GFileTest  *test_results,
             GError    **error)
{
  struct stat stat_results;
  int ret;

  do
    ret = fstat (fd, &stat_results);
  while (ret < 0 && errno == EINTR);

  if (ret < 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Failed to fstat file descriptor: %s", g_strerror (errno));
      return FALSE;
    }

  if (max_size >= 0 && S_ISREG (stat_results.st_mode) &&
      stat_results.st_size > max_size)
   {
      g_autofree char *size_string = NULL;

      size_string = g_format_size_full (max_size, G_FORMAT_SIZE_LONG_FORMAT);
      g_set_error (error, G_IO_ERROR, G_FILE_ERROR_FAILED,
                    "File size exceeds %s", size_string);

      return FALSE;
    }

  *test_results = 0;
  if (S_ISREG (stat_results.st_mode))
    *test_results |= G_FILE_TEST_IS_REGULAR;
  else if (S_ISDIR (stat_results.st_mode))
    *test_results |= G_FILE_TEST_IS_DIR;
  else if (S_ISLNK (stat_results.st_mode))
    *test_results |= G_FILE_TEST_IS_SYMLINK;

  if (stat_results.st_nlink > 0)
    *test_results |= G_FILE_TEST_EXISTS;

  if ((stat_results.st_mode & S_IXUSR) || (stat_results.st_mode & S_IXGRP) ||
      (stat_results.st_mode & S_IXOTH))
    *test_results |= G_FILE_TEST_IS_EXECUTABLE;

  return TRUE;
}
