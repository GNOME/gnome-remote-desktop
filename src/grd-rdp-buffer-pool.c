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

#include "grd-rdp-buffer-pool.h"

#include "grd-rdp-buffer.h"

typedef struct _BufferInfo
{
  gboolean buffer_taken;
} BufferInfo;

struct _GrdRdpBufferPool
{
  GObject parent;

  gboolean has_buffer_size;
  uint32_t buffer_width;
  uint32_t buffer_height;
  uint32_t buffer_stride;

  GSource *resize_pool_source;
  uint32_t minimum_pool_size;

  GMutex pool_mutex;
  GHashTable *buffer_table;
  uint32_t buffers_taken;
};

G_DEFINE_TYPE (GrdRdpBufferPool, grd_rdp_buffer_pool, G_TYPE_OBJECT)

static void
add_buffer_to_pool (GrdRdpBufferPool *buffer_pool)
{
  GrdRdpBuffer *buffer;
  BufferInfo *buffer_info;

  buffer = grd_rdp_buffer_new ();
  buffer_info = g_new0 (BufferInfo, 1);

  if (buffer_pool->has_buffer_size)
    {
      grd_rdp_buffer_resize (buffer,
                             buffer_pool->buffer_width,
                             buffer_pool->buffer_height,
                             buffer_pool->buffer_stride);
    }

  g_hash_table_insert (buffer_pool->buffer_table, buffer, buffer_info);
}

void
grd_rdp_buffer_pool_resize_buffers (GrdRdpBufferPool *buffer_pool,
                                    uint32_t          buffer_width,
                                    uint32_t          buffer_height,
                                    uint32_t          buffer_stride)
{
  GHashTableIter iter;
  GrdRdpBuffer *buffer;
  BufferInfo *buffer_info;

  g_mutex_lock (&buffer_pool->pool_mutex);
  g_assert (buffer_pool->buffers_taken == 0);

  buffer_pool->buffer_width = buffer_width;
  buffer_pool->buffer_height = buffer_height;
  buffer_pool->buffer_stride = buffer_stride;
  buffer_pool->has_buffer_size = TRUE;

  g_hash_table_iter_init (&iter, buffer_pool->buffer_table);
  while (g_hash_table_iter_next (&iter, (gpointer *) &buffer,
                                        (gpointer *) &buffer_info))
    {
      g_assert (!buffer_info->buffer_taken);
      grd_rdp_buffer_resize (buffer, buffer_width, buffer_height, buffer_stride);
    }
  g_mutex_unlock (&buffer_pool->pool_mutex);
}

GrdRdpBuffer *
grd_rdp_buffer_pool_acquire (GrdRdpBufferPool *buffer_pool)
{
  GHashTableIter iter;
  GrdRdpBuffer *buffer;
  BufferInfo *buffer_info;

  g_mutex_lock (&buffer_pool->pool_mutex);
  if (g_hash_table_size (buffer_pool->buffer_table) <= buffer_pool->buffers_taken)
    add_buffer_to_pool (buffer_pool);

  g_hash_table_iter_init (&iter, buffer_pool->buffer_table);
  while (g_hash_table_iter_next (&iter, (gpointer *) &buffer,
                                        (gpointer *) &buffer_info))
    {
      if (!buffer_info->buffer_taken)
        break;
    }

  buffer_info->buffer_taken = TRUE;
  ++buffer_pool->buffers_taken;
  g_mutex_unlock (&buffer_pool->pool_mutex);

  return buffer;
}

static gboolean
should_resize_buffer_pool (GrdRdpBufferPool *buffer_pool)
{
  uint32_t buffers_taken = buffer_pool->buffers_taken;
  uint32_t minimum_size = buffer_pool->minimum_pool_size;
  uint32_t current_pool_size;

  current_pool_size = g_hash_table_size (buffer_pool->buffer_table);

  if (current_pool_size > minimum_size &&
      current_pool_size > buffers_taken)
    return TRUE;

  return FALSE;
}

