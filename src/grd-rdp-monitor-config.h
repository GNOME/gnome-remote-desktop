/*
 * Copyright (C) 2021 Pascal Nowack
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

#pragma once

#include <freerdp/channels/disp.h>
#include <freerdp/freerdp.h>
#include <gio/gio.h>

typedef enum _GrdRdpMonitorOrientation
{
  /* The monitor is not rotated */
  GRD_RDP_MONITOR_DIRECTION_LANDSCAPE         = 0,
  /* The monitor is rotated clockwise by 90 degrees */
  GRD_RDP_MONITOR_DIRECTION_PORTRAIT          = 1,
  /* The monitor is rotated clockwise by 180 degrees */
  GRD_RDP_MONITOR_DIRECTION_LANDSCAPE_FLIPPED = 2,
  /* The monitor is rotated clockwise by 270 degrees */
  GRD_RDP_MONITOR_DIRECTION_PORTRAIT_FLIPPED  = 3,
} GrdRdpMonitorOrientation;

typedef struct _GrdRdpVirtualMonitor
{
  int32_t pos_x;
  int32_t pos_y;
  uint32_t width;           /* Valid values are in range of [200, 8192] */
  uint32_t height;          /* Valid values are in range of [200, 8192] */
  gboolean is_primary;

  /* Physical width of the monitor in millimeters. Ignore, if 0 */
  uint32_t physical_width;  /* Valid values are in range of [10, 10000] */
  /* Physical height of the monitor in millimeters. Ignore, if 0 */
  uint32_t physical_height; /* Valid values are in range of [10, 10000] */

  GrdRdpMonitorOrientation orientation;

  /* Monitor scale in percent. Ignore, if 0 */
  uint32_t scale;           /* Valid values are in range of [100, 500] */
} GrdRdpVirtualMonitor;

typedef struct _GrdRdpMonitorConfig
{
  gboolean is_virtual;

  /* Server configs only */
  char **connectors;
  /* Client configs only */
  GrdRdpVirtualMonitor *virtual_monitors;
  /* Count of items in connectors or virtual_monitors */
  uint32_t monitor_count;

  /* Offset between monitor layout, and input- and output-region */
  int32_t layout_offset_x;
  int32_t layout_offset_y;

  /* Size of the Desktop (and the Graphics Output Buffer ADM element) */
  uint32_t desktop_width;
  uint32_t desktop_height;
} GrdRdpMonitorConfig;

GrdRdpMonitorConfig *grd_rdp_monitor_config_new_from_client_data (rdpSettings  *rdp_settings,
                                                                  uint32_t      max_monitor_count,
                                                                  GError      **error);

GrdRdpMonitorConfig *grd_rdp_monitor_config_new_from_disp_monitor_layout (const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU  *monitor_layout,
                                                                          GError                                   **error);

void grd_rdp_monitor_config_free (GrdRdpMonitorConfig *monitor_config);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdRdpMonitorConfig, grd_rdp_monitor_config_free)
