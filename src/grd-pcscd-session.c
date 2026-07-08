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

#include "grd-pcscd-session.h"

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib/gstdio.h>
#include <polkit/polkit.h>

#include "grd-dbus-pcscd.h"

#define PCSCD_OBJECT_PATH_PREFIX "/org/gnome/RemoteDesktop/Pcscd"
#define GRD_USE_GRD_PCSCD_POLKIT_ACTION "org.gnome.remotedesktop.use-grd-pcscd"

struct _GrdPcscdSession
{
  GObject parent;

  char *session_id;

  PolkitAuthority *authority;
  GHashTable *authorized_senders;

  GCancellable *cancellable;

  GrdDBusPcscdSession *private_proxy;
  GrdDBusPcscdSession *system_proxy;
};

enum
{
  CLOSED,

  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE (GrdPcscdSession, grd_pcscd_session, G_TYPE_OBJECT)

static void
on_polkit_authority_changed (PolkitAuthority *authority,
                             gpointer         user_data)
{
  GrdPcscdSession *session = user_data;

  g_debug ("[PCSCD.SESSION %s] Polkit rules changed, clearing authorization cache",
           session->session_id);
  g_hash_table_remove_all (session->authorized_senders);
}

static gboolean
ensure_polkit_authority (GrdPcscdSession *session)
{
  g_autoptr (GError) error = NULL;

  if (session->authority)
    return TRUE;

  session->authority = polkit_authority_get_sync (session->cancellable, &error);
  if (!session->authority)
    {
      g_warning ("[PCSCD.SESSION %s] Failed to get polkit authority: %s",
                 session->session_id, error->message);
      return FALSE;
    }

  g_signal_connect_object (session->authority, "changed",
                           G_CALLBACK (on_polkit_authority_changed),
                           session, G_CONNECT_DEFAULT);

  return TRUE;
}

static gboolean
check_polkit_action (GrdPcscdSession *session,
                     const char      *sender,
                     const char      *action)
{
  g_autoptr (PolkitAuthorizationResult) result = NULL;
  g_autoptr (PolkitSubject) subject = NULL;
  g_autoptr (GError) error = NULL;

  subject = polkit_system_bus_name_new (sender);
  result = polkit_authority_check_authorization_sync (session->authority,
                                                      subject,
                                                      action,
                                                      NULL,
                                                      POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE,
                                                      session->cancellable,
                                                      &error);
  if (!result)
    {
      g_warning ("[PCSCD.SESSION %s] Failed to check authorization for %s: %s",
                 session->session_id, action, error->message);
      return FALSE;
    }

  return polkit_authorization_result_get_is_authorized (result);
}

static gboolean
check_polkit (GrdPcscdSession *session,
              const char      *sender)
{
  if (!ensure_polkit_authority (session))
    return FALSE;

  return check_polkit_action (session, sender,
                              GRD_USE_GRD_PCSCD_POLKIT_ACTION);
}

static gboolean
on_authorize_method (GDBusInterfaceSkeleton *interface,
                     GDBusMethodInvocation  *invocation,
                     gpointer                user_data)
{
  GrdPcscdSession *session = user_data;
  const char *sender;

  sender = g_dbus_method_invocation_get_sender (invocation);

  if (g_hash_table_contains (session->authorized_senders, sender))
    return TRUE;

  if (!check_polkit (session, sender))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Not authorized");
      return FALSE;
    }

  g_hash_table_add (session->authorized_senders, g_strdup (sender));

  return TRUE;
}

static void
on_establish_context_finished (GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_establish_context_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(xx))",
                                                        ret));
}

static gboolean
on_handle_establish_context (GrdDBusPcscdSession   *skeleton,
                             GDBusMethodInvocation *invocation,
                             GVariant              *call,
                             gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_establish_context (session->private_proxy,
                                                 call,
                                                 session->cancellable,
                                                 on_establish_context_finished,
                                                 g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_release_context_finished (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_release_context_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(x))",
                                                        ret));
}

static gboolean
on_handle_release_context (GrdDBusPcscdSession   *skeleton,
                           GDBusMethodInvocation *invocation,
                           GVariant              *call,
                           gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_release_context (session->private_proxy,
                                               call,
                                               session->cancellable,
                                               on_release_context_finished,
                                               g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_is_valid_context_finished (GObject      *source_object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_is_valid_context_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(x))",
                                                        ret));
}

