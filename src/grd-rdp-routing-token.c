/*
 * Copyright (C) 2022 SUSE Software Solutions Germany GmbH
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
 * Written by:
 *     Joan Torres <joan.torres@suse.com>
 */

#include "config.h"

#include "grd-rdp-routing-token.h"

#include <freerdp/freerdp.h>
#include <sys/socket.h>

#define MAX_PEEK_TIME_MS 2000

typedef struct _RoutingTokenContext
{
  GrdRdpServer *rdp_server;
  GSocketConnection *connection;

  GCancellable *cancellable;
  unsigned int abort_peek_source_id;

  GCancellable *server_cancellable;
} RoutingTokenContext;

static int
find_cr_lf (const char *buffer,
            int         length)
{
  int i;

  for (i = 0; i < length; ++i)
    {
      if (buffer[i] == 0x0D && buffer[i + 1] == 0x0A)
        return i;
    }

  return -1;
}

static gboolean
peek_bytes (int            fd,
            uint8_t       *buffer,
            int            length,
            GCancellable  *cancellable,
            GError       **error)
{
  GPollFD poll_fds[2] = {};
  int n_fds = 0;
  int ret;

  poll_fds[n_fds].fd = fd;
  poll_fds[n_fds].events = G_IO_IN;
  n_fds++;

  if (!g_cancellable_make_pollfd (cancellable, &poll_fds[n_fds]))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failure preparing the cancellable for pollfd");
      return FALSE;
    }

  n_fds++;

  do
    {
      do
        ret = g_poll (poll_fds, n_fds, MAX_PEEK_TIME_MS);
      while (ret == -1 && errno == EINTR);

      if (ret == -1)
        {
          g_set_error (error, G_IO_ERROR,
                       g_io_error_from_errno (errno),
                       "On poll command: %s", strerror (errno));
          return FALSE;
        }

      if (g_cancellable_is_cancelled (cancellable))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                       "Cancelled");
          return FALSE;
        }

      do
        ret = recv (fd, (void *) buffer, (size_t) length, MSG_PEEK);
      while (ret == -1 && errno == EINTR);

      if (ret == -1)
        {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            continue;

          g_set_error (error, G_IO_ERROR,
                       g_io_error_from_errno (errno),
                       "On recv command: %s", strerror (errno));
          return FALSE;
        }

    }
  while (ret < length);

  g_cancellable_release_fd (cancellable);

  return TRUE;
}

static char *
get_routing_token_without_prefix (char   *buffer,
                                  size_t  buffer_length)
{
  g_autofree char *peeked_prefix = NULL;
  g_autofree char *prefix = NULL;
  size_t prefix_length;
  size_t routing_token_length;

  prefix = g_strdup ("Cookie: msts=");
  prefix_length = strlen (prefix);

  if (buffer_length < prefix_length)
    return NULL;

  peeked_prefix = g_strndup (buffer, prefix_length);
  if (g_strcmp0 (peeked_prefix, prefix) != 0)
    return NULL;

  routing_token_length = find_cr_lf (buffer, buffer_length);
  if (routing_token_length == -1)
    return NULL;

  return g_strndup (buffer + prefix_length,
                    routing_token_length - prefix_length);
}

