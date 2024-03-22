/*
 * Copyright (C) 2023 Pascal Nowack
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

#include "grd-rdp-cursor-renderer.h"

#include "grd-damage-utils.h"
#include "grd-rdp-renderer.h"

typedef struct
{
  GrdRdpCursorUpdate *cursor_update;

  int64_t last_used;
} GrdRdpCursor;

struct _GrdRdpCursorRenderer
{
  GObject parent;

  gboolean session_ready;

  rdpContext *rdp_context;

  GSource *render_source;
  /* Current cursor pointer does not own cursor data */
  GrdRdpCursor *current_cursor;

  GrdRdpCursor current_system_cursor;
  /* Current cached cursor pointer does not own cursor data */
  GrdRdpCursor *current_cached_cursor;

  uint32_t pointer_cache_size;
  GrdRdpCursor *pointer_cache;

  GMutex update_mutex;
  GrdRdpCursorUpdate *pending_cursor_update;
};

G_DEFINE_TYPE (GrdRdpCursorRenderer, grd_rdp_cursor_renderer, G_TYPE_OBJECT)

static void
grd_rdp_cursor_update_free (GrdRdpCursorUpdate *cursor_update)
{
  g_free (cursor_update->bitmap);
  g_free (cursor_update);
}

void
grd_rdp_cursor_renderer_notify_session_ready (GrdRdpCursorRenderer *cursor_renderer)
{
  cursor_renderer->session_ready = TRUE;

  g_source_set_ready_time (cursor_renderer->render_source, 0);
}

void
grd_rdp_cursor_renderer_submit_cursor_update (GrdRdpCursorRenderer *cursor_renderer,
                                              GrdRdpCursorUpdate   *cursor_update)
{
  g_mutex_lock (&cursor_renderer->update_mutex);
  g_clear_pointer (&cursor_renderer->pending_cursor_update,
                   grd_rdp_cursor_update_free);

  cursor_renderer->pending_cursor_update = cursor_update;
  g_mutex_unlock (&cursor_renderer->update_mutex);

  g_source_set_ready_time (cursor_renderer->render_source, 0);
}

static void
update_to_system_cursor (GrdRdpCursorRenderer *cursor_renderer,
                         GrdRdpCursorUpdate   *cursor_update)
{
  GrdRdpCursor *current_system_cursor = &cursor_renderer->current_system_cursor;

  g_clear_pointer (&current_system_cursor->cursor_update,
                   grd_rdp_cursor_update_free);
  current_system_cursor->cursor_update = cursor_update;

  cursor_renderer->current_cached_cursor = NULL;
  cursor_renderer->current_cursor = current_system_cursor;
}

static void
reset_cursor_shape_fastpath (GrdRdpCursorRenderer *cursor_renderer)
{
  rdpContext *rdp_context = cursor_renderer->rdp_context;
  rdpUpdate *rdp_update = rdp_context->update;
  POINTER_SYSTEM_UPDATE pointer_system = {};

  pointer_system.type = SYSPTR_DEFAULT;

  rdp_update->pointer->PointerSystem (rdp_context, &pointer_system);
}

static gboolean
maybe_reset_cursor_shape (GrdRdpCursorRenderer *cursor_renderer,
                          GrdRdpCursorUpdate   *cursor_update)
{
  GrdRdpCursor *current_cursor = cursor_renderer->current_cursor;
  GrdRdpCursorUpdate *current_cursor_update = NULL;

  if (current_cursor)
    current_cursor_update = current_cursor->cursor_update;

  if (current_cursor_update &&
      current_cursor_update->update_type == GRD_RDP_CURSOR_UPDATE_TYPE_DEFAULT)
    return FALSE;

  update_to_system_cursor (cursor_renderer, cursor_update);

  reset_cursor_shape_fastpath (cursor_renderer);

  return TRUE;
}

