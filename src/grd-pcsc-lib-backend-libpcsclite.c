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

#include <dlfcn.h>

#define LIBPCSCLITE "libpcsclite.so.1"

#define LOAD_SYMBOL(backend, member, handle)             \
  do                                                     \
    {                                                    \
      (backend)->member = dlsym ((handle), #member);     \
      if (!(backend)->member)                            \
        {                                                \
          g_clear_pointer (&(handle), dlclose);          \
          return FALSE;                                  \
        }                                                \
    }                                                    \
  while (0)

static void *lib_handle;

gboolean
grd_pcsc_lib_backend_init_libpcsclite (GrdPcscLibBackend *backend)
{
  lib_handle = dlopen (LIBPCSCLITE, RTLD_LAZY);
  if (!lib_handle)
    return FALSE;

  LOAD_SYMBOL (backend, SCardEstablishContext, lib_handle);
  LOAD_SYMBOL (backend, SCardReleaseContext, lib_handle);
  LOAD_SYMBOL (backend, SCardIsValidContext, lib_handle);
  LOAD_SYMBOL (backend, SCardConnect, lib_handle);
  LOAD_SYMBOL (backend, SCardReconnect, lib_handle);
  LOAD_SYMBOL (backend, SCardDisconnect, lib_handle);
  LOAD_SYMBOL (backend, SCardBeginTransaction, lib_handle);
  LOAD_SYMBOL (backend, SCardEndTransaction, lib_handle);
  LOAD_SYMBOL (backend, SCardStatus, lib_handle);
  LOAD_SYMBOL (backend, SCardGetStatusChange, lib_handle);
  LOAD_SYMBOL (backend, SCardControl, lib_handle);
  LOAD_SYMBOL (backend, SCardTransmit, lib_handle);
  LOAD_SYMBOL (backend, SCardListReaderGroups, lib_handle);
  LOAD_SYMBOL (backend, SCardListReaders, lib_handle);
  LOAD_SYMBOL (backend, SCardFreeMemory, lib_handle);
  LOAD_SYMBOL (backend, SCardCancel, lib_handle);
  LOAD_SYMBOL (backend, SCardGetAttrib, lib_handle);
  LOAD_SYMBOL (backend, SCardSetAttrib, lib_handle);

  return TRUE;
}

void
grd_pcsc_lib_backend_cleanup_libpcsclite (void)
{
  g_clear_pointer (&lib_handle, dlclose);
}
