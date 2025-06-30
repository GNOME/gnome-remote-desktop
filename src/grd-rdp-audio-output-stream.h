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

#include <glib-object.h>
#include <spa/param/audio/raw.h>
#include <pipewire/pipewire.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_AUDIO_OUTPUT_STREAM (grd_rdp_audio_output_stream_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpAudioOutputStream, grd_rdp_audio_output_stream,
                      GRD, RDP_AUDIO_OUTPUT_STREAM, GObject)

struct _GrdRdpAudioVolumeData
{
  gboolean audio_muted;

  float volumes[SPA_AUDIO_MAX_CHANNELS];
  uint32_t n_volumes;
};

GrdRdpAudioOutputStream *grd_rdp_audio_output_stream_new (GrdRdpDvcAudioPlayback  *audio_playback,
                                                          struct pw_core          *pipewire_core,
                                                          struct pw_registry      *pipewire_registry,
                                                          uint32_t                 target_node_id,
                                                          uint32_t                 n_samples_per_sec,
                                                          uint32_t                 n_channels,
                                                          uint32_t                *position,
                                                          GError                 **error);

void grd_rdp_audio_output_stream_set_active (GrdRdpAudioOutputStream *audio_output_stream,
                                             gboolean                 active);

void grd_rdp_audio_output_stream_get_volume_data (GrdRdpAudioOutputStream *audio_output_stream,
                                                  GrdRdpAudioVolumeData   *volume_data);