static void
hide_cursor_fastpath (GrdRdpCursorRenderer *cursor_renderer)
{
  rdpContext *rdp_context = cursor_renderer->rdp_context;
  rdpUpdate *rdp_update = rdp_context->update;
  POINTER_SYSTEM_UPDATE pointer_system = {};

  pointer_system.type = SYSPTR_NULL;

  rdp_update->pointer->PointerSystem (rdp_context, &pointer_system);
}

static gboolean
maybe_hide_cursor (GrdRdpCursorRenderer *cursor_renderer,
                   GrdRdpCursorUpdate   *cursor_update)
{
  GrdRdpCursor *current_cursor = cursor_renderer->current_cursor;
  GrdRdpCursorUpdate *current_cursor_update = NULL;

  if (current_cursor)
    current_cursor_update = current_cursor->cursor_update;

  if (current_cursor_update &&
      current_cursor_update->update_type == GRD_RDP_CURSOR_UPDATE_TYPE_HIDDEN)
    return FALSE;

  update_to_system_cursor (cursor_renderer, cursor_update);

  hide_cursor_fastpath (cursor_renderer);

  return TRUE;
}

static gboolean
is_cursor_hidden (GrdRdpCursorUpdate *cursor_update)
{
  uint32_t cursor_size;
  uint8_t *src_data;
  uint32_t i;

  cursor_size = ((uint32_t) cursor_update->width) * cursor_update->height;
  for (i = 0, src_data = cursor_update->bitmap; i < cursor_size; ++i, ++src_data)
    {
      src_data += 3;
      if (*src_data)
        return FALSE;
    }

  return TRUE;
}

static gboolean
are_cursor_bitmaps_equal (GrdRdpCursorUpdate *first,
                          GrdRdpCursorUpdate *second)
{
  cairo_rectangle_int_t cairo_rect = {};

  g_assert (first);
  g_assert (second);
  g_assert (first->update_type == GRD_RDP_CURSOR_UPDATE_TYPE_NORMAL);
  g_assert (second->update_type == GRD_RDP_CURSOR_UPDATE_TYPE_NORMAL);

  if (first->hotspot_x != second->hotspot_x ||
      first->hotspot_y != second->hotspot_y ||
      first->width != second->width ||
      first->height != second->height)
    return FALSE;

  cairo_rect.x = cairo_rect.y = 0;
  cairo_rect.width = first->width;
  cairo_rect.height = first->height;

  return !grd_is_tile_dirty (&cairo_rect, first->bitmap, second->bitmap,
                             first->width * 4u, 4u);
}

static void
use_cached_cursor_fastpath (GrdRdpCursorRenderer *cursor_renderer,
                            uint16_t              cache_index)
{
  rdpContext *rdp_context = cursor_renderer->rdp_context;
  rdpUpdate *rdp_update = rdp_context->update;
  POINTER_CACHED_UPDATE pointer_cached = {};

  pointer_cached.cacheIndex = cache_index;

  rdp_update->pointer->PointerCached (rdp_context, &pointer_cached);
}

static void
use_cached_cursor (GrdRdpCursorRenderer *cursor_renderer,
                   uint16_t              cache_index)
{
  use_cached_cursor_fastpath (cursor_renderer, cache_index);
}

GrdRdpCursor *
get_cursor_to_replace (GrdRdpCursorRenderer *cursor_renderer,
                       uint32_t             *cache_index)
{
  GrdRdpCursor *lru_cursor = NULL;
  uint32_t i;

  g_assert (cache_index);

  for (i = 0; i < cursor_renderer->pointer_cache_size; ++i)
    {
      GrdRdpCursor *cached_cursor = &cursor_renderer->pointer_cache[i];

      /* Least recently used cursor */
      if (!lru_cursor || lru_cursor->last_used > cached_cursor->last_used)
        {
          lru_cursor = cached_cursor;
          *cache_index = i;
        }
    }
  g_assert (lru_cursor);

  return lru_cursor;
}

