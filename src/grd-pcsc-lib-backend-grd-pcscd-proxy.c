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

#include <stdint.h>
#include <systemd/sd-login.h>

#include "grd-dbus-pcscd.h"

#define PCSCD_BUS_NAME "org.gnome.RemoteDesktop.Pcscd"

static GrdDBusPcscdSession *session_proxy;

static LONG
grd_pcscd_proxy_establish_context (DWORD          dwScope,
                                   LPCVOID        pvReserved1,
                                   LPCVOID        pvReserved2,
                                   LPSCARDCONTEXT phContext)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  int64_t context = 0;
  GVariant *call;

  call = g_variant_new ("(t)", (uint64_t) dwScope);

  if (!grd_dbus_pcscd_session_call_establish_context_sync (session_proxy,
                                                           call,
                                                           &ret,
                                                           NULL,
                                                           &error))
    {
      g_warning ("[PCSC] EstablishContext D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(xx)",
                 &return_value,
                 &context);

  if (phContext)
    *phContext = context;

  return return_value;
}

static LONG
grd_pcscd_proxy_release_context (SCARDCONTEXT hContext)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  GVariant *call;

  call = g_variant_new ("(x)", (int64_t) hContext);

  if (!grd_dbus_pcscd_session_call_release_context_sync (session_proxy,
                                                         call,
                                                         &ret,
                                                         NULL,
                                                         &error))
    {
      g_warning ("[PCSC] ReleaseContext D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x)", &return_value);

  return return_value;
}

static LONG
grd_pcscd_proxy_is_valid_context (SCARDCONTEXT hContext)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  GVariant *call;

  call = g_variant_new ("(x)", (int64_t) hContext);

  if (!grd_dbus_pcscd_session_call_is_valid_context_sync (session_proxy,
                                                          call,
                                                          &ret,
                                                          NULL,
                                                          &error))
    {
      g_warning ("[PCSC] IsValidContext D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x)", &return_value);

  return return_value;
}

static LONG
grd_pcscd_proxy_connect (SCARDCONTEXT  hContext,
                         LPCSTR        szReader,
                         DWORD         dwShareMode,
                         DWORD         dwPreferredProtocols,
                         LPSCARDHANDLE phCard,
                         LPDWORD       pdwActiveProtocol)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  int64_t out_context = 0;
  int64_t out_card = 0;
  uint64_t active_protocol = 0;
  GVariant *call;

  call = g_variant_new ("(xstt)",
                        (int64_t) hContext,
                        szReader,
                        (uint64_t) dwShareMode,
                        (uint64_t) dwPreferredProtocols);

  if (!grd_dbus_pcscd_session_call_connect_card_sync (session_proxy,
                                                      call,
                                                      &ret,
                                                      NULL,
                                                      &error))
    {
      g_warning ("[PCSC] Connect D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(xxxt)",
                 &return_value,
                 &out_context,
                 &out_card,
                 &active_protocol);

  if (phCard)
    *phCard = out_card;
  if (pdwActiveProtocol)
    *pdwActiveProtocol = active_protocol;

  return return_value;
}

static LONG
grd_pcscd_proxy_reconnect (SCARDHANDLE hCard,
                           DWORD       dwShareMode,
                           DWORD       dwPreferredProtocols,
                           DWORD       dwInitialization,
                           LPDWORD     pdwActiveProtocol)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  uint64_t active_protocol = 0;
  GVariant *call;

  call = g_variant_new ("(xttt)",
                        (int64_t) hCard,
                        (uint64_t) dwShareMode,
                        (uint64_t) dwPreferredProtocols,
                        (uint64_t) dwInitialization);

  if (!grd_dbus_pcscd_session_call_reconnect_sync (session_proxy,
                                                   call,
                                                   &ret,
                                                   NULL,
                                                   &error))
    {
      g_warning ("[PCSC] Reconnect D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(xt)",
                 &return_value,
                 &active_protocol);

  if (pdwActiveProtocol)
    *pdwActiveProtocol = active_protocol;

  return return_value;
}

