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

#include "grd-rdp-smartcard.h"

#include <freerdp/utils/smartcard_pack.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib/gstdio.h>

#include "grd-context.h"
#include "grd-daemon-utils.h"
#include "grd-dbus-pcscd.h"
#include "grd-private.h"
#include "grd-rdp-device-redirection.h"
#include "grd-session-rdp.h"

#define EXTEND_32_TO_64(val) \
  ((uint64_t) (uint32_t) (val))

#define NARROW_64_TO_32(val) \
  ((uint64_t) (val) > UINT32_MAX \
    ? (g_warning ("Narrowing 64-to-32 lost data in '"#val"': 0x%lx", \
                  (uint64_t) (val)), \
       (uint32_t) (val)) \
    : (uint32_t) (val))

/*
 * pcsc-lite dwState bitmask values. These differ from the WinSCard
 * sequential values (0-6) defined by FreeRDP/WinPR in winpr/smartcard.h.
 */
#define PCSC_SCARD_UNKNOWN    0x0001
#define PCSC_SCARD_ABSENT     0x0002
#define PCSC_SCARD_PRESENT    0x0004
#define PCSC_SCARD_SWALLOWED  0x0008
#define PCSC_SCARD_POWERED    0x0010
#define PCSC_SCARD_NEGOTIABLE 0x0020

/*
 * pcsc-lite and WinSCard use different values for SCARD_PROTOCOL_RAW.
 * SCARD_PROTOCOL_T15 is not in WinSCard.
 * T=0 (0x01) and T=1 (0x02) are the same in both.
 */
#define PCSC_SCARD_PROTOCOL_RAW 0x0004
#define PCSC_SCARD_PROTOCOL_T15 0x0008

typedef struct
{
  GrdRdpSmartcard *smartcard;
  int fd;
} PcscdConnectData;

struct _GrdRdpSmartcard
{
  GObject parent;

  GMutex shutdown_mutex;
  gboolean in_shutdown;

  RdpdrServerContext *rdpdr_context;
  GHashTable *smartcard_device_ids;
  GHashTable *pending_invocations;

  /* System bus proxy to grd-pcscd */
  GCancellable *pcscd_proxy_cancellable;
  unsigned long name_owner_changed_id;
  GrdDBusPcscd *pcscd_proxy;

  /* Private peer-to-peer proxy for system-level clients (via grd-pcscd) */
  GCancellable *private_proxy_cancellable;
  GrdDBusPcscdSession *private_proxy;

  /* Session proxy for session-level clients (direct access) */
  GrdDBusPcscdSession *session_proxy;
  unsigned int session_bus_name_id;

  GHashTable *card_to_context;
};

G_DEFINE_TYPE (GrdRdpSmartcard, grd_rdp_smartcard, G_TYPE_OBJECT)

static uint32_t
convert_card_state_to_pcsc (uint32_t card_reader_state)
{
  switch (card_reader_state)
    {
    case SCARD_UNKNOWN:
      return PCSC_SCARD_UNKNOWN;
    case SCARD_ABSENT:
      return PCSC_SCARD_ABSENT;
    case SCARD_PRESENT:
      return PCSC_SCARD_PRESENT;
    case SCARD_SWALLOWED:
      return PCSC_SCARD_PRESENT | PCSC_SCARD_SWALLOWED;
    case SCARD_POWERED:
      return PCSC_SCARD_PRESENT | PCSC_SCARD_POWERED;
    case SCARD_NEGOTIABLE:
    case SCARD_SPECIFIC:
      return PCSC_SCARD_PRESENT | PCSC_SCARD_POWERED | PCSC_SCARD_NEGOTIABLE;
    default:
      g_warning ("[RDP.SMARTCARD] Unknown CardReaderState: %u", card_reader_state);
      return PCSC_SCARD_UNKNOWN;
    }
}

static uint32_t
convert_protocol_to_pcsc (uint32_t dwProtocols)
{
  if (dwProtocols & SCARD_PROTOCOL_RAW)
    {
      dwProtocols &= ~SCARD_PROTOCOL_RAW;
      dwProtocols |= PCSC_SCARD_PROTOCOL_RAW;
    }

  if (dwProtocols & SCARD_PROTOCOL_DEFAULT)
    {
      dwProtocols &= ~SCARD_PROTOCOL_DEFAULT;
    }

  if (dwProtocols == SCARD_PROTOCOL_UNDEFINED)
    {
      dwProtocols = SCARD_PROTOCOL_Tx;
    }

  return dwProtocols;
}

static uint32_t
convert_protocol_to_winscard (uint32_t dwProtocols)
{
  if (dwProtocols & PCSC_SCARD_PROTOCOL_RAW)
    {
      dwProtocols &= ~PCSC_SCARD_PROTOCOL_RAW;
      dwProtocols |= SCARD_PROTOCOL_RAW;
    }

  if (dwProtocols & PCSC_SCARD_PROTOCOL_T15)
  {
    dwProtocols &= ~PCSC_SCARD_PROTOCOL_T15;
  }

  return dwProtocols;
}

static gboolean
is_device_unavailable (GrdRdpSmartcard       *smartcard,
                       GDBusMethodInvocation *invocation)
{
  g_mutex_lock (&smartcard->shutdown_mutex);
  if (!smartcard->in_shutdown &&
      g_hash_table_size (smartcard->smartcard_device_ids) > 0)
    {
      g_mutex_unlock (&smartcard->shutdown_mutex);
      return FALSE;
    }
  g_mutex_unlock (&smartcard->shutdown_mutex);

  g_dbus_method_invocation_return_error (invocation,
                                         G_DBUS_ERROR,
                                         G_DBUS_ERROR_FAILED,
                                         "Smartcard device not available");
  return TRUE;
}

static void
remove_pending_invocation (RdpdrServerContext    *rdpdr_context,
                           GDBusMethodInvocation *invocation)
{
  GrdRdpDeviceRedirection *device_redirection = rdpdr_context->data;
  GrdRdpSmartcard *smartcard;

  smartcard = grd_rdp_device_redirection_get_smartcard (device_redirection);
  if (!smartcard)
    return;
 
  g_hash_table_remove (smartcard->pending_invocations, invocation);
}