void
grd_rdp_buffer_pool_release_buffer (GrdRdpBufferPool *buffer_pool,
                                    GrdRdpBuffer     *buffer)
{
  BufferInfo *buffer_info;
  gboolean queue_pool_resize;

  g_mutex_lock (&buffer_pool->pool_mutex);
  if (!g_hash_table_lookup_extended (buffer_pool->buffer_table, buffer,
                                     NULL, (gpointer *) &buffer_info))
    g_assert_not_reached ();

  g_assert (buffer_info->buffer_taken);

  buffer_info->buffer_taken = FALSE;
  --buffer_pool->buffers_taken;

  queue_pool_resize = should_resize_buffer_pool (buffer_pool);
  g_mutex_unlock (&buffer_pool->pool_mutex);

  if (queue_pool_resize)
    g_source_set_ready_time (buffer_pool->resize_pool_source, 0);
}

static gboolean
resize_buffer_pool (gpointer user_data)
{
  GrdRdpBufferPool *buffer_pool = user_data;
  GHashTableIter iter;
  BufferInfo *buffer_info;

  g_mutex_lock (&buffer_pool->pool_mutex);
  g_hash_table_iter_init (&iter, buffer_pool->buffer_table);
  while (should_resize_buffer_pool (buffer_pool) &&
         g_hash_table_iter_next (&iter, NULL, (gpointer *) &buffer_info))
    {
      if (!buffer_info->buffer_taken)
        g_hash_table_iter_remove (&iter);
    }
  g_mutex_unlock (&buffer_pool->pool_mutex);

  return G_SOURCE_CONTINUE;
}

static gboolean
resize_pool_source_dispatch (GSource     *source,
                             GSourceFunc  callback,
                             gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs resize_pool_source_funcs =
{
  .dispatch = resize_pool_source_dispatch,
};

static void
fill_buffer_pool (GrdRdpBufferPool *buffer_pool)
{
  uint32_t minimum_size = buffer_pool->minimum_pool_size;

  while (g_hash_table_size (buffer_pool->buffer_table) < minimum_size)
    add_buffer_to_pool (buffer_pool);
}

GrdRdpBufferPool *
grd_rdp_buffer_pool_new (uint32_t minimum_size)
{
  GrdRdpBufferPool *buffer_pool;

  buffer_pool = g_object_new (GRD_TYPE_RDP_BUFFER_POOL, NULL);
  buffer_pool->minimum_pool_size = minimum_size;

  buffer_pool->resize_pool_source = g_source_new (&resize_pool_source_funcs,
                                                 sizeof (GSource));
  g_source_set_callback (buffer_pool->resize_pool_source,
                         resize_buffer_pool, buffer_pool, NULL);
  g_source_set_ready_time (buffer_pool->resize_pool_source, -1);
  g_source_attach (buffer_pool->resize_pool_source, NULL);

  fill_buffer_pool (buffer_pool);

  return buffer_pool;
}

static void
grd_rdp_buffer_pool_dispose (GObject *object)
{
  GrdRdpBufferPool *buffer_pool = GRD_RDP_BUFFER_POOL (object);

  if (buffer_pool->resize_pool_source)
    {
      g_source_destroy (buffer_pool->resize_pool_source);
      g_clear_pointer (&buffer_pool->resize_pool_source, g_source_unref);
    }

  g_assert (buffer_pool->buffers_taken == 0);

  G_OBJECT_CLASS (grd_rdp_buffer_pool_parent_class)->dispose (object);
}

static void
grd_rdp_buffer_pool_finalize (GObject *object)
{
  GrdRdpBufferPool *buffer_pool = GRD_RDP_BUFFER_POOL (object);

  g_mutex_clear (&buffer_pool->pool_mutex);

  g_clear_pointer (&buffer_pool->buffer_table, g_hash_table_unref);

  G_OBJECT_CLASS (grd_rdp_buffer_pool_parent_class)->finalize (object);
}

static void
grd_rdp_buffer_pool_init (GrdRdpBufferPool *buffer_pool)
{
  buffer_pool->buffer_table = g_hash_table_new_full (NULL, NULL,
                                                     (GDestroyNotify) grd_rdp_buffer_free,
                                                     g_free);

  g_mutex_init (&buffer_pool->pool_mutex);
}

static void
grd_rdp_buffer_pool_class_init (GrdRdpBufferPoolClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_buffer_pool_dispose;
  object_class->finalize = grd_rdp_buffer_pool_finalize;
}
