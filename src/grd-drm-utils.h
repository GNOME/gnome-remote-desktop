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

#pragma once

#include <glib.h>
#include <stdint.h>

gboolean grd_drm_render_node_supports_explicit_sync (int device_fd);

gboolean grd_create_drm_syncobj_handle (int        device_fd,
                                        int        syncobj_fd,
                                        uint32_t  *out_handle,
                                        GError   **error);

gboolean grd_wait_for_drm_timeline_point (int        device_fd,
                                          uint32_t   syncobj_handle,
                                          uint64_t   timeline_point,
                                          GError   **error);

int grd_export_drm_timeline_syncfile (int        device_fd,
                                      uint32_t   syncobj_handle,
                                      uint64_t   timeline_point,
                                      GError   **error);

gboolean grd_signal_drm_timeline_point (int        device_fd,
                                        int        syncobj_fd,
                                        uint64_t   timeline_point,
                                        GError   **error);
