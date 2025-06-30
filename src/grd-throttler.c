/*
 * Copyright (C) 2025 Red Hat
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

#include "grd-throttler.h"

#include "grd-context.h"
#include "grd-settings.h"
#include "grd-utils.h"

#define DEFAULT_MAX_CONNECTIONS_PER_PEER 5
#define DEFAULT_MAX_PENDING_CONNECTIONS 5
#define DEFAULT_MAX_ATTEMPTS_PER_SECOND 10

struct _GrdThrottlerLimits
{
  int max_global_connections;
  int max_connections_per_peer;
  int max_pending_connections;
  int max_attempts_per_second;
};

#define PRUNE_TIME_CUTOFF_US (s2us (1))

typedef struct _GrdPeer
{
  GrdThrottler *throttler;
  char *name;
  int active_connections;
  GArray *connect_timestamps;
  GQueue *delayed_connections;
  int64_t last_accept_us;
} GrdPeer;

struct _GrdThrottler
{
  GObject parent;

  GrdThrottlerLimits *limits;

  int active_connections;

  GrdThrottlerAllowCallback allow_callback;
  gpointer user_data;

  GHashTable *peers;

  GSource *delayed_connections_source;
};

G_DEFINE_TYPE (GrdThrottler, grd_throttler, G_TYPE_OBJECT)

static GQuark quark_remote_address;

static void
maybe_queue_timeout (GrdThrottler *throttler);

static void
prune_old_timestamps (GArray  *timestamps,
                      int64_t  now_us)
{
  size_t i;

  if (!timestamps)
    return;

  /* Prune all timestamps older than 1 second, so we can determine how many
   * connections that happened the last second by looking at how many timestamps
   * we have.
   */
  for (i = 0; i < timestamps->len; i++)
    {
      int64_t ts_us = g_array_index (timestamps, int64_t, i);

      if (now_us - ts_us < PRUNE_TIME_CUTOFF_US)
        break;
    }
  g_array_remove_range (timestamps, 0, i);
}

static void
maybe_dispose_peer (GrdThrottler   *throttler,
                    GrdPeer        *peer,
                    GHashTableIter *iter)
{
  if (peer->active_connections > 0)
    return;

  if (peer->connect_timestamps && peer->connect_timestamps->len > 0)
    return;

  if (!g_queue_is_empty (peer->delayed_connections))
    return;

  if (iter)
    g_hash_table_iter_remove (iter);
  else
    g_hash_table_remove (throttler->peers, peer->name);
}

static void
grd_throttler_register_connection (GrdThrottler *throttler,
                                   GrdPeer      *peer)
{
  int64_t now_us;

  peer->active_connections++;
  throttler->active_connections++;

  if (!peer->connect_timestamps)
    peer->connect_timestamps = g_array_new (FALSE, FALSE, sizeof (int64_t));

  now_us = g_get_monotonic_time ();

  prune_old_timestamps (peer->connect_timestamps, now_us);
  g_array_append_val (peer->connect_timestamps, now_us);
}

static void
grd_throttler_unregister_connection (GrdThrottler      *throttler,
                                     GSocketConnection *connection,
                                     GrdPeer           *peer)
{
  g_assert (peer->active_connections > 0);
  g_assert (throttler->active_connections > 0);

  peer->active_connections--;
  throttler->active_connections--;

  maybe_dispose_peer (throttler, peer, NULL);
  maybe_queue_timeout (throttler);
}

static void
grd_throttler_deny_connection (GrdThrottler      *throttler,
                               const char        *peer_name,
                               GSocketConnection *connection)
{
  g_debug ("Denying connection from %s", peer_name);
  g_io_stream_close (G_IO_STREAM (connection), NULL, NULL);
}

static void
on_connection_closed_changed (GSocketConnection *connection,
                              GParamSpec        *pspec,
                              GrdThrottler      *throttler)
{
  const char *peer_name;
  GrdPeer *peer;

  g_assert (g_io_stream_is_closed (G_IO_STREAM (connection)));

  peer_name = g_object_get_qdata (G_OBJECT (connection), quark_remote_address);
  peer = g_hash_table_lookup (throttler->peers, peer_name);
  grd_throttler_unregister_connection (throttler, connection, peer);
}

