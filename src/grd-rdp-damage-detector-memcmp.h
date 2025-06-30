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

#pragma once

#include "grd-rdp-damage-detector.h"

#define GRD_TYPE_RDP_DAMAGE_DETECTOR_MEMCMP (grd_rdp_damage_detector_memcmp_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpDamageDetectorMemcmp, grd_rdp_damage_detector_memcmp,
                      GRD, RDP_DAMAGE_DETECTOR_MEMCMP, GrdRdpDamageDetector)

GrdRdpDamageDetectorMemcmp *grd_rdp_damage_detector_memcmp_new (void);
