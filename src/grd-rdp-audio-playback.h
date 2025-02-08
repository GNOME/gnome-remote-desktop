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

#ifndef GRD_RDP_AUDIO_PLAYBACK_H
#define GRD_RDP_AUDIO_PLAYBACK_H

#include <freerdp/server/rdpsnd.h>
#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_RDP_AUDIO_PLAYBACK (grd_rdp_audio_playback_get_type ())
G_DECLARE_FINAL_TYPE (GrdRdpAudioPlayback, grd_rdp_audio_playback,
                      GRD, RDP_AUDIO_PLAYBACK, GObject)

GrdRdpAudioPlayback *grd_rdp_audio_playback_new (GrdSessionRdp    *session_rdp,
                                                 GrdRdpDvcHandler *dvc_handler,
                                                 HANDLE            vcm,
                                                 rdpContext       *rdp_context);

void grd_rdp_audio_playback_maybe_init (GrdRdpAudioPlayback *audio_playback);

void grd_rdp_audio_playback_maybe_submit_samples (GrdRdpAudioPlayback   *audio_playback,
                                                  uint32_t               node_id,
                                                  GrdRdpAudioVolumeData *volume_data,
                                                  int16_t               *data,
                                                  uint32_t               size);

#endif /* GRD_RDP_AUDIO_PLAYBACK_H */