static void
grd_throttler_allow_connection (GrdThrottler      *throttler,
                                GSocketConnection *connection,
                                GrdPeer           *peer)
{
  g_debug ("Accepting connection from %s", peer->name);

  throttler->allow_callback (throttler, connection, throttler->user_data);

  peer->last_accept_us = g_get_monotonic_time ();

  g_object_set_qdata_full (G_OBJECT (connection), quark_remote_address,
                           g_strdup (peer->name), g_free);
  grd_throttler_register_connection (throttler, peer);
  g_signal_connect (connection, "notify::closed",
                    G_CALLBACK (on_connection_closed_changed), throttler);
}

static gboolean
source_dispatch (GSource     *source,
                 GSourceFunc  callback,
                 gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs source_funcs =
{
  .dispatch = source_dispatch,
};

static void
prune_closed_connections (GQueue *queue)
{
  GList *l;

  l = queue->head;
  while (l)
    {
      GSocketConnection *connection = G_SOCKET_CONNECTION (l->data);
      GList *l_next = l->next;

      if (g_io_stream_is_closed (G_IO_STREAM (connection)))
        {
          g_queue_delete_link (queue, l);
          g_object_unref (connection);
        }

      l = l_next;
    }
}

static gboolean
is_connection_limit_reached (GrdThrottler *throttler,
                             GrdPeer      *peer)
{
  GrdThrottlerLimits *limits = throttler->limits;

  if (peer->active_connections >= limits->max_connections_per_peer)
    return TRUE;

  if (throttler->active_connections >= limits->max_global_connections)
    return TRUE;

  return FALSE;
}

static gboolean
is_new_connection_allowed (GrdThrottler *throttler,
                           GrdPeer      *peer)
{
  GrdThrottlerLimits *limits = throttler->limits;

  if (is_connection_limit_reached (throttler, peer))
    return FALSE;

  if (peer->connect_timestamps &&
      peer->connect_timestamps->len >= limits->max_attempts_per_second)
    return FALSE;

  return TRUE;
}

static gboolean
dispatch_delayed_connections (gpointer user_data)
{
  GrdThrottler *throttler = GRD_THROTTLER (user_data);
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, throttler->peers);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GrdPeer *peer = value;
      GQueue *queue = peer->delayed_connections;
      GSocketConnection *connection;

      prune_closed_connections (queue);
      connection = g_queue_peek_head (queue);

      if (!connection)
        {
          maybe_dispose_peer (throttler, peer, &iter);
          continue;
        }

      if (is_new_connection_allowed (throttler, peer))
        {
          g_queue_pop_head (queue);
          grd_throttler_allow_connection (throttler, connection, peer);
          g_object_unref (connection);
        }
    }

  maybe_queue_timeout (throttler);

  return G_SOURCE_CONTINUE;
}

static void
ensure_delayed_connections_source (GrdThrottler *throttler)
{
  if (throttler->delayed_connections_source)
    return;

  throttler->delayed_connections_source = g_source_new (&source_funcs,
                                                        sizeof (GSource));
  g_source_set_callback (throttler->delayed_connections_source,
                         dispatch_delayed_connections, throttler, NULL);
  g_source_attach (throttler->delayed_connections_source, NULL);
  g_source_unref (throttler->delayed_connections_source);
}

static void
maybe_queue_timeout (GrdThrottler *throttler)
{
  GrdThrottlerLimits *limits = throttler->limits;
  GHashTableIter iter;
  gpointer key, value;
  int64_t next_timeout_us = INT64_MAX;

  g_hash_table_iter_init (&iter, throttler->peers);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GrdPeer *peer = value;

      if (is_connection_limit_reached (throttler, peer))
        continue;

      if (g_queue_is_empty (peer->delayed_connections))
        continue;

      next_timeout_us = MIN (next_timeout_us,
                             peer->last_accept_us +
                             G_USEC_PER_SEC / limits->max_attempts_per_second);
    }


  if (next_timeout_us == INT64_MAX)
    next_timeout_us = -1;

  if (next_timeout_us >= 0)
    {
      ensure_delayed_connections_source (throttler);
      g_source_set_ready_time (throttler->delayed_connections_source,
                               next_timeout_us);
    }
}

static void
maybe_delay_connection (GrdThrottler      *throttler,
                        GSocketConnection *connection,
                        GrdPeer           *peer,
                        int64_t            now_us)
{
  GrdThrottlerLimits *limits = throttler->limits;
  GQueue *delayed_connections;

  delayed_connections = peer->delayed_connections;
  if (!delayed_connections)
    {
      delayed_connections = g_queue_new ();
      peer->delayed_connections = delayed_connections;
    }

  if (g_queue_get_length (delayed_connections) > limits->max_pending_connections)
    {
      grd_throttler_deny_connection (throttler, peer->name, connection);
      return;
    }

  g_debug ("Delaying connection from %s", peer->name);

  g_queue_push_tail (delayed_connections, g_object_ref (connection));
  maybe_queue_timeout (throttler);
}