static gboolean
on_handle_is_valid_context (GrdDBusPcscdSession   *skeleton,
                            GDBusMethodInvocation *invocation,
                            GVariant              *call,
                            gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_is_valid_context (session->private_proxy,
                                                call,
                                                session->cancellable,
                                                on_is_valid_context_finished,
                                                g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_connect_card_finished (GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_connect_card_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(xxxt))",
                                                        ret));
}

static gboolean
on_handle_connect_card (GrdDBusPcscdSession   *skeleton,
                        GDBusMethodInvocation *invocation,
                        GVariant              *call,
                        gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_connect_card (session->private_proxy,
                                            call,
                                            session->cancellable,
                                            on_connect_card_finished,
                                            g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_reconnect_finished (GObject      *source_object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_reconnect_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(xt))",
                                                        ret));
}

static gboolean
on_handle_reconnect (GrdDBusPcscdSession   *skeleton,
                     GDBusMethodInvocation *invocation,
                     GVariant              *call,
                     gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_reconnect (session->private_proxy,
                                         call,
                                         session->cancellable,
                                         on_reconnect_finished,
                                         g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_disconnect_card_finished (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_disconnect_card_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(x))",
                                                        ret));
}

static gboolean
on_handle_disconnect_card (GrdDBusPcscdSession   *skeleton,
                           GDBusMethodInvocation *invocation,
                           GVariant              *call,
                           gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_disconnect_card (session->private_proxy,
                                               call,
                                               session->cancellable,
                                               on_disconnect_card_finished,
                                               g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_begin_transaction_finished (GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_begin_transaction_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(x))",
                                                        ret));
}

static gboolean
on_handle_begin_transaction (GrdDBusPcscdSession   *skeleton,
                             GDBusMethodInvocation *invocation,
                             GVariant              *call,
                             gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_begin_transaction (session->private_proxy,
                                                 call,
                                                 session->cancellable,
                                                 on_begin_transaction_finished,
                                                 g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_end_transaction_finished (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_end_transaction_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(x))",
                                                        ret));
}

static gboolean
on_handle_end_transaction (GrdDBusPcscdSession   *skeleton,
                           GDBusMethodInvocation *invocation,
                           GVariant              *call,
                           gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_end_transaction (session->private_proxy,
                                               call,
                                               session->cancellable,
                                               on_end_transaction_finished,
                                               g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_status_card_finished (GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_status_card_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(xsttay))",
                                                        ret));
}

static gboolean
on_handle_status_card (GrdDBusPcscdSession   *skeleton,
                       GDBusMethodInvocation *invocation,
                       GVariant              *call,
                       gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_status_card (session->private_proxy,
                                           call,
                                           session->cancellable,
                                           on_status_card_finished,
                                           g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_get_status_change_finished (GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_get_status_change_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(xa(ttay)))",
                                                        ret));
}

static gboolean
on_handle_get_status_change (GrdDBusPcscdSession   *skeleton,
                             GDBusMethodInvocation *invocation,
                             GVariant              *call,
                             gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_get_status_change (session->private_proxy,
                                                 call,
                                                 session->cancellable,
                                                 on_get_status_change_finished,
                                                 g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_control_card_finished (GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_control_card_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(xay))",
                                                        ret));
}

static gboolean
on_handle_control_card (GrdDBusPcscdSession   *skeleton,
                        GDBusMethodInvocation *invocation,
                        GVariant              *call,
                        gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_control_card (session->private_proxy,
                                            call,
                                            session->cancellable,
                                            on_control_card_finished,
                                            g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_transmit_finished (GObject      *source_object,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_transmit_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(xtay))",
                                                        ret));
}

static gboolean
on_handle_transmit (GrdDBusPcscdSession   *skeleton,
                    GDBusMethodInvocation *invocation,
                    GVariant              *call,
                    gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_transmit (session->private_proxy,
                                        call,
                                        session->cancellable,
                                        on_transmit_finished,
                                        g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_list_reader_groups_finished (GObject      *source_object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_list_reader_groups_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(xas))",
                                                        ret));
}

static gboolean
on_handle_list_reader_groups (GrdDBusPcscdSession   *skeleton,
                              GDBusMethodInvocation *invocation,
                              GVariant              *call,
                              gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_list_reader_groups (session->private_proxy,
                                                  call,
                                                  session->cancellable,
                                                  on_list_reader_groups_finished,
                                                  g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_list_readers_finished (GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_list_readers_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(xas))",
                                                        ret));
}

static gboolean
on_handle_list_readers (GrdDBusPcscdSession   *skeleton,
                        GDBusMethodInvocation *invocation,
                        GVariant              *call,
                        gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_list_readers (session->private_proxy,
                                            call,
                                            session->cancellable,
                                            on_list_readers_finished,
                                            g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_cancel_finished (GObject      *source_object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_cancel_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(x))",
                                                        ret));
}

