/*
 * Copyright (C) 2018 Red Hat Inc.
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

#include "config.h"

#include "grd-vnc-cursor.h"

#include <glib.h>
#include <stdint.h>

static void
get_pixel_components (uint32_t        pixel,
                      GrdPixelFormat  format,
                      uint8_t        *a,
                      uint8_t        *r,
                      uint8_t        *g,
                      uint8_t        *b)
{
  g_assert (format == GRD_PIXEL_FORMAT_RGBA8888);

  *a = (pixel & 0xff000000) >> 24;
  *b = (pixel & 0xff0000) >> 16;
  *g = (pixel & 0xff00) >> 8;
  *r = pixel & 0xff;
}

static gboolean
is_practically_black (uint8_t r,
                      uint8_t g,
                      uint8_t b)
{
  if (r <= 0x62 &&
      g <= 0x62 &&
      b <= 0x62)
    return TRUE;
  else
    return FALSE;
}

static gboolean
is_practically_opaque (uint8_t a)
{
  return a > 0xe0;
}

rfbCursorPtr
grd_vnc_create_cursor (int             width,
                       int             height,
                       int             stride,
                       GrdPixelFormat  format,
                       uint8_t        *buf)
{
  g_autofree char *cursor = NULL;
  g_autofree char *mask = NULL;
  int y;

  g_return_val_if_fail (format == GRD_PIXEL_FORMAT_RGBA8888, NULL);

  cursor = g_new0 (char, width * height);
  mask = g_new0 (char, width * height);

  for (y = 0; y < height; y++)
    {
      uint32_t *pixel_row;
      int x;

      pixel_row = (uint32_t *) &buf[y * stride];

      for (x = 0; x < width; x++)
        {
          uint32_t pixel = pixel_row[x];
          uint8_t a, r, g, b;

          get_pixel_components (pixel,
                                format,
                                &a, &r, &g, &b);

          if (is_practically_opaque (a))
            {
              if (is_practically_black (r, g, b))
                cursor[y * width + x] = ' ';
              else
                cursor[y * width + x] = 'x';

              mask[y * width + x] = 'x';
            }
          else
            {
              cursor[y * width + x] = ' ';
              mask[y * width + x] = ' ';
            }
        }
    }

  return rfbMakeXCursor (width, height, cursor, mask);
}

rfbCursorPtr
grd_vnc_create_empty_cursor (int width,
                             int height)
{
  g_autofree char *cursor = NULL;
  cursor = g_new0 (char, width * height);

  memset (cursor, ' ', width * height);

  return rfbMakeXCursor (width, height, cursor, cursor);
}