static void
on_smartcard_establish_context_complete (RdpdrServerContext            *rdpdr_context,
                                         void                          *callback_data,
                                         uint32_t                       io_status,
                                         int32_t                        return_code,
                                         const EstablishContext_Return *ret)
{
  GDBusMethodInvocation *invocation = callback_data;
  int64_t context = 0;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] EstablishContext complete: return_code=%i",
           return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  if (ret)
    context = (int64_t) smartcard_scard_context_native_from_redir (&ret->hContext);

  ret_variant = g_variant_new ("((xx))",
                               (int64_t) EXTEND_32_TO_64 (return_code),
                               context);
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_establish_context (GrdDBusPcscdSession   *skeleton,
                             GDBusMethodInvocation *invocation,
                             GVariant              *call_variant,
                             gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  uint64_t scope;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] EstablishContext");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  g_variant_get (call_variant, "(t)", &scope);
  status = rdpdr_context->SmartcardEstablishContext (rdpdr_context,
                                                     invocation,
                                                     NARROW_64_TO_32 (scope),
                                                     &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardEstablishContext failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_release_context_complete (RdpdrServerContext *rdpdr_context,
                                       void               *callback_data,
                                       uint32_t            io_status,
                                       int32_t             return_code)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] ReleaseContext complete: return_code=%i",
           return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  ret_variant = g_variant_new ("((x))",
                               (int64_t) EXTEND_32_TO_64 (return_code));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
remove_cards_for_context (gpointer key,
                          gpointer value,
                          gpointer user_data)
{
  int64_t card_context = *(int64_t *) value;
  int64_t context = *(int64_t *) user_data;

  return card_context == context;
}

static gboolean
on_handle_release_context (GrdDBusPcscdSession   *skeleton,
                           GDBusMethodInvocation *invocation,
                           GVariant              *call_variant,
                           gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  REDIR_SCARDCONTEXT redir_context = {};
  int64_t context;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] ReleaseContext");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  g_variant_get (call_variant, "(x)", &context);

  g_hash_table_foreach_remove (smartcard->card_to_context,
                               remove_cards_for_context,
                               &context);

  smartcard_scard_context_native_to_redir (&redir_context, context);

  status = rdpdr_context->SmartcardReleaseContext (rdpdr_context,
                                                   invocation,
                                                   &redir_context,
                                                   &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardReleaseContext failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_is_valid_context_complete (RdpdrServerContext *rdpdr_context,
                                        void               *callback_data,
                                        uint32_t            io_status,
                                        int32_t             return_code)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] IsValidContext complete: return_code=%i",
           return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  ret_variant = g_variant_new ("((x))",
                               (int64_t) EXTEND_32_TO_64 (return_code));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_is_valid_context (GrdDBusPcscdSession   *skeleton,
                            GDBusMethodInvocation *invocation,
                            GVariant              *call_variant,
                            gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  REDIR_SCARDCONTEXT redir_context = {};
  int64_t context;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] IsValidContext");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  g_variant_get (call_variant, "(x)", &context);
  smartcard_scard_context_native_to_redir (&redir_context, context);

  status = rdpdr_context->SmartcardIsValidContext (rdpdr_context,
                                                   invocation,
                                                   &redir_context,
                                                   &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardIsValidContext failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
track_card_context (RdpdrServerContext *rdpdr_context,
                    int64_t             card,
                    int64_t             context)
{
  GrdRdpDeviceRedirection *device_redirection = rdpdr_context->data;
  GrdRdpSmartcard *smartcard;

  smartcard = grd_rdp_device_redirection_get_smartcard (device_redirection);
  if (smartcard)
    {
      g_hash_table_insert (smartcard->card_to_context,
                           g_memdup2 (&card, sizeof card),
                           g_memdup2 (&context, sizeof context));
    }
}

static void
untrack_card_context (GrdRdpSmartcard *smartcard,
                      int64_t          card)
{
  g_hash_table_remove (smartcard->card_to_context, &card);
}

static void
on_smartcard_connect_complete (RdpdrServerContext  *rdpdr_context,
                               void                *callback_data,
                               uint32_t             io_status,
                               int32_t              return_code,
                               const Connect_Return *ret)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariant *ret_variant;
  int64_t out_context = 0;
  int64_t card = 0;
  uint64_t active_protocol = 0;

  g_debug ("[RDP.SMARTCARD] Connect complete: return_code=%i", return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  if (ret)
    {
      out_context =
        (int64_t) smartcard_scard_context_native_from_redir (&ret->hContext);
      card =
        (int64_t) smartcard_scard_handle_native_from_redir (&ret->hCard);
      active_protocol =
        EXTEND_32_TO_64 (convert_protocol_to_pcsc (ret->dwActiveProtocol));

      if (card != 0)
        track_card_context (rdpdr_context, card, out_context);
    }

  ret_variant = g_variant_new ("((xxxt))",
                               (int64_t) EXTEND_32_TO_64 (return_code),
                               out_context, card, active_protocol);
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_connect_card (GrdDBusPcscdSession   *skeleton,
                        GDBusMethodInvocation *invocation,
                        GVariant              *variant_call,
                        gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  ConnectA_Call call = {};
  int64_t context;
  const char *reader;
  uint64_t share_mode;
  uint64_t preferred_protocols;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] Connect");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  g_variant_get (variant_call, "(x&stt)",
                 &context,
                 &reader,
                 &share_mode,
                 &preferred_protocols);

  smartcard_scard_context_native_to_redir (&call.Common.handles.hContext,
                                           context);
  call.Common.dwShareMode = NARROW_64_TO_32 (share_mode);
  call.szReader = (char *) reader;
  call.Common.dwPreferredProtocols =
    convert_protocol_to_winscard (NARROW_64_TO_32 (preferred_protocols));

  status = rdpdr_context->SmartcardConnectA (rdpdr_context,
                                             invocation,
                                             &call,
                                             &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardConnectA failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_reconnect_complete (RdpdrServerContext      *rdpdr_context,
                                 void                    *callback_data,
                                 uint32_t                 io_status,
                                 int32_t                  return_code,
                                 const Reconnect_Return  *ret)
{
  GDBusMethodInvocation *invocation = callback_data;
  uint64_t active_protocol = 0;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] Reconnect complete: return_code=%i", return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  if (ret)
    {
      active_protocol =
        EXTEND_32_TO_64 (convert_protocol_to_pcsc (ret->dwActiveProtocol));
    }

  ret_variant = g_variant_new ("((xt))",
                               (int64_t) EXTEND_32_TO_64 (return_code),
                               active_protocol);
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
resolve_context_for_card (GrdRdpSmartcard       *smartcard,
                          GDBusMethodInvocation *invocation,
                          int64_t                card,
                          int64_t               *out_context)
{
  gpointer value;

  if (!g_hash_table_lookup_extended (smartcard->card_to_context,
                                     &card,
                                     NULL,
                                     &value))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "No context found for card handle");
      return FALSE;
    }

  *out_context = *(int64_t *) value;
  return TRUE;
}