static gboolean
peek_routing_token (int            fd,
                    char         **routing_token,
                    GCancellable  *cancellable,
                    GError       **error)
{
  BOOL success = FALSE;
  wStream *s = NULL;

  /* TPKT values */
  uint8_t  version;
  uint16_t tpkt_length;

  /* x224Crq values */
  uint8_t  length_indicator;
  uint8_t  cr_cdt;
  uint16_t dst_ref;
  uint8_t  class_opt;

  /* Peek TPKT Header */
  s = Stream_New (NULL, 4);
  g_assert (s);

  if (!peek_bytes (fd, Stream_Buffer (s), 4, cancellable, error))
    goto out;

  Stream_Read_UINT8 (s, version);
  Stream_Seek (s, 1);
  Stream_Read_UINT16_BE (s, tpkt_length);

  if (version != 3)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "The TPKT Header doesn't have version 3");
      goto out;
    }

  /* Peek full PDU */
  Stream_Free (s, TRUE);
  s = Stream_New (NULL, tpkt_length);
  g_assert (s);

  if (!peek_bytes (fd, Stream_Buffer (s), tpkt_length, cancellable, error))
    goto out;

  Stream_Seek (s, 4);

  /* Check x224Crq */
  Stream_Read_UINT8 (s, length_indicator);
  Stream_Read_UINT8 (s, cr_cdt);
  Stream_Read_UINT16 (s, dst_ref);
  Stream_Seek (s, 2);
  Stream_Read_UINT8 (s, class_opt);
  if (tpkt_length - 5 != length_indicator ||
      cr_cdt != 0xE0 ||
      dst_ref != 0 ||
      (class_opt & 0xFC) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Wrong info on x224Crq");
      goto out;
    }

  /* Check routingToken */
  *routing_token =
    get_routing_token_without_prefix ((char *) Stream_Pointer (s),
                                      Stream_GetRemainingLength (s));

  success = TRUE;

out:
  Stream_Free (s, TRUE);
  return success;
}

static void
peek_routing_token_in_thread (GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
                              GCancellable *cancellable)
{
  RoutingTokenContext *routing_token_context = task_data;
  GSocket *socket;
  int fd;
  char *routing_token;
  GError *error = NULL;

  socket = g_socket_connection_get_socket (routing_token_context->connection);
  fd = g_socket_get_fd (socket);

  if (!peek_routing_token (fd, &routing_token,
                           routing_token_context->cancellable,
                           &error))
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, routing_token, g_free);
}

static gboolean
abort_peek_routing_token (gpointer user_data)
{
  RoutingTokenContext *routing_token_context = user_data;

  g_assert (routing_token_context->abort_peek_source_id);

  g_debug ("RoutingToken: Aborting current peek operation "
           "(Timeout reached)");

  g_cancellable_cancel (routing_token_context->cancellable);

  routing_token_context->abort_peek_source_id = 0;

  return G_SOURCE_REMOVE;
}

static void
clear_routing_token_context (gpointer data)
{
  RoutingTokenContext *routing_token_context = data;

  g_clear_object (&routing_token_context->connection);
  g_clear_object (&routing_token_context->server_cancellable);
  g_clear_object (&routing_token_context->cancellable);
  g_clear_handle_id (&routing_token_context->abort_peek_source_id,
                     g_source_remove);

  g_free (routing_token_context);
}

void
grd_routing_token_peek_async (GrdRdpServer        *rdp_server,
                              GSocketConnection   *connection,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  on_finished_callback)
{
  RoutingTokenContext *routing_token_context;
  GTask *task;

  routing_token_context = g_new0 (RoutingTokenContext, 1);
  routing_token_context->rdp_server = rdp_server;
  routing_token_context->connection = g_object_ref (connection);
  routing_token_context->cancellable = g_cancellable_new ();
  routing_token_context->server_cancellable = g_object_ref (cancellable);

  task = g_task_new (NULL, NULL, on_finished_callback, NULL);
  g_task_set_task_data (task, routing_token_context, clear_routing_token_context);
  g_task_run_in_thread (task, peek_routing_token_in_thread);
  g_object_unref (task);

  routing_token_context->abort_peek_source_id =
    g_timeout_add (MAX_PEEK_TIME_MS,
                   abort_peek_routing_token,
                   routing_token_context);
}

char *
grd_routing_token_peek_finish (GAsyncResult       *result,
                               GrdRdpServer      **rdp_server,
                               GSocketConnection **connection,
                               GCancellable      **server_cancellable,
                               GError            **error)
{
  RoutingTokenContext *routing_token_context =
    g_task_get_task_data (G_TASK (result));

  *rdp_server = routing_token_context->rdp_server;
  *connection = routing_token_context->connection;
  *server_cancellable = routing_token_context->server_cancellable;

  return g_task_propagate_pointer (G_TASK (result), error);
}
