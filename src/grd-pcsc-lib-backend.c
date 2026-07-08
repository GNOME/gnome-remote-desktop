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

#define SET_DEFAULT(backend, member, func) \
  (backend)->member = (typeof ((backend)->member)) (func)

static LONG
internal_error (void)
{
  return SCARD_F_INTERNAL_ERROR;
}

void
grd_pcsc_lib_backend_set_default (GrdPcscLibBackend *backend)
{
  SET_DEFAULT (backend, SCardEstablishContext, internal_error);
  SET_DEFAULT (backend, SCardReleaseContext, internal_error);
  SET_DEFAULT (backend, SCardIsValidContext, internal_error);
  SET_DEFAULT (backend, SCardConnect, internal_error);
  SET_DEFAULT (backend, SCardReconnect, internal_error);
  SET_DEFAULT (backend, SCardDisconnect, internal_error);
  SET_DEFAULT (backend, SCardBeginTransaction, internal_error);
  SET_DEFAULT (backend, SCardEndTransaction, internal_error);
  SET_DEFAULT (backend, SCardStatus, internal_error);
  SET_DEFAULT (backend, SCardGetStatusChange, internal_error);
  SET_DEFAULT (backend, SCardControl, internal_error);
  SET_DEFAULT (backend, SCardTransmit, internal_error);
  SET_DEFAULT (backend, SCardListReaderGroups, internal_error);
  SET_DEFAULT (backend, SCardListReaders, internal_error);
  SET_DEFAULT (backend, SCardFreeMemory, internal_error);
  SET_DEFAULT (backend, SCardCancel, internal_error);
  SET_DEFAULT (backend, SCardGetAttrib, internal_error);
  SET_DEFAULT (backend, SCardSetAttrib, internal_error);
}