static gboolean
on_handle_cancel (GrdDBusPcscdSession   *skeleton,
                  GDBusMethodInvocation *invocation,
                  GVariant              *call,
                  gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_cancel (session->private_proxy,
                                      call,
                                      session->cancellable,
                                      on_cancel_finished,
                                      g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_get_attrib_finished (GObject      *source_object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_get_attrib_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(xay))",
                                                        ret));
}

static gboolean
on_handle_get_attrib (GrdDBusPcscdSession   *skeleton,
                      GDBusMethodInvocation *invocation,
                      GVariant              *call,
                      gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_get_attrib (session->private_proxy,
                                          call,
                                          session->cancellable,
                                          on_get_attrib_finished,
                                          g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_set_attrib_finished (GObject      *source_object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  g_autoptr (GDBusMethodInvocation) invocation = user_data;
  g_autoptr (GVariant) ret = NULL;
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_pcscd_session_call_set_attrib_finish (
         GRD_DBUS_PCSCD_SESSION (source_object),
         &ret, result, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@(x))",
                                                        ret));
}

static gboolean
on_handle_set_attrib (GrdDBusPcscdSession   *skeleton,
                      GDBusMethodInvocation *invocation,
                      GVariant              *call,
                      gpointer               user_data)
{
  GrdPcscdSession *session = user_data;

  if (!session->private_proxy)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "RDP Smartcard not connected");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  grd_dbus_pcscd_session_call_set_attrib (session->private_proxy,
                                          call,
                                          session->cancellable,
                                          on_set_attrib_finished,
                                          g_object_ref (invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_private_connection_closed (GDBusConnection *connection,
                              gboolean         remote_peer_vanished,
                              GError          *error,
                              gpointer         user_data)
{
  GrdPcscdSession *session = user_data;

  g_debug ("[PCSCD.SESSION %s] Private connection closed", session->session_id);

  g_signal_emit (session, signals[CLOSED], 0);
}

static void
on_private_proxy_ready (GObject      *source_object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  g_autoptr (GrdDBusPcscdSession) private_proxy = NULL;
  g_autoptr (GError) error = NULL;
  GrdPcscdSession *session = user_data;
  GDBusConnection *private_connection;

  private_proxy = grd_dbus_pcscd_session_proxy_new_finish (result, &error);
  if (!private_proxy)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("[PCSCD.SESSION %s] Failed to create private proxy: %s",
                     session->session_id, error->message);
          g_signal_emit (session, signals[CLOSED], 0);
        }
      return;
    }

  private_connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (private_proxy));
  g_signal_connect_object (private_connection, "closed",
                           G_CALLBACK (on_private_connection_closed),
                           session, G_CONNECT_DEFAULT);

  session->private_proxy = g_steal_pointer (&private_proxy);

  g_debug ("[PCSCD.SESSION %s] Private D-Bus connection established",
           session->session_id);
}

static void
on_private_connection_ready (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  GrdPcscdSession *session = user_data;
  g_autoptr (GDBusConnection) private_connection = NULL;
  g_autoptr (GError) error = NULL;

  private_connection = g_dbus_connection_new_finish (result, &error);
  if (!private_connection)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("[PCSCD.SESSION %s] Failed to create private connection: %s",
                     session->session_id, error->message);
          g_signal_emit (session, signals[CLOSED], 0);
        }
      return;
    }

  grd_dbus_pcscd_session_proxy_new (private_connection,
                                    G_DBUS_PROXY_FLAGS_NONE,
                                    NULL,
                                    "/",
                                    session->cancellable,
                                    on_private_proxy_ready,
                                    session);
}

