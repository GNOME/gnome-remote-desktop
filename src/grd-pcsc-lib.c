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

#include "config.h"

#include "grd-pcsc-lib-backend.h"

#define PCSC_API __attribute__((visibility ("default")))
#define PCSC_DESTRUCTOR __attribute__ ((destructor))

static GrdPcscLibBackend backend;

PCSC_DESTRUCTOR static void
on_unload (void)
{
  grd_pcsc_lib_backend_cleanup_grd_pcscd_proxy ();
  grd_pcsc_lib_backend_cleanup_libpcsclite ();
}

static void
init_grd_pcsc_backend (void)
{
  grd_pcsc_lib_backend_set_default (&backend);

  if (grd_pcsc_lib_backend_init_grd_pcscd_proxy (&backend))
    return;

  if (grd_pcsc_lib_backend_init_libpcsclite (&backend))
    return;

  g_warning ("[PCSC] No backend available");
}

static void
ensure_initialized (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized))
    {
      init_grd_pcsc_backend ();
      g_once_init_leave (&initialized, 1);
    }
}

PCSC_API LONG
SCardEstablishContext (DWORD          dwScope,
                       LPCVOID        pvReserved1,
                       LPCVOID        pvReserved2,
                       LPSCARDCONTEXT phContext)
{
  ensure_initialized ();

  return backend.SCardEstablishContext (dwScope,
                                        pvReserved1,
                                        pvReserved2,
                                        phContext);
}

PCSC_API LONG
SCardReleaseContext (SCARDCONTEXT hContext)
{
  ensure_initialized ();

  return backend.SCardReleaseContext (hContext);
}

PCSC_API LONG
SCardIsValidContext (SCARDCONTEXT hContext)
{
  ensure_initialized ();

  return backend.SCardIsValidContext (hContext);
}

PCSC_API LONG
SCardConnect (SCARDCONTEXT  hContext,
              LPCSTR        szReader,
              DWORD         dwShareMode,
              DWORD         dwPreferredProtocols,
              LPSCARDHANDLE phCard,
              LPDWORD       pdwActiveProtocol)
{
  ensure_initialized ();

  return backend.SCardConnect (hContext,
                               szReader,
                               dwShareMode,
                               dwPreferredProtocols,
                               phCard,
                               pdwActiveProtocol);
}

PCSC_API LONG
SCardReconnect (SCARDHANDLE hCard,
                DWORD       dwShareMode,
                DWORD       dwPreferredProtocols,
                DWORD       dwInitialization,
                LPDWORD     pdwActiveProtocol)
{
  ensure_initialized ();

  return backend.SCardReconnect (hCard,
                                 dwShareMode,
                                 dwPreferredProtocols,
                                 dwInitialization,
                                 pdwActiveProtocol);
}

PCSC_API LONG
SCardDisconnect (SCARDHANDLE hCard,
                 DWORD       dwDisposition)
{
  ensure_initialized ();

  return backend.SCardDisconnect (hCard,
                                  dwDisposition);
}

PCSC_API LONG
SCardBeginTransaction (SCARDHANDLE hCard)
{
  ensure_initialized ();

  return backend.SCardBeginTransaction (hCard);
}

PCSC_API LONG
SCardEndTransaction (SCARDHANDLE hCard,
                     DWORD       dwDisposition)
{
  ensure_initialized ();

  return backend.SCardEndTransaction (hCard,
                                      dwDisposition);
}

PCSC_API LONG
SCardStatus (SCARDHANDLE hCard,
             LPSTR       mszReaderName,
             LPDWORD     pcchReaderLen,
             LPDWORD     pdwState,
             LPDWORD     pdwProtocol,
             LPBYTE      pbAtr,
             LPDWORD     pcbAtrLen)
{
  ensure_initialized ();

  return backend.SCardStatus (hCard,
                              mszReaderName,
                              pcchReaderLen,
                              pdwState,
                              pdwProtocol,
                              pbAtr,
                              pcbAtrLen);
}

PCSC_API LONG
SCardGetStatusChange (SCARDCONTEXT       hContext,
                      DWORD              dwTimeout,
                      SCARD_READERSTATE *rgReaderStates,
                      DWORD              cReaders)
{
  ensure_initialized ();

  return backend.SCardGetStatusChange (hContext,
                                       dwTimeout,
                                       rgReaderStates,
                                       cReaders);
}

PCSC_API LONG
SCardControl (SCARDHANDLE hCard,
              DWORD       dwControlCode,
              LPCVOID     pbSendBuffer,
              DWORD       cbSendLength,
              LPVOID      pbRecvBuffer,
              DWORD       cbRecvLength,
              LPDWORD     lpBytesReturned)
{
  ensure_initialized ();

  return backend.SCardControl (hCard,
                               dwControlCode,
                               pbSendBuffer,
                               cbSendLength,
                               pbRecvBuffer,
                               cbRecvLength,
                               lpBytesReturned);
}

PCSC_API LONG
SCardTransmit (SCARDHANDLE             hCard,
               const SCARD_IO_REQUEST *pioSendPci,
               LPCBYTE                 pbSendBuffer,
               DWORD                   cbSendLength,
               SCARD_IO_REQUEST       *pioRecvPci,
               LPBYTE                  pbRecvBuffer,
               LPDWORD                 pcbRecvLength)
{
  ensure_initialized ();

  return backend.SCardTransmit (hCard,
                                pioSendPci,
                                pbSendBuffer,
                                cbSendLength,
                                pioRecvPci,
                                pbRecvBuffer,
                                pcbRecvLength);
}

PCSC_API LONG
SCardListReaderGroups (SCARDCONTEXT hContext,
                       LPSTR        mszGroups,
                       LPDWORD      pcchGroups)
{
  ensure_initialized ();

  return backend.SCardListReaderGroups (hContext,
                                        mszGroups,
                                        pcchGroups);
}

PCSC_API LONG
SCardListReaders (SCARDCONTEXT hContext,
                  LPCSTR       mszGroups,
                  LPSTR        mszReaders,
                  LPDWORD      pcchReaders)
{
  ensure_initialized ();

  return backend.SCardListReaders (hContext,
                                   mszGroups,
                                   mszReaders,
                                   pcchReaders);
}

PCSC_API LONG
SCardFreeMemory (SCARDCONTEXT hContext,
                 LPCVOID      pvMem)
{
  ensure_initialized ();

  return backend.SCardFreeMemory (hContext,
                                  pvMem);
}

PCSC_API LONG
SCardCancel (SCARDCONTEXT hContext)
{
  ensure_initialized ();

  return backend.SCardCancel (hContext);
}

PCSC_API LONG
SCardGetAttrib (SCARDHANDLE hCard,
                DWORD       dwAttrId,
                LPBYTE      pbAttr,
                LPDWORD     pcbAttrLen)
{
  ensure_initialized ();

  return backend.SCardGetAttrib (hCard,
                                 dwAttrId,
                                 pbAttr,
                                 pcbAttrLen);
}

PCSC_API LONG
SCardSetAttrib (SCARDHANDLE hCard,
                DWORD       dwAttrId,
                LPCBYTE     pbAttr,
                DWORD       cbAttrLen)
{
  ensure_initialized ();

  return backend.SCardSetAttrib (hCard,
                                 dwAttrId,
                                 pbAttr,
                                 cbAttrLen);
}
