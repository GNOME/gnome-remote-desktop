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

#ifndef GRD_UTILS_H
#define GRD_UTILS_H

#include <gio/gio.h>
#include <stdint.h>

typedef enum
{
  GRD_SYSTEMD_UNIT_ACTIVE_STATE_UNKNOWN = 0,
  GRD_SYSTEMD_UNIT_ACTIVE_STATE_ACTIVE,
  GRD_SYSTEMD_UNIT_ACTIVE_STATE_RELOADING,
  GRD_SYSTEMD_UNIT_ACTIVE_STATE_INACTIVE,
  GRD_SYSTEMD_UNIT_ACTIVE_STATE_FAILED,
  GRD_SYSTEMD_UNIT_ACTIVE_STATE_ACTIVATING,
  GRD_SYSTEMD_UNIT_ACTIVE_STATE_DEACTIVATING,
} GrdSystemdUnitActiveState;

typedef struct _GrdSyncPoint
{
  GCond sync_cond;
  GMutex sync_mutex;
  gboolean completed;

  gboolean success;
} GrdSyncPoint;

static inline uint32_t
grd_get_aligned_size (uint32_t size,
                      uint32_t alignment)
{
  return size + (size % alignment ? alignment - size % alignment : 0);
}

void grd_sync_point_init (GrdSyncPoint *sync_point);

void grd_sync_point_clear (GrdSyncPoint *sync_point);

void grd_sync_point_complete (GrdSyncPoint *sync_point,
                              gboolean      success);

gboolean grd_sync_point_wait_for_completion (GrdSyncPoint *sync_point);

GSource * grd_create_fd_source (int             fd,
                                const char     *name,
                                GSourceFunc     prepare,
                                GSourceFunc     dispatch,
                                gpointer        user_data,
                                GDestroyNotify  notify);

gboolean grd_bind_socket (GSocketListener  *server,
                          uint16_t          port,
                          uint16_t         *selected_port,
                          gboolean          port_negotiation_enabled,
                          GError          **error);

void grd_rewrite_path_to_user_data_dir (char       **path,
                                        const char  *subdir,
                                        const char  *fallback_path);

gboolean grd_write_fd_to_file (int            fd,
                               const char    *filename,
                               GCancellable  *cancellable,
                               GError       **error);

gboolean grd_test_fd (int         fd,
                      ssize_t     max_size,
                      GFileTest  *test_results,
                      GError    **error);

gboolean grd_toggle_systemd_unit (gboolean   enabled,
                                  GError   **error);

gboolean grd_systemd_get_unit (GBusType     bus_type,
                               const char  *unit,
                               GDBusProxy **proxy,
                               GError     **error);

gboolean grd_systemd_unit_get_active_state (GDBusProxy                 *unit_proxy,
                                            GrdSystemdUnitActiveState  *active_state,
                                            GError                    **error);

#endif /* GRD_UTILS_H */