static void
submit_cursor_fastpath (GrdRdpCursorRenderer *cursor_renderer,
                        GrdRdpCursorUpdate   *cursor_update,
                        uint8_t              *xor_mask,
                        uint32_t              xor_mask_length,
                        uint32_t              cache_index)
{
  rdpContext *rdp_context = cursor_renderer->rdp_context;
  rdpUpdate *rdp_update = rdp_context->update;
  POINTER_NEW_UPDATE pointer_new = {};
  POINTER_LARGE_UPDATE pointer_large = {};
  POINTER_COLOR_UPDATE *pointer_color;

  if (cursor_update->width <= 96 && cursor_update->height <= 96)
    {
      pointer_new.xorBpp = 32;
      pointer_color = &pointer_new.colorPtrAttr;
      pointer_color->cacheIndex = cache_index;
      pointer_color->hotSpotX = cursor_update->hotspot_x;
      pointer_color->hotSpotY = cursor_update->hotspot_y;
      pointer_color->width = cursor_update->width;
      pointer_color->height = cursor_update->height;
      pointer_color->lengthAndMask = 0;
      pointer_color->lengthXorMask = xor_mask_length;
      pointer_color->xorMaskData = xor_mask;
      pointer_color->andMaskData = NULL;

      rdp_update->pointer->PointerNew (rdp_context, &pointer_new);
    }
  else
    {
      pointer_large.xorBpp = 32;
      pointer_large.cacheIndex = cache_index;
      pointer_large.hotSpotX = cursor_update->hotspot_x;
      pointer_large.hotSpotY = cursor_update->hotspot_y;
      pointer_large.width = cursor_update->width;
      pointer_large.height = cursor_update->height;
      pointer_large.lengthAndMask = 0;
      pointer_large.lengthXorMask = xor_mask_length;
      pointer_large.xorMaskData = xor_mask;
      pointer_large.andMaskData = NULL;

      rdp_update->pointer->PointerLarge (rdp_context, &pointer_large);
    }
}

static void
submit_cursor (GrdRdpCursorRenderer *cursor_renderer,
               GrdRdpCursorUpdate   *cursor_update,
               uint32_t              cache_index)
{
  uint32_t width = cursor_update->width;
  uint32_t height = cursor_update->height;
  g_autofree uint8_t *xor_mask = NULL;
  uint32_t xor_mask_length;
  uint32_t stride;
  uint8_t *src_data, *dst_data;
  uint8_t r, g, b, a;
  uint32_t x, y;

  stride = width * 4u;

  xor_mask_length = height * stride * sizeof (uint8_t);
  xor_mask = g_malloc0 (xor_mask_length);

  for (y = 0; y < height; ++y)
    {
      src_data = &cursor_update->bitmap[stride * (height - 1 - y)];
      dst_data = &xor_mask[stride * y];

      for (x = 0; x < width; ++x)
        {
          r = *src_data++;
          g = *src_data++;
          b = *src_data++;
          a = *src_data++;

          *dst_data++ = b;
          *dst_data++ = g;
          *dst_data++ = r;
          *dst_data++ = a;
        }
    }

  submit_cursor_fastpath (cursor_renderer, cursor_update,
                          xor_mask, xor_mask_length, cache_index);
}

