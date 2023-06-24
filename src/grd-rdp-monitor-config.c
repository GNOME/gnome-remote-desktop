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

#include "config.h"

#include "grd-rdp-monitor-config.h"

#include <cairo/cairo.h>

#define CLAMP_DESKTOP_SIZE(value) MAX (MIN (value, 8192), 200)

static uint32_t
sanitize_value (uint32_t value,
                uint32_t lower_bound,
                uint32_t upper_bound)
{
  g_assert (lower_bound < upper_bound);
  g_assert (lower_bound > 0);

  if (value < lower_bound || value > upper_bound)
    return 0;

  return value;
}

GrdRdpMonitorOrientation
transform_monitor_orientation (uint32_t value)
{
  switch (value)
    {
    case ORIENTATION_LANDSCAPE:
      return GRD_RDP_MONITOR_DIRECTION_LANDSCAPE;
    case ORIENTATION_PORTRAIT:
      return GRD_RDP_MONITOR_DIRECTION_PORTRAIT;
    case ORIENTATION_LANDSCAPE_FLIPPED:
      return GRD_RDP_MONITOR_DIRECTION_LANDSCAPE_FLIPPED;
    case ORIENTATION_PORTRAIT_FLIPPED:
      return GRD_RDP_MONITOR_DIRECTION_PORTRAIT_FLIPPED;
    default:
      return GRD_RDP_MONITOR_DIRECTION_LANDSCAPE;
    }

  g_assert_not_reached ();
}

static gboolean
write_sanitized_monitor_data (GrdRdpVirtualMonitor  *monitor,
                              int32_t                pos_x,
                              int32_t                pos_y,
                              uint32_t               width,
                              uint32_t               height,
                              gboolean               is_primary,
                              uint32_t               physical_width,
                              uint32_t               physical_height,
                              uint32_t               orientation,
                              uint32_t               scale,
                              GError               **error)
{
  if (width % 2 ||
      width < 200 || height < 200 ||
      width > 8192 || height > 8192)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid monitor dimensions");
      return FALSE;
    }

  monitor->pos_x = pos_x;
  monitor->pos_y = pos_y;
  monitor->width = CLAMP_DESKTOP_SIZE (width);
  monitor->height = CLAMP_DESKTOP_SIZE (height);
  monitor->is_primary = is_primary;

  monitor->physical_width = sanitize_value (physical_width, 10, 10000);
  monitor->physical_height = sanitize_value (physical_height, 10, 10000);
  if (monitor->physical_width == 0 || monitor->physical_height == 0)
    monitor->physical_width = monitor->physical_height = 0;

  monitor->orientation = transform_monitor_orientation (orientation);
  monitor->scale = sanitize_value (scale, 100, 500);

  return TRUE;
}

/**
 * Create a new monitor config from the Client Core Data data block
 * ([MS-RDPBCGR] 2.2.1.3.2 Client Core Data (TS_UD_CS_CORE))
 */
static GrdRdpMonitorConfig *
create_monitor_config_from_client_core_data (rdpSettings  *rdp_settings,
                                             GError      **error)
{
  g_autoptr (GrdRdpMonitorConfig) monitor_config = NULL;
  GrdRdpVirtualMonitor *virtual_monitor;

  monitor_config = g_malloc0 (sizeof (GrdRdpMonitorConfig));
  monitor_config->is_virtual = TRUE;
  monitor_config->monitor_count = 1;
  monitor_config->virtual_monitors = g_new0 (GrdRdpVirtualMonitor, 1);

  virtual_monitor = &monitor_config->virtual_monitors[0];

  /* Ignore the DeviceScaleFactor. It is deprecated (Win 8.1 only) */
  if (!write_sanitized_monitor_data (virtual_monitor,
                                     0, 0,
                                     rdp_settings->DesktopWidth,
                                     rdp_settings->DesktopHeight,
                                     TRUE,
                                     rdp_settings->DesktopPhysicalWidth,
                                     rdp_settings->DesktopPhysicalHeight,
                                     rdp_settings->DesktopOrientation,
                                     rdp_settings->DesktopScaleFactor,
                                     error))
    return NULL;

  monitor_config->desktop_width = virtual_monitor->width;
  monitor_config->desktop_height = virtual_monitor->height;

  return g_steal_pointer (&monitor_config);
}

