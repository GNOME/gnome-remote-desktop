/*
 * Copyright (C) 2024 Pascal Nowack
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

#include "grd-rdp-pw-buffer.h"

struct _GrdRdpPwBuffer
{
  GMutex buffer_mutex;
  gboolean is_locked;
};

void
grd_rdp_pw_buffer_ensure_unlocked (GrdRdpPwBuffer *rdp_pw_buffer)
{
  g_mutex_lock (&rdp_pw_buffer->buffer_mutex);
  g_mutex_unlock (&rdp_pw_buffer->buffer_mutex);
}

void
grd_rdp_pw_buffer_acquire_lock (GrdRdpPwBuffer *rdp_pw_buffer)
{
  g_mutex_lock (&rdp_pw_buffer->buffer_mutex);
  g_assert (!rdp_pw_buffer->is_locked);
  rdp_pw_buffer->is_locked = TRUE;
}

void
grd_rdp_pw_buffer_maybe_release_lock (GrdRdpPwBuffer *rdp_pw_buffer)
{
  if (!rdp_pw_buffer->is_locked)
    return;

  rdp_pw_buffer->is_locked = FALSE;
  g_mutex_unlock (&rdp_pw_buffer->buffer_mutex);
}

static enum spa_data_type
get_pw_buffer_type (struct pw_buffer *pw_buffer)
{
  struct spa_buffer *spa_buffer = pw_buffer->buffer;

  return spa_buffer->datas[0].type;
}

GrdRdpPwBuffer *
grd_rdp_pw_buffer_new (struct pw_buffer  *pw_buffer,
                       GError           **error)
{
  g_autoptr (GrdRdpPwBuffer) rdp_pw_buffer = NULL;

  rdp_pw_buffer = g_new0 (GrdRdpPwBuffer, 1);

  g_mutex_init (&rdp_pw_buffer->buffer_mutex);

  if (get_pw_buffer_type (pw_buffer) != SPA_DATA_MemFd &&
      get_pw_buffer_type (pw_buffer) != SPA_DATA_DmaBuf)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "PipeWire buffer contains invalid data type 0x%08X",
                   get_pw_buffer_type (pw_buffer));
      return NULL;
    }

  return g_steal_pointer (&rdp_pw_buffer);
}

void
grd_rdp_pw_buffer_free (GrdRdpPwBuffer *rdp_pw_buffer)
{
  g_mutex_clear (&rdp_pw_buffer->buffer_mutex);
  g_free (rdp_pw_buffer);
}
