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
#include "grd-rdp-device-redirection.h"
#include "grd-session-rdp.h"

struct _GrdRdpSmartcard
{
  GObject parent;

  GMutex shutdown_mutex;
  gboolean in_shutdown;

  RdpdrServerContext *rdpdr_context;
  GHashTable *smartcard_device_ids;
};

G_DEFINE_TYPE (GrdRdpSmartcard, grd_rdp_smartcard, G_TYPE_OBJECT)

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