static gboolean
determine_primary_monitor (GrdRdpMonitorConfig  *monitor_config,
                           GError              **error)
{
  uint32_t i;

  for (i = 0; i < monitor_config->monitor_count; ++i)
    {
      if (monitor_config->virtual_monitors[i].pos_x == 0 &&
          monitor_config->virtual_monitors[i].pos_y == 0)
        {
          monitor_config->virtual_monitors[i].is_primary = TRUE;
          return TRUE;
        }
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "No suitable primary monitor in monitor layout");

  return FALSE;
}

static gboolean
verify_monitor_config (GrdRdpMonitorConfig  *monitor_config,
                       GError              **error)
{
  cairo_region_t *region;
  cairo_rectangle_int_t rect;
  uint32_t i;

  /* Check for overlapping monitors */
  region = cairo_region_create ();
  for (i = 0; i < monitor_config->monitor_count; ++i)
    {
      rect.x = monitor_config->virtual_monitors[i].pos_x;
      rect.y = monitor_config->virtual_monitors[i].pos_y;
      rect.width = monitor_config->virtual_monitors[i].width;
      rect.height = monitor_config->virtual_monitors[i].height;

      if (cairo_region_contains_rectangle (region, &rect) != CAIRO_REGION_OVERLAP_OUT)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Monitor overlaps other monitor in layout");
          cairo_region_destroy (region);
          return FALSE;
        }
      cairo_region_union_rectangle (region, &rect);
    }

  /* Calculate the size of the desktop and Graphics Output Buffer ADM element */
  cairo_region_get_extents (region, &rect);
  monitor_config->desktop_width = rect.width;
  monitor_config->desktop_height = rect.height;

  /* We already checked, that the primary monitor is at (0, 0) */
  g_assert (rect.x <= 0);
  g_assert (rect.y <= 0);

  /* Determine monitor offset in input- and output-region */
  monitor_config->layout_offset_x = rect.x;
  monitor_config->layout_offset_y = rect.y;
  cairo_region_destroy (region);

  return TRUE;
}

/**
 * Create a new monitor config from the Client Monitor Data packet
 * ([MS-RDPBCGR] 2.2.1.3.6 Client Monitor Data (TS_UD_CS_MONITOR))
 */
static GrdRdpMonitorConfig *
create_monitor_config_from_client_monitor_data (rdpSettings  *rdp_settings,
                                                GError      **error)
{
  g_autoptr (GrdRdpMonitorConfig) monitor_config = NULL;
  uint32_t monitor_count = rdp_settings->MonitorCount;
  gboolean found_primary_monitor = FALSE;
  uint32_t i;

  g_assert (monitor_count > 0);

  monitor_config = g_malloc0 (sizeof (GrdRdpMonitorConfig));
  monitor_config->is_virtual = TRUE;
  monitor_config->monitor_count = monitor_count;
  monitor_config->virtual_monitors = g_new0 (GrdRdpVirtualMonitor,
                                             monitor_count);

  for (i = 0; i < monitor_count; ++i)
    {
      rdpMonitor *monitor = &rdp_settings->MonitorDefArray[i];
      MONITOR_ATTRIBUTES *monitor_attributes = &monitor->attributes;
      gboolean is_primary = !!monitor->is_primary;
      uint32_t physical_width = 0;
      uint32_t physical_height = 0;
      uint32_t orientation = 0;
      uint32_t scale = 0;

      if (found_primary_monitor || monitor->x != 0 || monitor->y != 0)
        is_primary = FALSE;
      if (!found_primary_monitor && is_primary)
        found_primary_monitor = TRUE;

      if (rdp_settings->HasMonitorAttributes)
        {
          physical_width = monitor_attributes->physicalWidth;
          physical_height = monitor_attributes->physicalHeight;
          orientation = monitor_attributes->orientation;
          scale = monitor_attributes->desktopScaleFactor;
        }

      /* Ignore the DeviceScaleFactor. It is deprecated (Win 8.1 only) */
      if (!write_sanitized_monitor_data (&monitor_config->virtual_monitors[i],
                                         monitor->x,
                                         monitor->y,
                                         monitor->width,
                                         monitor->height,
                                         is_primary,
                                         physical_width,
                                         physical_height,
                                         orientation,
                                         scale,
                                         error))
        return NULL;
    }

  if (!found_primary_monitor &&
      !determine_primary_monitor (monitor_config, error))
    return NULL;

  if (!verify_monitor_config (monitor_config, error))
    return NULL;

  return g_steal_pointer (&monitor_config);
}