static LONG
grd_pcscd_proxy_disconnect (SCARDHANDLE hCard,
                            DWORD       dwDisposition)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  GVariant *call;

  call = g_variant_new ("(xt)",
                        (int64_t) hCard,
                        (uint64_t) dwDisposition);

  if (!grd_dbus_pcscd_session_call_disconnect_card_sync (session_proxy,
                                                         call,
                                                         &ret,
                                                         NULL,
                                                         &error))
    {
      g_warning ("[PCSC] Disconnect D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x)", &return_value);

  return return_value;
}

static LONG
grd_pcscd_proxy_begin_transaction (SCARDHANDLE hCard)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  GVariant *call;

  call = g_variant_new ("(x)", (int64_t) hCard);

  if (!grd_dbus_pcscd_session_call_begin_transaction_sync (session_proxy,
                                                           call,
                                                           &ret,
                                                           NULL,
                                                           &error))
    {
      g_warning ("[PCSC] BeginTransaction D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x)", &return_value);

  return return_value;
}

static LONG
grd_pcscd_proxy_end_transaction (SCARDHANDLE hCard,
                                 DWORD       dwDisposition)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  GVariant *call;

  call = g_variant_new ("(xt)",
                        (int64_t) hCard,
                        (uint64_t) dwDisposition);

  if (!grd_dbus_pcscd_session_call_end_transaction_sync (session_proxy,
                                                         call,
                                                         &ret,
                                                         NULL,
                                                         &error))
    {
      g_warning ("[PCSC] EndTransaction D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x)", &return_value);

  return return_value;
}