static gboolean
on_handle_reconnect (GrdDBusPcscdSession   *skeleton,
                     GDBusMethodInvocation *invocation,
                     GVariant              *call_variant,
                     gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  Reconnect_Call call = {};
  int64_t card;
  int64_t context;
  uint64_t share_mode;
  uint64_t preferred_protocols;
  uint64_t initialization;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] Reconnect");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_variant_get (call_variant, "(xttt)",
                 &card,
                 &share_mode,
                 &preferred_protocols,
                 &initialization);

  if (!resolve_context_for_card (smartcard, invocation, card, &context))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  smartcard_scard_handle_native_to_redir (&call.handles.hCard, card);
  call.dwShareMode = NARROW_64_TO_32 (share_mode);
  call.dwPreferredProtocols =
    convert_protocol_to_winscard (NARROW_64_TO_32 (preferred_protocols));
  call.dwInitialization = NARROW_64_TO_32 (initialization);

  status = rdpdr_context->SmartcardReconnect (rdpdr_context,
                                              invocation,
                                              &call,
                                              &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardReconnect failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_disconnect_complete (RdpdrServerContext *rdpdr_context,
                                  void               *callback_data,
                                  uint32_t            io_status,
                                  int32_t             return_code)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] Disconnect complete: return_code=%i", return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  ret_variant = g_variant_new ("((x))",
                               (int64_t) EXTEND_32_TO_64 (return_code));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_disconnect_card (GrdDBusPcscdSession   *skeleton,
                           GDBusMethodInvocation *invocation,
                           GVariant              *call_variant,
                           gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  HCardAndDisposition_Call call = {};
  int64_t card;
  int64_t context;
  uint64_t disposition;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] Disconnect");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_variant_get (call_variant, "(xt)", &card, &disposition);

  if (!resolve_context_for_card (smartcard, invocation, card, &context))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  untrack_card_context (smartcard, card);

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  smartcard_scard_handle_native_to_redir (&call.handles.hCard, card);
  call.dwDisposition = NARROW_64_TO_32 (disposition);

  status = rdpdr_context->SmartcardDisconnect (rdpdr_context,
                                               invocation,
                                               &call,
                                               &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardDisconnect failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_begin_transaction_complete (RdpdrServerContext *rdpdr_context,
                                         void               *callback_data,
                                         uint32_t            io_status,
                                         int32_t             return_code)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] BeginTransaction complete: return_code=%i",
           return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  ret_variant = g_variant_new ("((x))",
                               (int64_t) EXTEND_32_TO_64 (return_code));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_begin_transaction (GrdDBusPcscdSession   *skeleton,
                             GDBusMethodInvocation *invocation,
                             GVariant              *call_variant,
                             gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  HCardAndDisposition_Call call = {};
  int64_t card;
  int64_t context;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] BeginTransaction");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_variant_get (call_variant, "(x)", &card);

  if (!resolve_context_for_card (smartcard, invocation, card, &context))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  smartcard_scard_handle_native_to_redir (&call.handles.hCard, card);

  status = rdpdr_context->SmartcardBeginTransaction (rdpdr_context,
                                                     invocation,
                                                     &call,
                                                     &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardBeginTransaction failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_end_transaction_complete (RdpdrServerContext *rdpdr_context,
                                       void               *callback_data,
                                       uint32_t            io_status,
                                       int32_t             return_code)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] EndTransaction complete: return_code=%i",
           return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  ret_variant = g_variant_new ("((x))",
                               (int64_t) EXTEND_32_TO_64 (return_code));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_end_transaction (GrdDBusPcscdSession   *skeleton,
                           GDBusMethodInvocation *invocation,
                           GVariant              *call_variant,
                           gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  HCardAndDisposition_Call call = {};
  int64_t card;
  int64_t context;
  uint64_t disposition;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] EndTransaction");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_variant_get (call_variant, "(xt)", &card, &disposition);

  if (!resolve_context_for_card (smartcard, invocation, card, &context))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  smartcard_scard_handle_native_to_redir (&call.handles.hCard, card);
  call.dwDisposition = NARROW_64_TO_32 (disposition);

  status = rdpdr_context->SmartcardEndTransaction (rdpdr_context,
                                                   invocation,
                                                   &call,
                                                   &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardEndTransaction failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_status_complete (RdpdrServerContext   *rdpdr_context,
                              void                 *callback_data,
                              uint32_t              io_status,
                              int32_t               return_code,
                              const Status_Return  *ret)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariant *ret_variant;
  const char *reader_name = "";
  uint64_t state = 0;
  uint64_t protocol = 0;
  const uint8_t *atr = NULL;
  size_t atr_len = 0;

  g_debug ("[RDP.SMARTCARD] Status complete: return_code=%i", return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  if (ret)
    {
      if (ret->mszReaderNames)
        reader_name = (const char *) ret->mszReaderNames;
      state = EXTEND_32_TO_64 (convert_card_state_to_pcsc (ret->dwState));
      protocol = EXTEND_32_TO_64 (convert_protocol_to_pcsc (ret->dwProtocol));
      atr = ret->pbAtr;
      atr_len = ret->cbAtrLen;
    }

  ret_variant = g_variant_new ("((xstt@ay))",
                               (int64_t) EXTEND_32_TO_64 (return_code),
                               reader_name, state, protocol,
                               g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                          atr, atr_len,
                                                          sizeof (uint8_t)));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_status_card (GrdDBusPcscdSession   *skeleton,
                       GDBusMethodInvocation *invocation,
                       GVariant              *call_variant,
                       gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  Status_Call call = {};
  int64_t card;
  int64_t context;
  uint64_t reader_len;
  uint64_t atr_len;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] Status");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_variant_get (call_variant, "(xtt)", &card, &reader_len, &atr_len);

  if (!resolve_context_for_card (smartcard, invocation, card, &context))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  smartcard_scard_handle_native_to_redir (&call.handles.hCard, card);
  call.cchReaderLen = NARROW_64_TO_32 (reader_len);
  call.cbAtrLen = NARROW_64_TO_32 (atr_len);

  status = rdpdr_context->SmartcardStatusA (rdpdr_context,
                                            invocation,
                                            &call,
                                            &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardStatusA failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_get_status_change_complete (RdpdrServerContext           *rdpdr_context,
                                         void                         *callback_data,
                                         uint32_t                      io_status,
                                         int32_t                       return_code,
                                         const GetStatusChange_Return *ret)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariantBuilder *builder;
  GVariant *ret_variant;
  int i;

  g_debug ("[RDP.SMARTCARD] GetStatusChange complete: return_code=%i",
           return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  builder = g_variant_builder_new (G_VARIANT_TYPE ("a(ttay)"));

  if (ret)
    {
      for (i = 0; i < ret->cReaders; i++)
        {
          g_variant_builder_add (builder, "(tt@ay)",
                                 EXTEND_32_TO_64 (ret->rgReaderStates[i].dwEventState),
                                 EXTEND_32_TO_64 (ret->rgReaderStates[i].cbAtr),
                                 g_variant_new_fixed_array (
                                   G_VARIANT_TYPE_BYTE,
                                   ret->rgReaderStates[i].rgbAtr,
                                   ret->rgReaderStates[i].cbAtr,
                                   sizeof (uint8_t)));
        }
    }

  ret_variant = g_variant_new ("((x@a(ttay)))",
                               (int64_t) EXTEND_32_TO_64 (return_code),
                               g_variant_builder_end (builder));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_get_status_change (GrdDBusPcscdSession   *skeleton,
                             GDBusMethodInvocation *invocation,
                             GVariant              *call_variant,
                             gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  g_autoptr (GVariant) reader_states = NULL;
  g_autofree SCARD_READERSTATEA *reader_states_a = NULL;
  GetStatusChangeA_Call call = {};
  int64_t context;
  uint64_t timeout;
  char *reader_name;
  uint64_t current_state;
  uint32_t completion_id;
  GVariantIter iter;
  size_t n_readers;
  uint32_t status;
  int i;

  g_debug ("[RDP.SMARTCARD] GetStatusChange");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  g_variant_get (call_variant, "(xt@a(st))",
                 &context,
                 &timeout,
                 &reader_states);

  n_readers = g_variant_n_children (reader_states);
  reader_states_a = g_new0 (SCARD_READERSTATEA, n_readers);

  i = 0;
  g_variant_iter_init (&iter, reader_states);
  while (g_variant_iter_next (&iter, "(&st)", &reader_name, &current_state))
    {
      reader_states_a[i].szReader = reader_name;
      reader_states_a[i].dwCurrentState = NARROW_64_TO_32 (current_state);
      i++;
    }

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  call.dwTimeOut = NARROW_64_TO_32 (timeout);
  call.cReaders = NARROW_64_TO_32 (n_readers);
  call.rgReaderStates = reader_states_a;

  status = rdpdr_context->SmartcardGetStatusChangeA (rdpdr_context,
                                                     invocation,
                                                     &call,
                                                     &completion_id);

  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardGetStatusChange failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_control_complete (RdpdrServerContext   *rdpdr_context,
                               void                 *callback_data,
                               uint32_t              io_status,
                               int32_t               return_code,
                               const Control_Return *ret)
{
  GDBusMethodInvocation *invocation = callback_data;
  const uint8_t *out_buffer = NULL;
  size_t out_buffer_size = 0;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] Control complete: return_code=%i", return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  if (ret)
    {
      out_buffer = ret->pvOutBuffer;
      out_buffer_size = ret->cbOutBufferSize;
    }

  ret_variant = g_variant_new ("((x@ay))",
                               (int64_t) EXTEND_32_TO_64 (return_code),
                               g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                          out_buffer,
                                                          out_buffer_size,
                                                          sizeof (uint8_t)));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_control_card (GrdDBusPcscdSession   *skeleton,
                        GDBusMethodInvocation *invocation,
                        GVariant              *call_variant,
                        gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  g_autoptr (GVariant) send_buffer = NULL;
  Control_Call call = {};
  int64_t card;
  int64_t context;
  uint64_t control_code;
  uint64_t send_buffer_size;
  uint64_t recv_buffer_size;
  const uint8_t *send_data;
  size_t send_size;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] Control");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_variant_get (call_variant, "(xt@aytt)",
                 &card,
                 &control_code,
                 &send_buffer,
                 &send_buffer_size,
                 &recv_buffer_size);

  if (!resolve_context_for_card (smartcard, invocation, card, &context))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  send_data = g_variant_get_fixed_array (send_buffer, &send_size,
                                         sizeof (uint8_t));

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  smartcard_scard_handle_native_to_redir (&call.handles.hCard, card);
  call.dwControlCode = NARROW_64_TO_32 (control_code);
  call.cbInBufferSize = NARROW_64_TO_32 (send_buffer_size);
  call.pvInBuffer = (uint8_t *) send_data;
  call.cbOutBufferSize = NARROW_64_TO_32 (recv_buffer_size);

  status = rdpdr_context->SmartcardControl (rdpdr_context,
                                            invocation,
                                            &call,
                                            &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardControl failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_transmit_complete (RdpdrServerContext    *rdpdr_context,
                                void                  *callback_data,
                                uint32_t               io_status,
                                int32_t                return_code,
                                const Transmit_Return *ret)
{
  GDBusMethodInvocation *invocation = callback_data;
  uint64_t recv_pci_protocol = 0;
  const uint8_t *recv_buffer = NULL;
  size_t recv_length = 0;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] Transmit complete: return_code=%i", return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  if (ret)
    {
      if (ret->pioRecvPci)
        {
          recv_pci_protocol =
            EXTEND_32_TO_64 (convert_protocol_to_pcsc (ret->pioRecvPci->dwProtocol));
        }
      recv_buffer = ret->pbRecvBuffer;
      recv_length = ret->cbRecvLength;
    }

  ret_variant = g_variant_new ("((xt@ay))",
                               (int64_t) EXTEND_32_TO_64 (return_code),
                               recv_pci_protocol,
                               g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                          recv_buffer,
                                                          recv_length,
                                                          sizeof (uint8_t)));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_transmit (GrdDBusPcscdSession   *skeleton,
                    GDBusMethodInvocation *invocation,
                    GVariant              *call_variant,
                    gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  g_autoptr (GVariant) send_buffer = NULL;
  SCARD_IO_REQUEST send_pci = {};
  SCARD_IO_REQUEST recv_pci = {};
  Transmit_Call call = {};
  int64_t card;
  int64_t context;
  uint64_t send_pci_protocol;
  uint64_t recv_pci_protocol_in;
  uint64_t recv_buffer_size;
  const uint8_t *send_data;
  size_t send_size;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] Transmit");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_variant_get (call_variant, "(xt@aytt)",
                 &card,
                 &send_pci_protocol,
                 &send_buffer,
                 &recv_pci_protocol_in,
                 &recv_buffer_size);

  if (!resolve_context_for_card (smartcard, invocation, card, &context))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  send_data = g_variant_get_fixed_array (send_buffer, &send_size,
                                         sizeof (uint8_t));

  send_pci.dwProtocol =
    convert_protocol_to_winscard (NARROW_64_TO_32 (send_pci_protocol));
  send_pci.cbPciLength = sizeof (SCARD_IO_REQUEST);

  recv_pci.dwProtocol =
    convert_protocol_to_winscard (NARROW_64_TO_32 (recv_pci_protocol_in));
  recv_pci.cbPciLength = sizeof (SCARD_IO_REQUEST);

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  smartcard_scard_handle_native_to_redir (&call.handles.hCard, card);
  call.pioSendPci = &send_pci;
  call.cbSendLength = NARROW_64_TO_32 (send_size);
  call.pbSendBuffer = (uint8_t *) send_data;
  call.pioRecvPci = &recv_pci;
  call.cbRecvLength = NARROW_64_TO_32 (recv_buffer_size);

  status = rdpdr_context->SmartcardTransmit (rdpdr_context,
                                             invocation,
                                             &call,
                                             &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardTransmit failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_list_reader_groups_complete (RdpdrServerContext            *rdpdr_context,
                                          void                          *callback_data,
                                          uint32_t                       io_status,
                                          int32_t                        return_code,
                                          const ListReaderGroups_Return *ret)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariantBuilder *builder;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] ListReaderGroups complete: return_code=%i",
           return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));

  if (ret && ret->msz && return_code == SCARD_S_SUCCESS)
    {
      const char *p = (const char *) ret->msz;
      const char *end = (const char *) (ret->msz + ret->cBytes);

      while (p < end && *p)
        {
          g_variant_builder_add (builder, "s", p);
          p += strlen (p) + 1;
        }
    }

  ret_variant = g_variant_new ("((x@as))",
                               (int64_t) EXTEND_32_TO_64 (return_code),
                               g_variant_builder_end (builder));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_list_reader_groups (GrdDBusPcscdSession   *skeleton,
                              GDBusMethodInvocation *invocation,
                              GVariant              *call_variant,
                              gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  ListReaderGroups_Call call = {};
  int64_t context;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] ListReaderGroups");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  g_variant_get (call_variant, "(x)", &context);

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  call.cchGroups = SCARD_AUTOALLOCATE;

  status = rdpdr_context->SmartcardListReaderGroupsA (rdpdr_context,
                                                      invocation,
                                                      &call,
                                                      &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardListReaderGroups failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_list_readers_complete (RdpdrServerContext       *rdpdr_context,
                                    void                     *callback_data,
                                    uint32_t                  io_status,
                                    int32_t                   return_code,
                                    const ListReaders_Return *ret)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariantBuilder *builder;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] ListReaders complete: return_code=%i", return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));

  if (ret && ret->msz && return_code == SCARD_S_SUCCESS)
    {
      const char *p = (const char *) ret->msz;
      const char *end = (const char *) (ret->msz + ret->cBytes);

      while (p < end && *p)
        {
          g_variant_builder_add (builder, "s", p);
          p += strlen (p) + 1;
        }
    }

  ret_variant = g_variant_new ("((x@as))",
                               (int64_t) EXTEND_32_TO_64 (return_code),
                               g_variant_builder_end (builder));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_list_readers (GrdDBusPcscdSession   *skeleton,
                        GDBusMethodInvocation *invocation,
                        GVariant              *call_variant,
                        gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  ListReaders_Call call = {};
  int64_t context;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] ListReaders");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  g_variant_get (call_variant, "(x)", &context);

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  call.cchReaders = SCARD_AUTOALLOCATE;

  status = rdpdr_context->SmartcardListReadersA (rdpdr_context,
                                                 invocation,
                                                 &call,
                                                 &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardListReaders failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_cancel_complete (RdpdrServerContext *rdpdr_context,
                              void               *callback_data,
                              uint32_t            io_status,
                              int32_t             return_code)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] Cancel complete: return_code=%i", return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  ret_variant = g_variant_new ("((x))",
                               (int64_t) EXTEND_32_TO_64 (return_code));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_cancel (GrdDBusPcscdSession   *skeleton,
                  GDBusMethodInvocation *invocation,
                  GVariant              *call_variant,
                  gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  REDIR_SCARDCONTEXT redir_context = {};
  int64_t context;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] Cancel");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  g_variant_get (call_variant, "(x)", &context);
  smartcard_scard_context_native_to_redir (&redir_context, context);

  status = rdpdr_context->SmartcardCancel (rdpdr_context,
                                           invocation,
                                           &redir_context,
                                           &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardCancel failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_get_attrib_complete (RdpdrServerContext       *rdpdr_context,
                                  void                     *callback_data,
                                  uint32_t                  io_status,
                                  int32_t                   return_code,
                                  const GetAttrib_Return   *ret)
{
  GDBusMethodInvocation *invocation = callback_data;
  const uint8_t *attr = NULL;
  size_t attr_len = 0;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] GetAttrib complete: return_code=%i", return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  if (ret)
    {
      attr = ret->pbAttr;
      attr_len = ret->cbAttrLen;
    }

  ret_variant = g_variant_new ("((x@ay))",
                               (int64_t) EXTEND_32_TO_64 (return_code),
                               g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                          attr, attr_len,
                                                          sizeof (uint8_t)));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_get_attrib (GrdDBusPcscdSession   *skeleton,
                      GDBusMethodInvocation *invocation,
                      GVariant              *call_variant,
                      gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  GetAttrib_Call call = {};
  int64_t card;
  int64_t context;
  uint64_t attr_id;
  uint64_t attr_len;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] GetAttrib");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_variant_get (call_variant, "(xtt)", &card, &attr_id, &attr_len);

  if (!resolve_context_for_card (smartcard, invocation, card, &context))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  smartcard_scard_handle_native_to_redir (&call.handles.hCard, card);
  call.dwAttrId = NARROW_64_TO_32 (attr_id);
  call.cbAttrLen = NARROW_64_TO_32 (attr_len);

  status = rdpdr_context->SmartcardGetAttrib (rdpdr_context,
                                              invocation,
                                              &call,
                                              &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardGetAttrib failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_smartcard_set_attrib_complete (RdpdrServerContext *rdpdr_context,
                                  void               *callback_data,
                                  uint32_t            io_status,
                                  int32_t             return_code)
{
  GDBusMethodInvocation *invocation = callback_data;
  GVariant *ret_variant;

  g_debug ("[RDP.SMARTCARD] SetAttrib complete: return_code=%i", return_code);

  remove_pending_invocation (rdpdr_context, invocation);

  ret_variant = g_variant_new ("((x))",
                               (int64_t) EXTEND_32_TO_64 (return_code));
  g_dbus_method_invocation_return_value (invocation, ret_variant);
}

static gboolean
on_handle_set_attrib (GrdDBusPcscdSession   *skeleton,
                      GDBusMethodInvocation *invocation,
                      GVariant              *call_variant,
                      gpointer               user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  RdpdrServerContext *rdpdr_context = smartcard->rdpdr_context;
  g_autoptr (GVariant) attr_buffer = NULL;
  SetAttrib_Call call = {};
  int64_t card;
  int64_t context;
  uint64_t attr_id;
  const uint8_t *attr_data;
  size_t attr_size;
  uint32_t completion_id;
  uint32_t status;

  g_debug ("[RDP.SMARTCARD] SetAttrib");

  if (is_device_unavailable (smartcard, invocation))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_variant_get (call_variant, "(xt@ay)",
                 &card,
                 &attr_id,
                 &attr_buffer);

  if (!resolve_context_for_card (smartcard, invocation, card, &context))
    return G_DBUS_METHOD_INVOCATION_HANDLED;

  g_hash_table_add (smartcard->pending_invocations, invocation);

  attr_data = g_variant_get_fixed_array (attr_buffer, &attr_size,
                                         sizeof (uint8_t));

  smartcard_scard_context_native_to_redir (&call.handles.hContext, context);
  smartcard_scard_handle_native_to_redir (&call.handles.hCard, card);
  call.dwAttrId = NARROW_64_TO_32 (attr_id);
  call.cbAttrLen = attr_size;
  call.pbAttr = (uint8_t *) attr_data;

  status = rdpdr_context->SmartcardSetAttrib (rdpdr_context,
                                              invocation,
                                              &call,
                                              &completion_id);
  if (status != CHANNEL_RC_OK)
    {
      g_hash_table_remove (smartcard->pending_invocations, invocation);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "SmartcardSetAttrib failed: %u",
                                             status);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
connect_pcscd_session_handlers (GrdDBusPcscdSession *skeleton,
                                GrdRdpSmartcard     *smartcard)
{
  g_signal_connect_object (skeleton, "handle-establish-context",
                           G_CALLBACK (on_handle_establish_context),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-release-context",
                           G_CALLBACK (on_handle_release_context),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-is-valid-context",
                           G_CALLBACK (on_handle_is_valid_context),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-connect-card",
                           G_CALLBACK (on_handle_connect_card),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-reconnect",
                           G_CALLBACK (on_handle_reconnect),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-disconnect-card",
                           G_CALLBACK (on_handle_disconnect_card),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-begin-transaction",
                           G_CALLBACK (on_handle_begin_transaction),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-end-transaction",
                           G_CALLBACK (on_handle_end_transaction),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-status-card",
                           G_CALLBACK (on_handle_status_card),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-get-status-change",
                           G_CALLBACK (on_handle_get_status_change),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-control-card",
                           G_CALLBACK (on_handle_control_card),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-transmit",
                           G_CALLBACK (on_handle_transmit),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-list-reader-groups",
                           G_CALLBACK (on_handle_list_reader_groups),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-list-readers",
                           G_CALLBACK (on_handle_list_readers),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-cancel",
                           G_CALLBACK (on_handle_cancel),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-get-attrib",
                           G_CALLBACK (on_handle_get_attrib),
                           smartcard, G_CONNECT_DEFAULT);
  g_signal_connect_object (skeleton, "handle-set-attrib",
                           G_CALLBACK (on_handle_set_attrib),
                           smartcard, G_CONNECT_DEFAULT);
}

static void
on_private_connection_ready (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  g_autoptr (GDBusConnection) private_connection = NULL;
  g_autoptr (GrdDBusPcscdSession) private_proxy = NULL;
  g_autoptr (GError) error = NULL;
  GrdRdpSmartcard *smartcard = user_data;

  private_connection = g_dbus_connection_new_finish (result, &error);
  if (!private_connection)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("[RDP.SMARTCARD] Failed to create private connection: %s",
                     error->message);
        }
      return;
    }

  private_proxy = grd_dbus_pcscd_session_skeleton_new ();
  if (!g_dbus_interface_skeleton_export (
         G_DBUS_INTERFACE_SKELETON (private_proxy),
         private_connection,
         "/",
         &error))
    {
      g_warning ("[RDP.SMARTCARD] Failed to export session skeleton: %s",
                 error->message);
      return;
    }

  connect_pcscd_session_handlers (private_proxy, smartcard);

  smartcard->private_proxy = g_steal_pointer (&private_proxy);

  g_debug ("[RDP.SMARTCARD] Private D-Bus connection established");
}

