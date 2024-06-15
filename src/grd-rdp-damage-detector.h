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

#ifndef GRD_RDP_DAMAGE_DETECTOR_H
#define GRD_RDP_DAMAGE_DETECTOR_H

#include <cairo/cairo.h>
#include <glib-object.h>
#include <stdint.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_DAMAGE_DETECTOR (grd_rdp_damage_detector_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdRdpDamageDetector, grd_rdp_damage_detector,
                          GRD, RDP_DAMAGE_DETECTOR, GObject)

struct _GrdRdpDamageDetectorClass
{
  GObjectClass parent_class;

  gboolean (* invalidate_surface) (GrdRdpDamageDetector *detector);
  gboolean (* resize_surface) (GrdRdpDamageDetector *detector,
                               uint32_t              width,
                               uint32_t              height);
  gboolean (* submit_new_framebuffer) (GrdRdpDamageDetector *detector,
                                       GrdRdpLegacyBuffer   *buffer);
  gboolean (* is_region_damaged) (GrdRdpDamageDetector *detector);
  cairo_region_t *(* get_damage_region) (GrdRdpDamageDetector *detector);
};

gboolean grd_rdp_damage_detector_invalidate_surface (GrdRdpDamageDetector *detector);

gboolean grd_rdp_damage_detector_resize_surface (GrdRdpDamageDetector *detector,
                                                 uint32_t              width,
                                                 uint32_t              height);

gboolean grd_rdp_damage_detector_submit_new_framebuffer (GrdRdpDamageDetector *detector,
                                                         GrdRdpLegacyBuffer   *buffer);

gboolean grd_rdp_damage_detector_is_region_damaged (GrdRdpDamageDetector *detector);

cairo_region_t *grd_rdp_damage_detector_get_damage_region (GrdRdpDamageDetector *detector);

#endif /* GRD_RDP_DAMAGE_DETECTOR_H */
