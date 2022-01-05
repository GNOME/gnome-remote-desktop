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

#include "config.h"

#include "grd-rdp-damage-detector.h"

G_DEFINE_TYPE (GrdRdpDamageDetector, grd_rdp_damage_detector, G_TYPE_OBJECT);

gboolean
grd_rdp_damage_detector_invalidate_surface (GrdRdpDamageDetector *detector)
{
  GrdRdpDamageDetectorClass *klass =
    GRD_RDP_DAMAGE_DETECTOR_GET_CLASS (detector);

  return klass->invalidate_surface (detector);
}

gboolean
grd_rdp_damage_detector_resize_surface (GrdRdpDamageDetector *detector,
                                        uint32_t              width,
                                        uint32_t              height)
{
  GrdRdpDamageDetectorClass *klass =
    GRD_RDP_DAMAGE_DETECTOR_GET_CLASS (detector);

  return klass->resize_surface (detector, width, height);
}

gboolean
grd_rdp_damage_detector_submit_new_framebuffer (GrdRdpDamageDetector *detector,
                                                GrdRdpBuffer         *buffer)
{
  GrdRdpDamageDetectorClass *klass =
    GRD_RDP_DAMAGE_DETECTOR_GET_CLASS (detector);

  return klass->submit_new_framebuffer (detector, buffer);
}

gboolean
grd_rdp_damage_detector_is_region_damaged (GrdRdpDamageDetector *detector)
{
  GrdRdpDamageDetectorClass *klass =
    GRD_RDP_DAMAGE_DETECTOR_GET_CLASS (detector);

  return klass->is_region_damaged (detector);
}

cairo_region_t *
grd_rdp_damage_detector_get_damage_region (GrdRdpDamageDetector *detector)
{
  GrdRdpDamageDetectorClass *klass =
    GRD_RDP_DAMAGE_DETECTOR_GET_CLASS (detector);

  return klass->get_damage_region (detector);
}

static void
grd_rdp_damage_detector_init (GrdRdpDamageDetector *detector)
{
}

static void
grd_rdp_damage_detector_class_init (GrdRdpDamageDetectorClass *klass)
{
}