static void
create_private_proxy (GrdRdpSmartcard *smartcard,
                      int              fd)
{
  g_autoptr (GOutputStream) output_stream = NULL;
  g_autoptr (GInputStream) input_stream = NULL;
  g_autoptr (GIOStream) io_stream = NULL;

  /* fd ownership: fd -> output_stream -> io_stream -> private_connection -> proxy */
  output_stream = g_unix_output_stream_new (fd, TRUE);
  input_stream = g_unix_input_stream_new (fd, FALSE);
  io_stream = g_simple_io_stream_new (input_stream, output_stream);

  g_dbus_connection_new (io_stream,
                         NULL,
                         G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                         NULL,
                         smartcard->private_proxy_cancellable,
                         on_private_connection_ready,
                         smartcard);
}

static void
on_connect_to_grd_pcscd_finished (GObject      *source_object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  GrdDBusPcscd *pcscd_proxy = GRD_DBUS_PCSCD (source_object);
  PcscdConnectData *data = user_data;
  GrdRdpSmartcard *smartcard = data->smartcard;
  g_autofd int fd = g_steal_fd (&data->fd);
  g_autoptr (GError) error = NULL;

  g_free (data);

  if (!grd_dbus_pcscd_call_connect_finish (pcscd_proxy,
                                           NULL,
                                           result,
                                           &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("[RDP.SMARTCARD] Failed to connect to pcscd: %s",
                     error->message);
        }
      return;
    }

  g_debug ("[RDP.SMARTCARD] Connected to pcscd, setting up private D-Bus");

  create_private_proxy (smartcard, g_steal_fd (&fd));
}

