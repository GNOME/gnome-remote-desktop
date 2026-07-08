/*
 * Copyright (C) 2026 Red Hat Inc.
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
 *     Joan Torres Lopez <joantolo@redhat.com>
 */

#pragma once

#include <glib.h>

typedef unsigned long DWORD;
typedef DWORD *LPDWORD;
typedef long LONG;
typedef LONG SCARDCONTEXT;
typedef SCARDCONTEXT *LPSCARDCONTEXT;
typedef LONG SCARDHANDLE;
typedef SCARDHANDLE *LPSCARDHANDLE;
typedef const void *LPCVOID;
typedef void *LPVOID;
typedef unsigned char BYTE;
typedef BYTE *LPBYTE;
typedef const BYTE *LPCBYTE;
typedef const char *LPCSTR;
typedef char *LPSTR;

#define MAX_ATR_SIZE 33

typedef struct
{
  const char *szReader;
  void *pvUserData;
  DWORD dwCurrentState;
  DWORD dwEventState;
  DWORD cbAtr;
  unsigned char rgbAtr[MAX_ATR_SIZE];
} SCARD_READERSTATE;

typedef struct
{
  unsigned long dwProtocol;
  unsigned long cbPciLength;
} SCARD_IO_REQUEST;

#define SCARD_S_SUCCESS              ((LONG) 0x00000000)
#define SCARD_F_INTERNAL_ERROR       ((LONG) 0x80100001)
#define SCARD_E_INSUFFICIENT_BUFFER  ((LONG) 0x80100008)

typedef struct
{
  LONG (*SCardEstablishContext) (DWORD,
                                 LPCVOID,
                                 LPCVOID,
                                 LPSCARDCONTEXT);
  LONG (*SCardReleaseContext) (SCARDCONTEXT);
  LONG (*SCardIsValidContext) (SCARDCONTEXT);
  LONG (*SCardConnect) (SCARDCONTEXT,
                        LPCSTR,
                        DWORD,
                        DWORD,
                        LPSCARDHANDLE,
                        LPDWORD);
  LONG (*SCardReconnect) (SCARDHANDLE,
                          DWORD,
                          DWORD,
                          DWORD,
                          LPDWORD);
  LONG (*SCardDisconnect) (SCARDHANDLE,
                           DWORD);
  LONG (*SCardBeginTransaction) (SCARDHANDLE);
  LONG (*SCardEndTransaction) (SCARDHANDLE,
                               DWORD);
  LONG (*SCardStatus) (SCARDHANDLE,
                       LPSTR,
                       LPDWORD,
                       LPDWORD,
                       LPDWORD,
                       LPBYTE,
                       LPDWORD);
  LONG (*SCardGetStatusChange) (SCARDCONTEXT,
                                DWORD,
                                SCARD_READERSTATE *,
                                DWORD);
  LONG (*SCardControl) (SCARDHANDLE,
                        DWORD,
                        LPCVOID,
                        DWORD,
                        LPVOID,
                        DWORD,
                        LPDWORD);
  LONG (*SCardTransmit) (SCARDHANDLE,
                         const SCARD_IO_REQUEST *,
                         LPCBYTE,
                         DWORD,
                         SCARD_IO_REQUEST *,
                         LPBYTE,
                         LPDWORD);
  LONG (*SCardListReaderGroups) (SCARDCONTEXT,
                                 LPSTR,
                                 LPDWORD);
  LONG (*SCardListReaders) (SCARDCONTEXT,
                            LPCSTR,
                            LPSTR,
                            LPDWORD);
  LONG (*SCardFreeMemory) (SCARDCONTEXT,
                           LPCVOID);
  LONG (*SCardCancel) (SCARDCONTEXT);
  LONG (*SCardGetAttrib) (SCARDHANDLE,
                          DWORD,
                          LPBYTE,
                          LPDWORD);
  LONG (*SCardSetAttrib) (SCARDHANDLE,
                          DWORD,
                          LPCBYTE,
                          DWORD);
} GrdPcscLibBackend;

void grd_pcsc_lib_backend_set_default (GrdPcscLibBackend *backend);

gboolean grd_pcsc_lib_backend_init_grd_pcscd_proxy (GrdPcscLibBackend *backend);

void grd_pcsc_lib_backend_cleanup_grd_pcscd_proxy (void);

gboolean grd_pcsc_lib_backend_init_libpcsclite (GrdPcscLibBackend *backend);

void grd_pcsc_lib_backend_cleanup_libpcsclite (void);