static gboolean
create_system_proxy (GrdPcscdSession  *session,
                     GDBusConnection  *system_connection,
                     GError          **error)
{
  g_autoptr (GrdDBusPcscdSession) system_proxy = NULL;
  g_autofree char *object_path = NULL;

  object_path =
    g_strdup_printf ("%s/%s", PCSCD_OBJECT_PATH_PREFIX, session->session_id);

  system_proxy = grd_dbus_pcscd_session_skeleton_new ();
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (system_proxy),
                                         system_connection,
                                         object_path,
                                         error))
    return FALSE;

  g_signal_connect_object (system_proxy, "g-authorize-method",
                           G_CALLBACK (on_authorize_method),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-establish-context",
                           G_CALLBACK (on_handle_establish_context),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-release-context",
                           G_CALLBACK (on_handle_release_context),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-is-valid-context",
                           G_CALLBACK (on_handle_is_valid_context),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-connect-card",
                           G_CALLBACK (on_handle_connect_card),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-reconnect",
                           G_CALLBACK (on_handle_reconnect),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-disconnect-card",
                           G_CALLBACK (on_handle_disconnect_card),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-begin-transaction",
                           G_CALLBACK (on_handle_begin_transaction),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-end-transaction",
                           G_CALLBACK (on_handle_end_transaction),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-status-card",
                           G_CALLBACK (on_handle_status_card),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-get-status-change",
                           G_CALLBACK (on_handle_get_status_change),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-control-card",
                           G_CALLBACK (on_handle_control_card),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-transmit",
                           G_CALLBACK (on_handle_transmit),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-list-reader-groups",
                           G_CALLBACK (on_handle_list_reader_groups),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-list-readers",
                           G_CALLBACK (on_handle_list_readers),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-cancel",
                           G_CALLBACK (on_handle_cancel),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-get-attrib",
                           G_CALLBACK (on_handle_get_attrib),
                           session, G_CONNECT_DEFAULT);
  g_signal_connect_object (system_proxy, "handle-set-attrib",
                           G_CALLBACK (on_handle_set_attrib),
                           session, G_CONNECT_DEFAULT);

  session->system_proxy = g_steal_pointer (&system_proxy);

  g_debug ("[PCSCD.SESSION %s] Exported on system bus at %s",
           session->session_id, object_path);

  return TRUE;
}

static void
create_private_proxy (GrdPcscdSession *session,
                      int              fd)
{
  g_autoptr (GInputStream) input_stream = NULL;
  g_autoptr (GOutputStream) output_stream = NULL;
  g_autoptr (GIOStream) io_stream = NULL;
  g_autofree char *guid = NULL;

  /* fd ownership: fd -> input_stream -> io_stream -> private_connection -> proxy */
  input_stream = g_unix_input_stream_new (fd, TRUE);
  output_stream = g_unix_output_stream_new (fd, FALSE);
  io_stream = g_simple_io_stream_new (input_stream, output_stream);

  guid = g_dbus_generate_guid ();
  g_dbus_connection_new (io_stream,
                         guid,
                         G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER |
                         G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_ALLOW_ANONYMOUS,
                         NULL,
                         session->cancellable,
                         on_private_connection_ready,
                         session);
}

const char *
grd_pcscd_session_get_session_id (GrdPcscdSession *session)
{
  return session->session_id;
}

GrdPcscdSession *
grd_pcscd_session_new (const char       *session_id,
                       int               fd,
                       GDBusConnection  *system_connection,
                       GError          **error)
{
  g_autoptr (GrdPcscdSession) session = NULL;

  session = g_object_new (GRD_TYPE_PCSCD_SESSION, NULL);
  session->session_id = g_strdup (session_id);
  session->authorized_senders = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        g_free, NULL);
  session->cancellable = g_cancellable_new ();

  if (!create_system_proxy (session, system_connection, error))
    {
      close (fd);
      return NULL;
    }

  create_private_proxy (session, fd);

  return g_steal_pointer (&session);
}

static void
grd_pcscd_session_dispose (GObject *object)
{
  GrdPcscdSession *session = GRD_PCSCD_SESSION (object);

  g_cancellable_cancel (session->cancellable);
  g_clear_object (&session->cancellable);

  g_clear_object (&session->authority);
  g_clear_pointer (&session->authorized_senders, g_hash_table_destroy);

  if (session->private_proxy)
    {
      GDBusConnection *private_connection =
        g_dbus_proxy_get_connection (G_DBUS_PROXY (session->private_proxy));

      if (!g_dbus_connection_is_closed (private_connection))
        g_dbus_connection_close (private_connection, NULL, NULL, NULL);

      g_clear_object (&session->private_proxy);
    }

  if (session->system_proxy)
    {
      g_dbus_interface_skeleton_unexport (
        G_DBUS_INTERFACE_SKELETON (session->system_proxy));
      g_clear_object (&session->system_proxy);
    }

  g_clear_pointer (&session->session_id, g_free);

  G_OBJECT_CLASS (grd_pcscd_session_parent_class)->dispose (object);
}

static void
grd_pcscd_session_init (GrdPcscdSession *session)
{
}

static void
grd_pcscd_session_class_init (GrdPcscdSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_pcscd_session_dispose;

  signals[CLOSED] =
    g_signal_new ("closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}