static LONG
grd_pcscd_proxy_status (SCARDHANDLE hCard,
                        LPSTR       mszReaderName,
                        LPDWORD     pcchReaderLen,
                        LPDWORD     pdwState,
                        LPDWORD     pdwProtocol,
                        LPBYTE      pbAtr,
                        LPDWORD     pcbAtrLen)
{
  g_autoptr (GVariant) atr_variant = NULL;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  const char *reader_name = NULL;
  int64_t return_value = 0;
  uint64_t state = 0;
  uint64_t protocol = 0;
  const uint8_t *atr_data;
  size_t atr_size;
  size_t reader_len;
  GVariant *call;

  call = g_variant_new ("(xtt)",
                        (int64_t) hCard,
                        (uint64_t) (pcchReaderLen ? *pcchReaderLen : 0),
                        (uint64_t) (pcbAtrLen ? *pcbAtrLen : 0));

  if (!grd_dbus_pcscd_session_call_status_card_sync (session_proxy,
                                                     call,
                                                     &ret,
                                                     NULL,
                                                     &error))
    {
      g_warning ("[PCSC] Status D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x&stt@ay)",
                 &return_value,
                 &reader_name,
                 &state,
                 &protocol,
                 &atr_variant);

  if (pdwState)
    *pdwState = state;
  if (pdwProtocol)
    *pdwProtocol = protocol;

  reader_len = strlen (reader_name) + 1;
  atr_data = g_variant_get_fixed_array (atr_variant, &atr_size,
                                        sizeof (uint8_t));

  if (pcchReaderLen)
    {
      if (mszReaderName)
        {
          if (reader_len > *pcchReaderLen)
            return_value = SCARD_E_INSUFFICIENT_BUFFER;

          strncpy (mszReaderName, reader_name, *pcchReaderLen);
        }
      *pcchReaderLen = reader_len;
    }

  if (pcbAtrLen)
    {
      if (pbAtr)
        {
          if (atr_size > *pcbAtrLen)
            return_value = SCARD_E_INSUFFICIENT_BUFFER;

          memcpy (pbAtr, atr_data, MIN (atr_size, *pcbAtrLen));
        }
      *pcbAtrLen = atr_size;
    }

  return return_value;
}

static LONG
grd_pcscd_proxy_get_status_change (SCARDCONTEXT       hContext,
                                   DWORD              dwTimeout,
                                   SCARD_READERSTATE *rgReaderStates,
                                   DWORD              cReaders)
{
  g_autoptr (GVariant) reader_states_out = NULL;
  g_autoptr (GVariant) ret = NULL;
  GVariantBuilder *builder;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  uint64_t atr_event_state;
  uint64_t atr_length;
  GVariant *atr_variant;
  GVariantIter iter;
  unsigned int i;
  GVariant *call;

  builder = g_variant_builder_new (G_VARIANT_TYPE ("a(st)"));
  for (i = 0; i < cReaders; i++)
    {
      g_variant_builder_add (builder, "(st)",
                             rgReaderStates[i].szReader,
                             rgReaderStates[i].dwCurrentState);
    }

  call = g_variant_new ("(xt@a(st))",
                        (int64_t) hContext,
                        (uint64_t) dwTimeout,
                        g_variant_builder_end (builder));

  if (!grd_dbus_pcscd_session_call_get_status_change_sync (session_proxy,
                                                           call,
                                                           &ret,
                                                           NULL,
                                                           &error))
    {
      g_warning ("[PCSC] GetStatusChange D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x@a(ttay))",
                 &return_value,
                 &reader_states_out);

  i = 0;
  g_variant_iter_init (&iter, reader_states_out);
  while (g_variant_iter_loop (&iter, "(tt@ay)",
                              &atr_event_state,
                              &atr_length,
                              &atr_variant))
    {
      if (i < cReaders)
        {
          const unsigned char *atr_data;
          size_t atr_size;

          rgReaderStates[i].dwEventState = atr_event_state;
          rgReaderStates[i].cbAtr = atr_length;

          atr_data = g_variant_get_fixed_array (atr_variant,
                                                &atr_size,
                                                sizeof (unsigned char));
          if (atr_size > MAX_ATR_SIZE)
            atr_size = MAX_ATR_SIZE;
          memcpy (rgReaderStates[i].rgbAtr, atr_data, atr_size);
        }
      i++;
    }

  return return_value;
}

static LONG
grd_pcscd_proxy_control (SCARDHANDLE hCard,
                         DWORD       dwControlCode,
                         LPCVOID     pbSendBuffer,
                         DWORD       cbSendLength,
                         LPVOID      pbRecvBuffer,
                         DWORD       cbRecvLength,
                         LPDWORD     lpBytesReturned)
{
  g_autoptr (GVariant) recv_variant = NULL;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  const uint8_t *recv_data;
  size_t recv_size;
  GVariant *call;

  call = g_variant_new ("(xt@aytt)",
                        (int64_t) hCard,
                        (uint64_t) dwControlCode,
                        g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                   pbSendBuffer,
                                                   cbSendLength,
                                                   sizeof (uint8_t)),
                        (uint64_t) cbSendLength,
                        (uint64_t) cbRecvLength);

  if (!grd_dbus_pcscd_session_call_control_card_sync (session_proxy,
                                                      call,
                                                      &ret,
                                                      NULL,
                                                      &error))
    {
      g_warning ("[PCSC] Control D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x@ay)",
                 &return_value,
                 &recv_variant);

  recv_data = g_variant_get_fixed_array (recv_variant, &recv_size,
                                         sizeof (uint8_t));
  if (recv_size > cbRecvLength)
    return_value = SCARD_E_INSUFFICIENT_BUFFER;

  if (pbRecvBuffer)
    memcpy (pbRecvBuffer, recv_data, MIN (recv_size, cbRecvLength));
  if (lpBytesReturned)
    *lpBytesReturned = recv_size;

  return return_value;
}

static LONG
grd_pcscd_proxy_transmit (SCARDHANDLE             hCard,
                          const SCARD_IO_REQUEST *pioSendPci,
                          LPCBYTE                 pbSendBuffer,
                          DWORD                   cbSendLength,
                          SCARD_IO_REQUEST       *pioRecvPci,
                          LPBYTE                  pbRecvBuffer,
                          LPDWORD                 pcbRecvLength)
{
  g_autoptr (GVariant) recv_variant = NULL;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  uint64_t recv_pci_protocol = 0;
  const uint8_t *recv_data;
  size_t recv_size;
  size_t copy_size;
  GVariant *call;

  call = g_variant_new ("(xt@aytt)",
                        (int64_t) hCard,
                        (uint64_t) (pioSendPci ? pioSendPci->dwProtocol : 0),
                        g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                   pbSendBuffer,
                                                   cbSendLength,
                                                   sizeof (uint8_t)),
                        (uint64_t) (pioRecvPci ? pioRecvPci->dwProtocol : 0),
                        (uint64_t) (pcbRecvLength ? *pcbRecvLength : 0));

  if (!grd_dbus_pcscd_session_call_transmit_sync (session_proxy,
                                                  call,
                                                  &ret,
                                                  NULL,
                                                  &error))
    {
      g_warning ("[PCSC] Transmit D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(xt@ay)",
                 &return_value,
                 &recv_pci_protocol,
                 &recv_variant);

  recv_data = g_variant_get_fixed_array (recv_variant, &recv_size,
                                         sizeof (uint8_t));
  copy_size = recv_size;
  if (pcbRecvLength && recv_size > *pcbRecvLength)
    {
      copy_size = *pcbRecvLength;
      return_value = SCARD_E_INSUFFICIENT_BUFFER;
    }

  if (pbRecvBuffer)
    memcpy (pbRecvBuffer, recv_data, copy_size);
  if (pcbRecvLength)
    *pcbRecvLength = recv_size;

  if (pioRecvPci)
    {
      pioRecvPci->dwProtocol = recv_pci_protocol;
      pioRecvPci->cbPciLength = sizeof (SCARD_IO_REQUEST);
    }

  return return_value;
}

static LONG
grd_pcscd_proxy_list_reader_groups (SCARDCONTEXT hContext,
                                    LPSTR        mszGroups,
                                    LPDWORD      pcchGroups)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) groups = NULL;
  int64_t return_value = 0;
  size_t groups_size = 0;
  unsigned int i;
  GVariant *call;

  call = g_variant_new ("(x)", (int64_t) hContext);

  if (!grd_dbus_pcscd_session_call_list_reader_groups_sync (session_proxy,
                                                            call,
                                                            &ret,
                                                            NULL,
                                                            &error))
    {
      g_warning ("[PCSC] ListReaderGroups D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x^as)",
                 &return_value,
                 &groups);

  if (return_value != SCARD_S_SUCCESS)
    return return_value;

  for (i = 0; groups[i]; i++)
    groups_size += strlen (groups[i]) + 1;
  groups_size++;

  if (!mszGroups)
    {
      if (pcchGroups)
        *pcchGroups = groups_size;
      return SCARD_S_SUCCESS;
    }

  if (pcchGroups && *pcchGroups < groups_size)
    {
      *pcchGroups = groups_size;
      return SCARD_E_INSUFFICIENT_BUFFER;
    }

  if (pcchGroups)
    *pcchGroups = groups_size;
  for (i = 0; groups[i]; i++)
    {
      size_t len = strlen (groups[i]) + 1;
      memcpy (mszGroups, groups[i], len);
      mszGroups += len;
    }
  *mszGroups = '\0';

  return SCARD_S_SUCCESS;
}

static LONG
grd_pcscd_proxy_list_readers (SCARDCONTEXT hContext,
                              LPCSTR       mszGroups,
                              LPSTR        mszReaders,
                              LPDWORD      pcchReaders)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) readers = NULL;
  int64_t return_value = 0;
  size_t readers_size = 0;
  unsigned int i;
  GVariant *call;

  call = g_variant_new ("(x)", (int64_t) hContext);

  if (!grd_dbus_pcscd_session_call_list_readers_sync (session_proxy,
                                                      call,
                                                      &ret,
                                                      NULL,
                                                      &error))
    {
      g_warning ("[PCSC] ListReaders D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x^as)",
                 &return_value,
                 &readers);

  if (return_value != SCARD_S_SUCCESS)
    return return_value;

  for (i = 0; readers[i]; i++)
    readers_size += strlen (readers[i]) + 1;
  readers_size++;

  if (!mszReaders)
    {
      if (pcchReaders)
        *pcchReaders = readers_size;
      return SCARD_S_SUCCESS;
    }

  if (pcchReaders && *pcchReaders < readers_size)
    {
      *pcchReaders = readers_size;
      return SCARD_E_INSUFFICIENT_BUFFER;
    }

  if (pcchReaders)
    *pcchReaders = readers_size;
  for (i = 0; readers[i]; i++)
    {
      size_t len = strlen (readers[i]) + 1;
      memcpy (mszReaders, readers[i], len);
      mszReaders += len;
    }
  *mszReaders = '\0';

  return SCARD_S_SUCCESS;
}