static void
connect_to_grd_pcscd (GrdRdpSmartcard *smartcard)
{
  g_autoptr (GUnixFDList) fd_list = NULL;
  g_autoptr (GError) error = NULL;
  PcscdConnectData *data;
  g_autofd int local_fd = -1;
  int fds[2];
  int fd_idx;

  g_assert (!smartcard->private_proxy_cancellable);

  smartcard->private_proxy_cancellable = g_cancellable_new ();

  if (socketpair (AF_UNIX, SOCK_STREAM, 0, fds) < 0)
    {
      g_warning ("[RDP.SMARTCARD] Failed to create socketpair: %s",
                 g_strerror (errno));
      return;
    }

  local_fd = fds[0];

  fd_list = g_unix_fd_list_new ();
  fd_idx = g_unix_fd_list_append (fd_list, fds[1], &error);
  close (fds[1]);

  if (fd_idx < 0)
    {
      g_warning ("[RDP.SMARTCARD] Failed to append fd to list: %s",
                 error->message);
      return;
    }

  data = g_new0 (PcscdConnectData, 1);
  data->smartcard = smartcard;
  data->fd = g_steal_fd (&local_fd);

  grd_dbus_pcscd_call_connect (smartcard->pcscd_proxy,
                               g_variant_new_handle (fd_idx),
                               fd_list,
                               smartcard->private_proxy_cancellable,
                               on_connect_to_grd_pcscd_finished,
                               data);
}

