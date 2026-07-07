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

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib/gstdio.h>

#include "grd-context.h"
#include "grd-dbus-pcscd.h"
#include "grd-private.h"
#include "grd-rdp-device-redirection.h"
#include "grd-session-rdp.h"

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

  /* System bus proxy to grd-pcscd */
  GCancellable *pcscd_proxy_cancellable;
  unsigned long name_owner_changed_id;
  GrdDBusPcscd *pcscd_proxy;

  /* Private peer-to-peer proxy for system-level clients (via grd-pcscd) */
  GCancellable *private_proxy_cancellable;
  GrdDBusPcscdSession *private_proxy;
};

G_DEFINE_TYPE (GrdRdpSmartcard, grd_rdp_smartcard, G_TYPE_OBJECT)

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

static void
disconnect_from_grd_pcscd (GrdRdpSmartcard *smartcard)
{
  g_cancellable_cancel (smartcard->private_proxy_cancellable);
  g_clear_object (&smartcard->private_proxy_cancellable);

  if (smartcard->private_proxy)
    {
      g_dbus_interface_skeleton_unexport (
        G_DBUS_INTERFACE_SKELETON (smartcard->private_proxy));
      g_clear_object (&smartcard->private_proxy);
    }
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
