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

#include <linux/dma-buf.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/raw.h>
#include <sys/ioctl.h>
#include <errno.h>

static gboolean is_pipewire_initialized = FALSE;

void
grd_maybe_initialize_pipewire (void)
{
  if (!is_pipewire_initialized)
    {
      pw_init (NULL, NULL);
      is_pipewire_initialized = TRUE;
    }
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

void
grd_sync_dma_buf (int      fd,
                  uint64_t start_or_end)
{
  struct dma_buf_sync sync = { 0 };

  sync.flags = start_or_end | DMA_BUF_SYNC_READ;

  while (TRUE)
    {
      int ret;

      ret = ioctl (fd, DMA_BUF_IOCTL_SYNC, &sync);
      if (ret == -1 && errno == EINTR)
        {
          continue;
        }
      else if (ret == -1)
        {
          g_warning ("Failed to synchronize DMA buffer: %s",
                     g_strerror (errno));
          break;
        }
      else
        {
          break;
        }
    }
}