static gboolean
fail_pending_invocation_by_connection (gpointer key,
                                       gpointer value,
                                       gpointer user_data)
{
  GDBusMethodInvocation *invocation = key;
  GDBusConnection *connection = user_data;

  if (g_dbus_method_invocation_get_connection (invocation) != connection)
    return FALSE;

  g_dbus_method_invocation_return_error (invocation,
                                         G_DBUS_ERROR,
                                         G_DBUS_ERROR_FAILED,
                                         "Smartcard device disconnected");
  return TRUE;
}

static void
unexport_and_fail_pending_invocations (GrdRdpSmartcard      *smartcard,
                                       GrdDBusPcscdSession **proxy)
{
  GDBusInterfaceSkeleton *skeleton;
  GDBusConnection *connection;

  if (!*proxy)
    return;
 
  skeleton = G_DBUS_INTERFACE_SKELETON (*proxy);
  connection = g_dbus_interface_skeleton_get_connection (skeleton);

  g_dbus_interface_skeleton_unexport (skeleton);
  g_clear_object (proxy);

  g_hash_table_foreach_remove (smartcard->pending_invocations,
                               fail_pending_invocation_by_connection,
                               connection);
}

static void
disconnect_from_grd_pcscd (GrdRdpSmartcard *smartcard)
{
  g_cancellable_cancel (smartcard->private_proxy_cancellable);
  g_clear_object (&smartcard->private_proxy_cancellable);

  unexport_and_fail_pending_invocations (smartcard, &smartcard->private_proxy);
}

