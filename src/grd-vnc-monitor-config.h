/*
 * Copyright (C) 2022 Pascal Nowack
 * Copyright (C) 2022 SUSE Software Solutions Germany GmbH
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

#ifndef GRD_VNC_MONITOR_CONFIG_H
#define GRD_VNC_MONITOR_CONFIG_H

#define GRD_VNC_CLAMP_DESKTOP_SIZE(value) MAX (MIN (value, 8192), 200)
#define GRD_VNC_DEFAULT_WIDTH             1920
#define GRD_VNC_DEFAULT_HEIGHT            1080

typedef struct _GrdVncVirtualMonitor
{
  uint16_t pos_x;
  uint16_t pos_y;
  uint16_t width;  /* Valid values are in range of [200, 8192] */
  uint16_t height; /* Valid values are in range of [200, 8192] */
} GrdVncVirtualMonitor;

typedef struct _GrdVncMonitorConfig
{
  gboolean is_virtual;

  /* Server configs only */
  char **connectors;
  /* Client configs only */
  GrdVncVirtualMonitor *virtual_monitors;
  /* Count of items in connectors or virtual_monitors */
  uint32_t monitor_count;
} GrdVncMonitorConfig;

#endif /* GRD_VNC_MONITOR_CONFIG_H */