static GrdPeer *
ensure_peer (GrdThrottler *throttler,
             const char   *peer_name)
{
  GrdPeer *peer;

  peer = g_hash_table_lookup (throttler->peers, peer_name);
  if (peer)
    return peer;

  peer = g_new0 (GrdPeer, 1);
  peer->throttler = throttler;
  peer->name = g_strdup (peer_name);
  peer->delayed_connections = g_queue_new ();

  g_hash_table_insert (throttler->peers,
                       g_strdup (peer_name), peer);

  return peer;
}

void
grd_throttler_handle_connection (GrdThrottler      *throttler,
                                 GSocketConnection *connection)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketAddress) remote_address = NULL;
  GInetAddress *inet_address;
  g_autofree char *peer_name = NULL;
  GrdPeer *peer;
  int64_t now_us;

  remote_address = g_socket_connection_get_remote_address (connection, &error);
  if (!remote_address)
    {
      g_warning ("Failed to get remote address: %s", error->message);
      grd_throttler_deny_connection (throttler, "unknown peer", connection);
      return;
    }

  inet_address =
    g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (remote_address));
  peer_name = g_inet_address_to_string (inet_address);

  g_debug ("New incoming connection from %s", peer_name);

  peer = ensure_peer (throttler, peer_name);

  prune_closed_connections (peer->delayed_connections);

  now_us = g_get_monotonic_time ();
  prune_old_timestamps (peer->connect_timestamps, now_us);
  if (is_new_connection_allowed (throttler, peer) &&
      g_queue_get_length (peer->delayed_connections) == 0)
    {
      grd_throttler_allow_connection (throttler, connection, peer);
      return;
    }

  maybe_delay_connection (throttler, connection, peer, now_us);
}

void
grd_throttler_limits_set_max_global_connections (GrdThrottlerLimits *limits,
                                                 int                 limit)
{
  limits->max_global_connections = limit;
}

GrdThrottlerLimits *
grd_throttler_limits_new (GrdContext *context)
{
  GrdSettings *settings = grd_context_get_settings (context);
  GrdThrottlerLimits *limits;

  limits = g_new0 (GrdThrottlerLimits, 1);
  limits->max_global_connections =
    grd_settings_get_max_parallel_connections (settings);
  limits->max_connections_per_peer = DEFAULT_MAX_CONNECTIONS_PER_PEER;
  limits->max_pending_connections = DEFAULT_MAX_PENDING_CONNECTIONS;
  limits->max_attempts_per_second = DEFAULT_MAX_ATTEMPTS_PER_SECOND;

  return limits;
}

GrdThrottler *
grd_throttler_new (GrdThrottlerLimits        *limits,
                   GrdThrottlerAllowCallback  allow_callback,
                   gpointer                   user_data)
{
  GrdThrottler *throttler;

  g_assert (limits);

  throttler = g_object_new (GRD_TYPE_THROTTLER, NULL);
  throttler->allow_callback = allow_callback;
  throttler->user_data = user_data;
  throttler->limits = limits;

  return throttler;
}

static void
grd_peer_free (GrdPeer *peer)
{
  if (peer->delayed_connections)
    g_queue_free_full (peer->delayed_connections, g_object_unref);
  g_clear_pointer (&peer->connect_timestamps, g_array_unref);
  g_clear_pointer (&peer->name, g_free);
  g_free (peer);
}

static void
grd_throttler_finalize (GObject *object)
{
  GrdThrottler *throttler = GRD_THROTTLER(object);

  g_clear_pointer (&throttler->delayed_connections_source, g_source_destroy);
  g_clear_pointer (&throttler->peers, g_hash_table_unref);
  g_clear_pointer (&throttler->limits, g_free);

  G_OBJECT_CLASS (grd_throttler_parent_class)->finalize (object);
}

static void
grd_throttler_init (GrdThrottler *throttler)
{
  throttler->peers =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free, (GDestroyNotify) grd_peer_free);
}

static void
grd_throttler_class_init (GrdThrottlerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_throttler_finalize;

  quark_remote_address =
    g_quark_from_static_string ("grd-remote-address-string");
}