static gboolean
maybe_update_cursor (GrdRdpCursorRenderer *cursor_renderer,
                     GrdRdpCursorUpdate   *cursor_update)
{
  GrdRdpCursor *current_cursor = cursor_renderer->current_cursor;
  GrdRdpCursorUpdate *current_cursor_update = NULL;
  GrdRdpCursor *cursor_slot = NULL;
  uint32_t cursor_slot_index = 0;
  uint32_t i;

  g_assert (cursor_update->update_type == GRD_RDP_CURSOR_UPDATE_TYPE_NORMAL);
  g_assert (cursor_update->bitmap);

  if (current_cursor)
    current_cursor_update = current_cursor->cursor_update;

  if (is_cursor_hidden (cursor_update))
    {
      cursor_update->update_type = GRD_RDP_CURSOR_UPDATE_TYPE_HIDDEN;
      g_clear_pointer (&cursor_update->bitmap, g_free);

      return maybe_hide_cursor (cursor_renderer, cursor_update);
    }

  /* RDP only handles pointer bitmaps up to 384x384 pixels */
  if (cursor_update->width > 384 || cursor_update->height > 384)
    {
      cursor_update->update_type = GRD_RDP_CURSOR_UPDATE_TYPE_DEFAULT;
      g_clear_pointer (&cursor_update->bitmap, g_free);

      return maybe_reset_cursor_shape (cursor_renderer, cursor_update);
    }

  if (current_cursor_update &&
      current_cursor_update->update_type == GRD_RDP_CURSOR_UPDATE_TYPE_NORMAL &&
      are_cursor_bitmaps_equal (cursor_update, current_cursor_update))
    {
      current_cursor->last_used = g_get_monotonic_time ();
      return FALSE;
    }

  for (i = 0; i < cursor_renderer->pointer_cache_size; ++i)
    {
      GrdRdpCursor *cached_cursor = &cursor_renderer->pointer_cache[i];
      GrdRdpCursorUpdate *cached_cursor_update = cached_cursor->cursor_update;
      GrdRdpCursorUpdateType update_type;

      if (!cached_cursor_update)
        {
          g_assert (!cursor_slot);
          cursor_slot = cached_cursor;
          cursor_slot_index = i;
          break;
        }

      update_type = cached_cursor_update->update_type;
      g_assert (update_type == GRD_RDP_CURSOR_UPDATE_TYPE_NORMAL);

      g_assert (cached_cursor);
      if (current_cursor == cached_cursor)
        continue;

      if (!are_cursor_bitmaps_equal (cursor_update, cached_cursor_update))
        continue;

      cursor_renderer->current_cached_cursor = cached_cursor;
      cursor_renderer->current_cursor = cursor_renderer->current_cached_cursor;

      cached_cursor->last_used = g_get_monotonic_time ();
      use_cached_cursor (cursor_renderer, i);

      return FALSE;
    }

  if (!cursor_slot)
    cursor_slot = get_cursor_to_replace (cursor_renderer, &cursor_slot_index);

  g_clear_pointer (&cursor_slot->cursor_update, grd_rdp_cursor_update_free);
  cursor_slot->cursor_update = cursor_update;

  cursor_renderer->current_cached_cursor = cursor_slot;
  cursor_renderer->current_cursor = cursor_renderer->current_cached_cursor;

  cursor_slot->last_used = g_get_monotonic_time ();
  submit_cursor (cursor_renderer, cursor_update, cursor_slot_index);
  use_cached_cursor (cursor_renderer, cursor_slot_index);

  return TRUE;
}

static gboolean
maybe_render_cursor (gpointer user_data)
{
  GrdRdpCursorRenderer *cursor_renderer = user_data;
  GrdRdpCursorUpdate *cursor_update;
  gboolean update_taken = FALSE;

  if (!cursor_renderer->session_ready)
    return G_SOURCE_CONTINUE;

  g_mutex_lock (&cursor_renderer->update_mutex);
  cursor_update = g_steal_pointer (&cursor_renderer->pending_cursor_update);
  g_mutex_unlock (&cursor_renderer->update_mutex);

  if (!cursor_update)
    return G_SOURCE_CONTINUE;

  switch (cursor_update->update_type)
    {
    case GRD_RDP_CURSOR_UPDATE_TYPE_DEFAULT:
      update_taken = maybe_reset_cursor_shape (cursor_renderer, cursor_update);
      break;
    case GRD_RDP_CURSOR_UPDATE_TYPE_HIDDEN:
      update_taken = maybe_hide_cursor (cursor_renderer, cursor_update);
      break;
    case GRD_RDP_CURSOR_UPDATE_TYPE_NORMAL:
      update_taken = maybe_update_cursor (cursor_renderer, cursor_update);
      break;
    }

  if (!update_taken)
    grd_rdp_cursor_update_free (cursor_update);

  return G_SOURCE_CONTINUE;
}