static void
on_grd_pcscd_name_owner_changed (GrdRdpSmartcard *smartcard)
{
  GrdDBusPcscd *pcscd_proxy = smartcard->pcscd_proxy;
  g_autofree char *name_owner = NULL;

  name_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (pcscd_proxy));

  disconnect_from_grd_pcscd (smartcard);

  if (name_owner)
    connect_to_grd_pcscd (smartcard);
}

static void
on_pcscd_proxy_acquired (GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  GrdRdpSmartcard *smartcard = user_data;
  g_autoptr (GError) error = NULL;
  GrdDBusPcscd *pcscd_proxy;
  g_autofree char *name_owner = NULL;

  pcscd_proxy = grd_dbus_pcscd_proxy_new_for_bus_finish (result, &error);
  if (!pcscd_proxy)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("[RDP.SMARTCARD] Failed to create pcscd proxy: %s",
                     error->message);
        }
      return;
    }

  smartcard->pcscd_proxy = pcscd_proxy;

  smartcard->name_owner_changed_id =
    g_signal_connect_swapped (pcscd_proxy, "notify::g-name-owner",
                              G_CALLBACK (on_grd_pcscd_name_owner_changed),
                              smartcard);

  name_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (pcscd_proxy));
  if (name_owner)
    connect_to_grd_pcscd (smartcard);
}

static void
setup_private_proxy (GrdRdpSmartcard *smartcard)
{
  g_assert (!smartcard->pcscd_proxy_cancellable);

  smartcard->pcscd_proxy_cancellable = g_cancellable_new ();

  grd_dbus_pcscd_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                    G_DBUS_PROXY_FLAGS_NONE,
                                    REMOTE_DESKTOP_PCSCD_BUS_NAME,
                                    REMOTE_DESKTOP_PCSCD_OBJECT_PATH,
                                    smartcard->pcscd_proxy_cancellable,
                                    on_pcscd_proxy_acquired,
                                    smartcard);
}

static void
teardown_private_proxy (GrdRdpSmartcard *smartcard)
{
  g_cancellable_cancel (smartcard->pcscd_proxy_cancellable);
  g_clear_object (&smartcard->pcscd_proxy_cancellable);

  g_clear_signal_handler (&smartcard->name_owner_changed_id, 
                          smartcard->pcscd_proxy);

  disconnect_from_grd_pcscd (smartcard);

  g_clear_object (&smartcard->pcscd_proxy);
}

static char *
get_session_id (void)
{
  const char *session_id;
  char *logind_session_id = NULL;

  session_id = g_getenv ("XDG_SESSION_ID");
  if (session_id && session_id[0] != '\0')
    return g_strdup (session_id);

  logind_session_id = grd_get_session_id_from_pid (getpid ());
  if (logind_session_id)
    return logind_session_id;

  logind_session_id = grd_get_session_id_from_uid (getuid ());
  if (!logind_session_id)
    g_warning ("[RDP.SMARTCARD] Failed to get session ID");

  return logind_session_id;
}

static void
on_session_bus_acquired (GDBusConnection *connection,
                         const char      *name,
                         gpointer         user_data)
{
  g_autoptr (GrdDBusPcscdSession) session_proxy = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *session_id = NULL;
  g_autofree char *object_path = NULL;
  GrdRdpSmartcard *smartcard = user_data;

  session_id = get_session_id ();
  if (!session_id)
    return;

  object_path =
    g_strdup_printf (REMOTE_DESKTOP_PCSCD_OBJECT_PATH "/%s", session_id);

  session_proxy = grd_dbus_pcscd_session_skeleton_new ();
  if (!g_dbus_interface_skeleton_export (
         G_DBUS_INTERFACE_SKELETON (session_proxy),
         connection,
         object_path,
         &error))
    {
      g_warning ("[RDP.SMARTCARD] Failed to export session proxy: %s",
                 error->message);
      return;
    }

  connect_pcscd_session_handlers (session_proxy, smartcard);

  smartcard->session_proxy = g_steal_pointer (&session_proxy);

  g_debug ("[RDP.SMARTCARD] Session bus proxy established at %s", object_path);
}

static void
on_session_bus_name_acquired (GDBusConnection *connection,
                              const char      *name,
                              gpointer         user_data)
{
  g_debug ("[RDP.SMARTCARD] Acquired session bus name %s", name);
}

static void
on_session_bus_name_lost (GDBusConnection *connection,
                          const char      *name,
                          gpointer         user_data)
{
  g_debug ("[RDP.SMARTCARD] Lost session bus name %s", name);
}

static void
setup_session_proxy (GrdRdpSmartcard *smartcard)
{
  g_assert (!smartcard->session_bus_name_id);

  smartcard->session_bus_name_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                                   REMOTE_DESKTOP_PCSCD_BUS_NAME,
                                                   G_BUS_NAME_OWNER_FLAGS_NONE,
                                                   on_session_bus_acquired,
                                                   on_session_bus_name_acquired,
                                                   on_session_bus_name_lost,
                                                   smartcard,
                                                   NULL);
}

static void
teardown_session_proxy (GrdRdpSmartcard *smartcard)
{
  g_clear_handle_id (&smartcard->session_bus_name_id, g_bus_unown_name);

  unexport_and_fail_pending_invocations (smartcard, &smartcard->session_proxy);
}

static void
setup_pcscd_proxies (GrdRdpSmartcard *smartcard)
{
  setup_private_proxy (smartcard);
  setup_session_proxy (smartcard);
}

static void
teardown_pcscd_proxies (GrdRdpSmartcard *smartcard)
{
  teardown_private_proxy (smartcard);
  teardown_session_proxy (smartcard);
  g_hash_table_remove_all (smartcard->card_to_context);
}