static LONG
grd_pcscd_proxy_free_memory (SCARDCONTEXT hContext,
                             LPCVOID      pvMem)
{
  return SCARD_S_SUCCESS;
}

static LONG
grd_pcscd_proxy_cancel (SCARDCONTEXT hContext)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  GVariant *call;

  call = g_variant_new ("(x)", (int64_t) hContext);

  if (!grd_dbus_pcscd_session_call_cancel_sync (session_proxy,
                                                call,
                                                &ret,
                                                NULL,
                                                &error))
    {
      g_warning ("[PCSC] Cancel D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x)", &return_value);

  return return_value;
}

static LONG
grd_pcscd_proxy_get_attrib (SCARDHANDLE hCard,
                            DWORD       dwAttrId,
                            LPBYTE      pbAttr,
                            LPDWORD     pcbAttrLen)
{
  g_autoptr (GVariant) attr_variant = NULL;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  const uint8_t *attr_data;
  size_t attr_size;
  size_t copy_size;
  GVariant *call;

  call = g_variant_new ("(xtt)",
                        (int64_t) hCard,
                        (uint64_t) dwAttrId,
                        (uint64_t) (pcbAttrLen ? *pcbAttrLen : 0));

  if (!grd_dbus_pcscd_session_call_get_attrib_sync (session_proxy,
                                                    call,
                                                    &ret,
                                                    NULL,
                                                    &error))
    {
      g_warning ("[PCSC] GetAttrib D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x@ay)",
                 &return_value,
                 &attr_variant);

  attr_data = g_variant_get_fixed_array (attr_variant, &attr_size,
                                         sizeof (uint8_t));

  if (!pbAttr)
    {
      if (pcbAttrLen)
        *pcbAttrLen = attr_size;
      return return_value;
    }

  copy_size = attr_size;
  if (pcbAttrLen && attr_size > *pcbAttrLen)
    {
      copy_size = *pcbAttrLen;
      return_value = SCARD_E_INSUFFICIENT_BUFFER;
    }

  memcpy (pbAttr, attr_data, copy_size);
  if (pcbAttrLen)
    *pcbAttrLen = attr_size;

  return return_value;
}

static LONG
grd_pcscd_proxy_set_attrib (SCARDHANDLE hCard,
                            DWORD       dwAttrId,
                            LPCBYTE     pbAttr,
                            DWORD       cbAttrLen)
{
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;
  int64_t return_value = 0;
  GVariant *call;

  call = g_variant_new ("(xt@ay)",
                        (int64_t) hCard,
                        (uint64_t) dwAttrId,
                        g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                   pbAttr,
                                                   cbAttrLen,
                                                   sizeof (uint8_t)));

  if (!grd_dbus_pcscd_session_call_set_attrib_sync (session_proxy,
                                                    call,
                                                    &ret,
                                                    NULL,
                                                    &error))
    {
      g_warning ("[PCSC] SetAttrib D-Bus call failed: %s",
                 error->message);
      return SCARD_F_INTERNAL_ERROR;
    }

  g_variant_get (ret, "(x)", &return_value);

  return return_value;
}

