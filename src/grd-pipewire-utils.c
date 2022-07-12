/*
 * Copyright (C) 2015 Red Hat Inc.
 * Copyright (C) 2020 Pascal Nowack
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

#include "grd-pipewire-utils.h"

#include <drm_fourcc.h>
#include <spa/param/video/raw.h>
#include <spa/utils/result.h>

static void pipewire_source_unref (GrdPipeWireSource *pipewire_source);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdPipeWireSource, pipewire_source_unref)

static gboolean
pipewire_loop_source_prepare (GSource *base,
                              int     *timeout)
{
  *timeout = -1;

  return FALSE;
}

static gboolean
pipewire_loop_source_dispatch (GSource     *source,
                               GSourceFunc  callback,
                               gpointer     user_data)
{
  GrdPipeWireSource *pipewire_source = (GrdPipeWireSource *) source;
  int result;

  result = pw_loop_iterate (pipewire_source->pipewire_loop, 0);
  if (result < 0)
    {
      g_warning ("%s pw_loop_iterate() failed: %s", pipewire_source->message_tag,
                 spa_strerror (result));
    }

  return TRUE;
}

static void
pipewire_loop_source_finalize (GSource *source)
{
  GrdPipeWireSource *pipewire_source = (GrdPipeWireSource *) source;

  if (pipewire_source->pipewire_loop)
    {
      pw_loop_leave (pipewire_source->pipewire_loop);
      pw_loop_destroy (pipewire_source->pipewire_loop);
    }
  g_clear_pointer (&pipewire_source->message_tag, g_free);
}

static GSourceFuncs pipewire_source_funcs =
{
  pipewire_loop_source_prepare,
  NULL,
  pipewire_loop_source_dispatch,
  pipewire_loop_source_finalize,
};

GrdPipeWireSource *
grd_attached_pipewire_source_new (const char  *message_tag,
                                  GError     **error)
{
  g_autoptr (GrdPipeWireSource) pipewire_source = NULL;

  g_assert (strlen (message_tag) > 0);

  pipewire_source =
    (GrdPipeWireSource *) g_source_new (&pipewire_source_funcs,
                                        sizeof (GrdPipeWireSource));
  pipewire_source->message_tag = g_strdup_printf ("[%s]", message_tag);

  pipewire_source->pipewire_loop = pw_loop_new (NULL);
  if (!pipewire_source->pipewire_loop)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire loop");
      return NULL;
    }

  g_source_add_unix_fd (&pipewire_source->base,
                        pw_loop_get_fd (pipewire_source->pipewire_loop),
                        G_IO_IN | G_IO_ERR);

  pw_loop_enter (pipewire_source->pipewire_loop);
  g_source_attach (&pipewire_source->base, NULL);

  return g_steal_pointer (&pipewire_source);
}

static void
pipewire_source_unref (GrdPipeWireSource *pipewire_source)
{
  g_source_unref (&pipewire_source->base);
}

gboolean
grd_pipewire_buffer_has_pointer_bitmap (struct pw_buffer *buffer)
{
  struct spa_meta_cursor *spa_meta_cursor;

  spa_meta_cursor = spa_buffer_find_meta_data (buffer->buffer, SPA_META_Cursor,
                                               sizeof *spa_meta_cursor);
  if (spa_meta_cursor && spa_meta_cursor_is_valid (spa_meta_cursor))
    {
      struct spa_meta_bitmap *spa_meta_bitmap = NULL;

      if (spa_meta_cursor->bitmap_offset)
        {
          spa_meta_bitmap = SPA_MEMBER (spa_meta_cursor,
                                        spa_meta_cursor->bitmap_offset,
                                        struct spa_meta_bitmap);
        }
      if (spa_meta_bitmap)
        return TRUE;
    }

  return FALSE;
}

gboolean
grd_pipewire_buffer_has_frame_data (struct pw_buffer *buffer)
{
  return buffer->buffer->datas[0].chunk->size > 0;
}

gboolean
grd_spa_pixel_format_to_grd_pixel_format (uint32_t        spa_format,
                                          GrdPixelFormat *out_format)
{
  if (spa_format == SPA_VIDEO_FORMAT_RGBA)
    *out_format = GRD_PIXEL_FORMAT_RGBA8888;
  else
    return FALSE;

  return TRUE;
}

static struct
{
  enum spa_video_format spa_format;
  uint32_t drm_format;
  int bpp;
} format_table[] = {
  { SPA_VIDEO_FORMAT_ARGB, DRM_FORMAT_BGRA8888, 4 },
  { SPA_VIDEO_FORMAT_BGRA, DRM_FORMAT_ARGB8888, 4 },
  { SPA_VIDEO_FORMAT_xRGB, DRM_FORMAT_BGRX8888, 4 },
  { SPA_VIDEO_FORMAT_BGRx, DRM_FORMAT_XRGB8888, 4 },
};

void
grd_get_spa_format_details (enum spa_video_format  spa_format,
                            uint32_t              *drm_format,
                            int                   *bpp)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (format_table); i++)
    {
      if (format_table[i].spa_format == spa_format)
        {
          if (drm_format)
            *drm_format = format_table[i].drm_format;
          if (bpp)
            *bpp = format_table[i].bpp;
          return;
        }
    }

  g_assert_not_reached ();
}
