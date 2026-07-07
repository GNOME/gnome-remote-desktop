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

#include "grd-context.h"
#include "grd-dbus-pcscd.h"
#include "grd-private.h"
#include "grd-rdp-device-redirection.h"
#include "grd-session-rdp.h"

struct _GrdRdpSmartcard
{
  GObject parent;

  GMutex shutdown_mutex;
  gboolean in_shutdown;

  RdpdrServerContext *rdpdr_context;
  GHashTable *smartcard_device_ids;

  /* System bus proxy to grd-pcscd */
  GCancellable *pcscd_proxy_cancellable;
  GrdDBusPcscd *pcscd_proxy;
};

G_DEFINE_TYPE (GrdRdpSmartcard, grd_rdp_smartcard, G_TYPE_OBJECT)

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

  g_clear_object (&smartcard->pcscd_proxy);
}

static void
setup_pcscd_proxies (GrdRdpSmartcard *smartcard)
{
  setup_private_proxy (smartcard);
}

static void
teardown_pcscd_proxies (GrdRdpSmartcard *smartcard)
{
  teardown_private_proxy (smartcard);
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

  rdpdr_context->supported |= RDPDR_DTYP_SMARTCARD;

  return smartcard;
}
static void
grd_rdp_smartcard_dispose (GObject *object)
{
  GrdRdpSmartcard *smartcard = GRD_RDP_SMARTCARD (object);

  teardown_pcscd_proxies (smartcard);

  g_clear_pointer (&smartcard->smartcard_device_ids, g_hash_table_unref);

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
}

static void
grd_rdp_smartcard_class_init (GrdRdpSmartcardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_smartcard_dispose;
  object_class->finalize = grd_rdp_smartcard_finalize;
}