static char *
get_session_id_from_uid (uid_t uid)
{
  const char * const graphical_types[] = { "wayland", "x11", NULL };
  const char * const active_states[] = { "active", "online", NULL };
  g_auto (GStrv) sessions = NULL;
  int n_sessions;
  int i;

  n_sessions = sd_uid_get_sessions (uid, 0, &sessions);

  for (i = n_sessions - 1; i >= 0; i--)
    {
      g_autofree char *type = NULL;
      g_autofree char *state = NULL;

      if (sd_session_get_type (sessions[i], &type) < 0)
        continue;

      if (!g_strv_contains (graphical_types, type))
        continue;

      if (sd_session_get_state (sessions[i], &state) < 0)
        continue;

      if (!g_strv_contains (active_states, state))
        continue;

      return g_strdup (sessions[i]);
    }

  return NULL;
}

static char *
get_session_id (void)
{
  const char *session_id;
  char *logind_session_id = NULL;

  session_id = g_getenv ("GDM_AUTH_SESSION_ID");
  if (session_id && g_strcmp0 (session_id, "") != 0)
    return g_strdup (session_id);

  session_id = g_getenv ("XDG_SESSION_ID");
  if (session_id && session_id[0] != '\0')
    return g_strdup (session_id);

  if (sd_pid_get_session (0, &logind_session_id) >= 0)
    return logind_session_id;

  return get_session_id_from_uid (getuid ());
}

