/*
 * Copyright (C) 2015 Red Hat Inc.
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
 *
 * Written by:
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#ifndef GRD_TYPES_H
#define GRD_TYPES_H

typedef struct _GrdContext GrdContext;
typedef struct _GrdClipboard GrdClipboard;
typedef struct _GrdClipboardRdp GrdClipboardRdp;
typedef struct _GrdClipboardVnc GrdClipboardVnc;
typedef struct _GrdEglThread GrdEglThread;
typedef struct _GrdHwAccelNvidia GrdHwAccelNvidia;
typedef struct _GrdRdpDamageDetector GrdRdpDamageDetector;
typedef struct _GrdRdpDisplayControl GrdRdpDisplayControl;
typedef struct _GrdRdpEventQueue GrdRdpEventQueue;
typedef struct _GrdRdpBuffer GrdRdpBuffer;
typedef struct _GrdRdpBufferPool GrdRdpBufferPool;
typedef struct _GrdRdpGfxFrameController GrdRdpGfxFrameController;
typedef struct _GrdRdpGfxFrameLog GrdRdpGfxFrameLog;
typedef struct _GrdRdpGfxSurface GrdRdpGfxSurface;
typedef struct _GrdRdpGraphicsPipeline GrdRdpGraphicsPipeline;
typedef struct _GrdRdpNetworkAutodetection GrdRdpNetworkAutodetection;
typedef struct _GrdRdpSAMFile GrdRdpSAMFile;
typedef struct _GrdRdpServer GrdRdpServer;
typedef struct _GrdRdpSurface GrdRdpSurface;
typedef struct _GrdSession GrdSession;
typedef struct _GrdSessionRdp GrdSessionRdp;
typedef struct _GrdSessionVnc GrdSessionVnc;
typedef struct _GrdStream GrdStream;
typedef struct _GrdPipeWireStream GrdPipeWireStream;
typedef struct _GrdPipeWireStreamMonitor GrdPipeWireStreamMonitor;
typedef struct _GrdVncServer GrdVncServer;

typedef enum _GrdPixelFormat
{
  GRD_PIXEL_FORMAT_RGBA8888,
} GrdPixelFormat;

#endif /* GRD_TYPES_H */
