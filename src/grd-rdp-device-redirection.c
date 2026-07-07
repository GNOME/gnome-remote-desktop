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

#include "grd-rdp-device-redirection.h"

#include <freerdp/server/rdpdr.h>

#include "grd-rdp-smartcard.h"

struct _GrdRdpDeviceRedirection
{
  GObject parent;

  RdpdrServerContext *rdpdr_context;

  GrdRdpSmartcard *smartcard;
};

G_DEFINE_TYPE (GrdRdpDeviceRedirection, grd_rdp_device_redirection,
               G_TYPE_OBJECT)

GrdRdpSmartcard *
grd_rdp_device_redirection_get_smartcard (GrdRdpDeviceRedirection *device_redirection)
{
  return device_redirection->smartcard;
}

GrdRdpDeviceRedirection *
grd_rdp_device_redirection_new (GrdSessionRdp *session_rdp,
                                HANDLE         vcm)
{
  g_autoptr (GrdRdpDeviceRedirection) device_redirection = NULL;
  RdpdrServerContext *rdpdr_context;

  device_redirection = g_object_new (GRD_TYPE_RDP_DEVICE_REDIRECTION, NULL);

  rdpdr_context = rdpdr_server_context_new (vcm);
  if (!rdpdr_context)
    g_error ("[RDP.RDPDR] Failed to create server context (OOM)");

  device_redirection->rdpdr_context = rdpdr_context;
  device_redirection->smartcard = grd_rdp_smartcard_new (session_rdp,
                                                         rdpdr_context);

  rdpdr_context->data = device_redirection;

  if (rdpdr_context->Start (rdpdr_context) != CHANNEL_RC_OK)
    {
      g_warning ("[RDP.RDPDR] Failed to start RDPDR channel");
      g_clear_pointer (&device_redirection->rdpdr_context,
                       rdpdr_server_context_free);
      return NULL;
    }

  return g_steal_pointer (&device_redirection);
}

static void
grd_rdp_device_redirection_dispose (GObject *object)
{
  GrdRdpDeviceRedirection *device_redirection =
    GRD_RDP_DEVICE_REDIRECTION (object);

  if (device_redirection->smartcard)
    grd_rdp_smartcard_invoke_shutdown (device_redirection->smartcard);

  if (device_redirection->rdpdr_context)
    {
      device_redirection->rdpdr_context->Stop (
        device_redirection->rdpdr_context);
      g_clear_pointer (&device_redirection->rdpdr_context,
                       rdpdr_server_context_free);
    }

  g_clear_object (&device_redirection->smartcard);

  G_OBJECT_CLASS (grd_rdp_device_redirection_parent_class)->dispose (object);
}

static void
grd_rdp_device_redirection_init (GrdRdpDeviceRedirection *device_redirection)
{
}

static void
grd_rdp_device_redirection_class_init (GrdRdpDeviceRedirectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_device_redirection_dispose;
}