static gboolean
dbus_interface_exists (GDBusProxy *proxy)
{
  g_autoptr (GVariant) result = NULL;

  result = g_dbus_connection_call_sync (g_dbus_proxy_get_connection (proxy),
                                        g_dbus_proxy_get_name (proxy),
                                        g_dbus_proxy_get_object_path (proxy),
                                        "org.freedesktop.DBus.Properties",
                                        "GetAll",
                                        g_variant_new ("(s)",
                                          g_dbus_proxy_get_interface_name (proxy)),
                                        NULL,
                                        G_DBUS_CALL_FLAGS_NONE,
                                        -1,
                                        NULL,
                                        NULL);
  return result != NULL;
}

static void
set_backend_functions (GrdPcscLibBackend *backend)
{
  backend->SCardEstablishContext = grd_pcscd_proxy_establish_context;
  backend->SCardReleaseContext = grd_pcscd_proxy_release_context;
  backend->SCardIsValidContext = grd_pcscd_proxy_is_valid_context;
  backend->SCardConnect = grd_pcscd_proxy_connect;
  backend->SCardReconnect = grd_pcscd_proxy_reconnect;
  backend->SCardDisconnect = grd_pcscd_proxy_disconnect;
  backend->SCardBeginTransaction = grd_pcscd_proxy_begin_transaction;
  backend->SCardEndTransaction = grd_pcscd_proxy_end_transaction;
  backend->SCardStatus = grd_pcscd_proxy_status;
  backend->SCardGetStatusChange = grd_pcscd_proxy_get_status_change;
  backend->SCardControl = grd_pcscd_proxy_control;
  backend->SCardTransmit = grd_pcscd_proxy_transmit;
  backend->SCardListReaderGroups = grd_pcscd_proxy_list_reader_groups;
  backend->SCardListReaders = grd_pcscd_proxy_list_readers;
  backend->SCardFreeMemory = grd_pcscd_proxy_free_memory;
  backend->SCardCancel = grd_pcscd_proxy_cancel;
  backend->SCardGetAttrib = grd_pcscd_proxy_get_attrib;
  backend->SCardSetAttrib = grd_pcscd_proxy_set_attrib;
}

static gboolean
try_bus_type (GBusType    bus_type,
              const char *object_path)
{
  g_autofree char *name_owner = NULL;

  session_proxy = grd_dbus_pcscd_session_proxy_new_for_bus_sync (
                    bus_type,
                    G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                    PCSCD_BUS_NAME,
                    object_path,
                    NULL,
                    NULL);
  if (!session_proxy)
    return FALSE;

  name_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (session_proxy));
  if (!name_owner)
    {
      g_clear_object (&session_proxy);
      return FALSE;
    }

  if (!dbus_interface_exists (G_DBUS_PROXY (session_proxy)))
    {
      g_clear_object (&session_proxy);
      return FALSE;
    }

  return TRUE;
}

gboolean
grd_pcsc_lib_backend_init_grd_pcscd_proxy (GrdPcscLibBackend *backend)
{
  g_autofree char *session_id = NULL;
  g_autofree char *object_path = NULL;

  session_id = get_session_id ();
  if (!session_id)
    return FALSE;

  object_path = g_strdup_printf ("/org/gnome/RemoteDesktop/Pcscd/%s", session_id);

  if (try_bus_type (G_BUS_TYPE_SESSION, object_path) ||
      try_bus_type (G_BUS_TYPE_SYSTEM, object_path))
    {
      set_backend_functions (backend);
      return TRUE;
    }

  return FALSE;
}

void
grd_pcsc_lib_backend_cleanup_grd_pcscd_proxy (void)
{
  g_clear_object (&session_proxy);
}