GrdRdpMonitorConfig *
grd_rdp_monitor_config_new_from_client_data (rdpSettings  *rdp_settings,
                                             uint32_t      max_monitor_count,
                                             GError      **error)
{
  if (rdp_settings->MonitorCount == 0 ||
      rdp_settings->MonitorCount > max_monitor_count)
    return create_monitor_config_from_client_core_data (rdp_settings, error);

  return create_monitor_config_from_client_monitor_data (rdp_settings, error);
}

/**
 * Create a new monitor config from DISPLAYCONTROL_MONITOR_LAYOUT PDU
 * ([MS-RDPEDISP] 2.2.2.2.1 DISPLAYCONTROL_MONITOR_LAYOUT)
 */
GrdRdpMonitorConfig *
grd_rdp_monitor_config_new_from_disp_monitor_layout (const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU  *monitor_layout,
                                                     GError                                   **error)
{
  g_autoptr (GrdRdpMonitorConfig) monitor_config = NULL;
  uint32_t monitor_count = monitor_layout->NumMonitors;
  gboolean found_primary_monitor = FALSE;
  uint32_t i;

  if (monitor_count == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Monitor Layout PDU contains no monitors");
      return NULL;
    }

  monitor_config = g_malloc0 (sizeof (GrdRdpMonitorConfig));
  monitor_config->is_virtual = TRUE;
  monitor_config->monitor_count = monitor_count;
  monitor_config->virtual_monitors = g_new0 (GrdRdpVirtualMonitor,
                                             monitor_count);

  for (i = 0; i < monitor_count; ++i)
    {
      DISPLAY_CONTROL_MONITOR_LAYOUT *monitor = &monitor_layout->Monitors[i];
      gboolean is_primary;

      is_primary = !!(monitor->Flags & DISPLAY_CONTROL_MONITOR_PRIMARY);
      if (found_primary_monitor || monitor->Left != 0 || monitor->Top != 0)
        is_primary = FALSE;
      if (!found_primary_monitor && is_primary)
        found_primary_monitor = TRUE;

      /* Ignore the DeviceScaleFactor. It is deprecated (Win 8.1 only) */
      if (!write_sanitized_monitor_data (&monitor_config->virtual_monitors[i],
                                         monitor->Left,
                                         monitor->Top,
                                         monitor->Width,
                                         monitor->Height,
                                         is_primary,
                                         monitor->PhysicalWidth,
                                         monitor->PhysicalHeight,
                                         monitor->Orientation,
                                         monitor->DesktopScaleFactor,
                                         error))
        return NULL;
    }

  if (!found_primary_monitor &&
      !determine_primary_monitor (monitor_config, error))
    return NULL;

  if (!verify_monitor_config (monitor_config, error))
    return NULL;

  return g_steal_pointer (&monitor_config);
}

void
grd_rdp_monitor_config_free (GrdRdpMonitorConfig *monitor_config)
{
  g_clear_pointer (&monitor_config->connectors, g_strfreev);
  g_free (monitor_config->virtual_monitors);
  g_free (monitor_config);
}