static UINT
on_smartcard_create (RdpdrServerContext *rdpdr_context,
                     const RdpdrDevice  *device)
{
  GrdRdpDeviceRedirection *device_redirection = rdpdr_context->data;
  GrdRdpSmartcard *smartcard;

  smartcard = grd_rdp_device_redirection_get_smartcard (device_redirection);
  if (!smartcard)
    return CHANNEL_RC_OK;

  g_mutex_lock (&smartcard->shutdown_mutex);
  if (smartcard->in_shutdown)
    {
      g_mutex_unlock (&smartcard->shutdown_mutex);
      return CHANNEL_RC_OK;
    }
  g_mutex_unlock (&smartcard->shutdown_mutex);

  if (g_hash_table_contains (smartcard->smartcard_device_ids,
                             GUINT_TO_POINTER (device->DeviceId)))
    {
      g_warning ("[RDP.SMARTCARD] Smartcard device already exists: id=%u",
                 device->DeviceId);
      return CHANNEL_RC_OK;
    }

  g_debug ("[RDP.SMARTCARD] Smartcard device created: id=%u", device->DeviceId);

  if (g_hash_table_size (smartcard->smartcard_device_ids) == 0)
    setup_pcscd_proxies (smartcard);

  g_hash_table_add (smartcard->smartcard_device_ids,
                    GUINT_TO_POINTER (device->DeviceId));

  return CHANNEL_RC_OK;
}

static UINT
on_smartcard_delete (RdpdrServerContext *rdpdr_context,
                     uint32_t            device_id)
{
  GrdRdpDeviceRedirection *device_redirection = rdpdr_context->data;
  GrdRdpSmartcard *smartcard;

  smartcard = grd_rdp_device_redirection_get_smartcard (device_redirection);
  if (!smartcard)
    return CHANNEL_RC_OK;

  g_mutex_lock (&smartcard->shutdown_mutex);
  if (smartcard->in_shutdown)
    {
      g_mutex_unlock (&smartcard->shutdown_mutex);
      return CHANNEL_RC_OK;
    }
  g_mutex_unlock (&smartcard->shutdown_mutex);

  if (!g_hash_table_remove (smartcard->smartcard_device_ids,
                            GUINT_TO_POINTER (device_id)))
    return CHANNEL_RC_OK;

  g_debug ("[RDP.SMARTCARD] Smartcard device deleted: id=%u", device_id);

  if (g_hash_table_size (smartcard->smartcard_device_ids) == 0)
    teardown_pcscd_proxies (smartcard);

  return CHANNEL_RC_OK;
}

void
grd_rdp_smartcard_invoke_shutdown (GrdRdpSmartcard *smartcard)
{
  g_mutex_lock (&smartcard->shutdown_mutex);
  smartcard->in_shutdown = TRUE;
  g_mutex_unlock (&smartcard->shutdown_mutex);
}

GrdRdpSmartcard *
grd_rdp_smartcard_new (GrdSessionRdp      *session_rdp,
                       RdpdrServerContext *rdpdr_context)
{
  GrdRdpSmartcard *smartcard;
  GrdContext *context;

  context = grd_session_get_context (GRD_SESSION (session_rdp));
  g_assert (grd_context_get_runtime_mode (context) != GRD_RUNTIME_MODE_SYSTEM);

  smartcard = g_object_new (GRD_TYPE_RDP_SMARTCARD, NULL);

  smartcard->rdpdr_context = rdpdr_context;

  rdpdr_context->OnSmartcardCreate = on_smartcard_create;
  rdpdr_context->OnSmartcardDelete = on_smartcard_delete;
  rdpdr_context->OnSmartcardEstablishContextComplete =
    on_smartcard_establish_context_complete;
  rdpdr_context->OnSmartcardReleaseContextComplete =
    on_smartcard_release_context_complete;
  rdpdr_context->OnSmartcardIsValidContextComplete =
    on_smartcard_is_valid_context_complete;
  rdpdr_context->OnSmartcardConnectComplete =
    on_smartcard_connect_complete;
  rdpdr_context->OnSmartcardReconnectComplete =
    on_smartcard_reconnect_complete;
  rdpdr_context->OnSmartcardDisconnectComplete =
    on_smartcard_disconnect_complete;
  rdpdr_context->OnSmartcardBeginTransactionComplete =
    on_smartcard_begin_transaction_complete;
  rdpdr_context->OnSmartcardEndTransactionComplete =
    on_smartcard_end_transaction_complete;
  rdpdr_context->OnSmartcardStatusComplete =
    on_smartcard_status_complete;
  rdpdr_context->OnSmartcardGetStatusChangeComplete =
    on_smartcard_get_status_change_complete;
  rdpdr_context->OnSmartcardControlComplete =
    on_smartcard_control_complete;
  rdpdr_context->OnSmartcardTransmitComplete =
    on_smartcard_transmit_complete;
  rdpdr_context->OnSmartcardListReaderGroupsComplete =
    on_smartcard_list_reader_groups_complete;
  rdpdr_context->OnSmartcardListReadersComplete =
    on_smartcard_list_readers_complete;
  rdpdr_context->OnSmartcardCancelComplete =
    on_smartcard_cancel_complete;
  rdpdr_context->OnSmartcardGetAttribComplete =
    on_smartcard_get_attrib_complete;
  rdpdr_context->OnSmartcardSetAttribComplete =
    on_smartcard_set_attrib_complete;

  rdpdr_context->supported |= RDPDR_DTYP_SMARTCARD;

  return smartcard;
}
static void
grd_rdp_smartcard_dispose (GObject *object)
{
  GrdRdpSmartcard *smartcard = GRD_RDP_SMARTCARD (object);

  teardown_pcscd_proxies (smartcard);

  g_clear_pointer (&smartcard->smartcard_device_ids, g_hash_table_unref);
  g_clear_pointer (&smartcard->pending_invocations, g_hash_table_unref);
  g_clear_pointer (&smartcard->card_to_context, g_hash_table_unref);

  G_OBJECT_CLASS (grd_rdp_smartcard_parent_class)->dispose (object);
}

static void
grd_rdp_smartcard_finalize (GObject *object)
{
  GrdRdpSmartcard *smartcard = GRD_RDP_SMARTCARD (object);

  g_mutex_clear (&smartcard->shutdown_mutex);

  G_OBJECT_CLASS (grd_rdp_smartcard_parent_class)->finalize (object);
}

static void
grd_rdp_smartcard_init (GrdRdpSmartcard *smartcard)
{
  g_mutex_init (&smartcard->shutdown_mutex);

  smartcard->smartcard_device_ids = g_hash_table_new (NULL, NULL);
  smartcard->pending_invocations = g_hash_table_new (NULL, NULL);
  smartcard->card_to_context = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                                      g_free, g_free);
}

static void
grd_rdp_smartcard_class_init (GrdRdpSmartcardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_smartcard_dispose;
  object_class->finalize = grd_rdp_smartcard_finalize;
}
