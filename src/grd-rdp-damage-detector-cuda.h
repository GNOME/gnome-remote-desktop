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

#ifndef GRD_RDP_DAMAGE_DETECTOR_CUDA_H
#define GRD_RDP_DAMAGE_DETECTOR_CUDA_H

#include <ffnvcodec/dynlink_loader.h>

#include "grd-rdp-damage-detector.h"

#define GRD_TYPE_RDP_DAMAGE_DETECTOR_CUDA (grd_rdp_damage_detector_cuda_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpDamageDetectorCuda, grd_rdp_damage_detector_cuda,
                      GRD, RDP_DAMAGE_DETECTOR_CUDA, GrdRdpDamageDetector)

GrdRdpDamageDetectorCuda *grd_rdp_damage_detector_cuda_new (GrdHwAccelNvidia *hwaccel_nvidia,
                                                            CUstream          cuda_stream);

#endif /* GRD_RDP_DAMAGE_DETECTOR_CUDA_H */