static gboolean
render_source_dispatch (GSource     *source,
                        GSourceFunc  callback,
                        gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs render_source_funcs =
{
  .dispatch = render_source_dispatch,
};

GrdRdpCursorRenderer *
grd_rdp_cursor_renderer_new (GrdRdpRenderer *renderer,
                             rdpContext     *rdp_context)
{
  GMainContext *render_context = grd_rdp_renderer_get_graphics_context (renderer);
  rdpSettings *rdp_settings = rdp_context->settings;
  GrdRdpCursorRenderer *cursor_renderer;
  GSource *render_source;

  cursor_renderer = g_object_new (GRD_TYPE_RDP_CURSOR_RENDERER, NULL);
  cursor_renderer->rdp_context = rdp_context;

  cursor_renderer->pointer_cache_size =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_PointerCacheSize);
  g_assert (cursor_renderer->pointer_cache_size > 0);
  g_assert (cursor_renderer->pointer_cache_size <= UINT16_MAX);

  g_debug ("[RDP] Cursor renderer: Cache has %u slots",
           cursor_renderer->pointer_cache_size);

  cursor_renderer->pointer_cache = g_new0 (GrdRdpCursor,
                                           cursor_renderer->pointer_cache_size);

  render_source = g_source_new (&render_source_funcs, sizeof (GSource));
  g_source_set_callback (render_source, maybe_render_cursor,
                         cursor_renderer, NULL);
  g_source_set_ready_time (render_source, -1);
  g_source_attach (render_source, render_context);
  cursor_renderer->render_source = render_source;

  return cursor_renderer;
}

static void
grd_rdp_cursor_renderer_dispose (GObject *object)
{
  GrdRdpCursorRenderer *cursor_renderer = GRD_RDP_CURSOR_RENDERER (object);

  if (cursor_renderer->render_source)
    {
      g_source_destroy (cursor_renderer->render_source);
      g_clear_pointer (&cursor_renderer->render_source, g_source_unref);
    }

  if (cursor_renderer->pointer_cache)
    {
      uint32_t i;

      for (i = 0; i < cursor_renderer->pointer_cache_size; ++i)
        {
          g_clear_pointer (&cursor_renderer->pointer_cache[i].cursor_update,
                           grd_rdp_cursor_update_free);
        }
      g_clear_pointer (&cursor_renderer->pointer_cache, g_free);
    }

  g_clear_pointer (&cursor_renderer->current_system_cursor.cursor_update,
                   grd_rdp_cursor_update_free);
  g_clear_pointer (&cursor_renderer->pending_cursor_update,
                   grd_rdp_cursor_update_free);

  G_OBJECT_CLASS (grd_rdp_cursor_renderer_parent_class)->dispose (object);
}

static void
grd_rdp_cursor_renderer_finalize (GObject *object)
{
  GrdRdpCursorRenderer *cursor_renderer = GRD_RDP_CURSOR_RENDERER (object);

  g_mutex_clear (&cursor_renderer->update_mutex);

  G_OBJECT_CLASS (grd_rdp_cursor_renderer_parent_class)->finalize (object);
}

static void
grd_rdp_cursor_renderer_init (GrdRdpCursorRenderer *cursor_renderer)
{
  g_mutex_init (&cursor_renderer->update_mutex);
}

static void
grd_rdp_cursor_renderer_class_init (GrdRdpCursorRendererClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_cursor_renderer_dispose;
  object_class->finalize = grd_rdp_cursor_renderer_finalize;
}
