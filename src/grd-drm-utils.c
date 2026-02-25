/*
 * Copyright (C) 2026 Red Hat Inc.
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

#include "grd-drm-utils.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <xf86drm.h>

gboolean
grd_drm_render_node_supports_explicit_sync (int device_fd)
{
  uint64_t syncobj_cap = 0;
  uint64_t syncobj_timeline_cap = 0;

  drmGetCap (device_fd, DRM_CAP_SYNCOBJ, &syncobj_cap);
  drmGetCap (device_fd, DRM_CAP_SYNCOBJ_TIMELINE, &syncobj_timeline_cap);

  return syncobj_cap && syncobj_timeline_cap;
}

int
grd_export_drm_timeline_syncfile (int        device_fd,
                                  uint32_t   syncobj_handle,
                                  uint64_t   timeline_point,
                                  GError   **error)
{
  uint32_t export_handle;
  int ret;
  g_autofd int syncfile_fd = -1;

  ret = drmSyncobjCreate (device_fd, 0, &export_handle);
  if (ret < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "drmSyncobjCreate failed: %s", g_strerror (errno));
      return -1;
    }

  ret = drmSyncobjTransfer (device_fd,
                            export_handle, 0,
                            syncobj_handle, timeline_point,
                            0);
  if (ret < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "drmSyncobjCreate failed: %s", g_strerror (errno));
      return -1;
    }

  ret = drmSyncobjExportSyncFile (device_fd, export_handle, &syncfile_fd);
  if (ret < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "drmSyncobjExportSyncFile failed: %s", g_strerror (errno));
      return -1;
    }

  ret = drmSyncobjDestroy (device_fd, export_handle);
  if (ret < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "drmSyncobjDestroy failed: %s", g_strerror (errno));
      return -1;
    }

  return g_steal_fd (&syncfile_fd);
}

gboolean
grd_create_drm_syncobj_handle (int        device_fd,
                               int        syncobj_fd,
                               uint32_t  *out_handle,
                               GError   **error)
{
  int ret;

  ret = drmSyncobjFDToHandle (device_fd, syncobj_fd, out_handle);
  if (ret < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "drmSyncobjFDToHandle failed: %s", g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

gboolean
grd_wait_for_drm_timeline_point (int        device_fd,
                                 uint32_t   syncobj_handle,
                                 uint64_t   timeline_point,
                                 GError   **error)
{
  int ret;

  ret = drmSyncobjTimelineWait (device_fd,
                                &syncobj_handle, &timeline_point, 1,
                                INT64_MAX,
                                DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
                                NULL);
  if (ret < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "drmSyncobjTimelineWait failed: %s", g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

gboolean
grd_signal_drm_timeline_point (int        device_fd,
                               int        syncobj_fd,
                               uint64_t   timeline_point,
                               GError   **error)
{
  uint32_t syncobj_handle;

  if (drmSyncobjFDToHandle (device_fd, syncobj_fd, &syncobj_handle) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "drmSyncobjFDToHandle failed: %s", g_strerror (errno));
      return FALSE;
    }

  if (drmSyncobjTimelineSignal (device_fd, &syncobj_handle, &timeline_point, 1) < 0)
    {
      drmSyncobjDestroy (device_fd, syncobj_handle);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "drmSyncobjTimelineSignal failed: %s", g_strerror (errno));
      return FALSE;
    }

  drmSyncobjDestroy (device_fd, syncobj_handle);

  return TRUE;
}
