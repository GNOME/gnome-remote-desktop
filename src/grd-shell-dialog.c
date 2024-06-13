/*
 * Copyright (C) 2024 SUSE Software Solutions Germany GmbH
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
 */

#include "grd-shell-dialog.h"

#define SHELL_NAME                     "org.gnome.Shell"
#define SHELL_PORTAL_DESKTOP_PATH      "/org/freedesktop/portal/desktop"
#define SHELL_PORTAL_DESKTOP_GRD_PATH  "/org/freedesktop/portal/desktop/grd"
#define SHELL_PORTAL_ACCESS_INTERFACE  "org.freedesktop.impl.portal.Access"
#define SHELL_PORTAL_REQUEST_INTERFACE "org.freedesktop.impl.portal.Request"

enum
{
  ACCEPTED,
  CANCELLED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef enum _ShellDialogResponse
{
  OK = 0,
  CANCEL = 1,
  CLOSED = 2,
} ShellDialogResponse;

typedef struct _GrdShellDialog
{
  GObject parent;

  GDBusProxy *access_proxy;
  GDBusProxy *handle_proxy;

  GCancellable *cancellable;
} GrdShellDialog;

G_DEFINE_TYPE (GrdShellDialog, grd_shell_dialog, G_TYPE_OBJECT)

static void
ensure_dialog_closed_sync (GrdShellDialog *shell_dialog)
{
  g_autoptr (GError) error = NULL;

  if (!shell_dialog->handle_proxy)
    return;

  if (!g_dbus_proxy_call_sync (shell_dialog->handle_proxy,
                               "Close",
                               NULL,
                               G_DBUS_CALL_FLAGS_NONE,
                               -1,
                               shell_dialog->cancellable,
                               &error))
    g_warning ("Unable to close shell dialog: %s", error->message);

  g_clear_object (&shell_dialog->handle_proxy);
}

static void
on_shell_access_dialog_finished (GObject      *source,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  GrdShellDialog *shell_dialog = GRD_SHELL_DIALOG (user_data);
  g_autoptr (GError) error = NULL;
  ShellDialogResponse response = 0;
  GVariant *response_variant;

  g_clear_object (&shell_dialog->handle_proxy);

  response_variant = g_dbus_proxy_call_finish (G_DBUS_PROXY (source),
                                               result,
                                               &error);
  if (!response_variant)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Shell dialog failed: %s", error->message);

      return;
    }

  g_variant_get (response_variant, "(ua{sv})", &response, NULL);
  switch (response)
    {
    case OK:
      g_signal_emit (shell_dialog, signals[ACCEPTED], 0);
      break;
    case CANCEL:
    case CLOSED:
      g_signal_emit (shell_dialog, signals[CANCELLED], 0);
      break;
    }
}

void
grd_shell_dialog_open (GrdShellDialog *shell_dialog,
                       const char     *title,
                       const char     *description,
                       const char     *cancel_label,
                       const char     *accept_label)
{
  g_autoptr (GDBusProxy) handle_proxy = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *handle_path = NULL;
  GVariantBuilder builder;

  ensure_dialog_closed_sync (shell_dialog);

  handle_path = g_strdup_printf (SHELL_PORTAL_DESKTOP_GRD_PATH "/%u",
                                 g_random_int ());

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}",
                         "deny_label", g_variant_new_string (cancel_label));
  g_variant_builder_add (&builder, "{sv}",
                         "grant_label", g_variant_new_string (accept_label));

  g_dbus_proxy_call (shell_dialog->access_proxy,
                     "AccessDialog",
                     g_variant_new ("(osssssa{sv})",
                                    handle_path,
                                    "",
                                    "",
                                    title,
                                    description,
                                    "",
                                    &builder),
                     G_DBUS_CALL_FLAGS_NONE,
                     G_MAXINT,
                     shell_dialog->cancellable,
                     on_shell_access_dialog_finished,
                     shell_dialog);

  handle_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                NULL,
                                                SHELL_NAME,
                                                handle_path,
                                                SHELL_PORTAL_REQUEST_INTERFACE,
                                                shell_dialog->cancellable,
                                                &error);
  if (!handle_proxy)
    {
      g_warning ("Could not connect to the shell Request interface: %s",
                 error->message);
      return;
    }

  g_clear_object (&shell_dialog->handle_proxy);
  shell_dialog->handle_proxy = g_steal_pointer (&handle_proxy);
}

GrdShellDialog *
grd_shell_dialog_new (GCancellable *cancellable)
{
  g_autoptr (GError) error = NULL;
  GrdShellDialog *shell_dialog;
  GDBusProxy *access_proxy;

  access_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                NULL,
                                                SHELL_NAME,
                                                SHELL_PORTAL_DESKTOP_PATH,
                                                SHELL_PORTAL_ACCESS_INTERFACE,
                                                cancellable,
                                                &error);
  if (!access_proxy)
    {
      g_message ("Could not connect to the shell Access interface: %s",
                 error->message);
      return NULL;
    }

  shell_dialog = g_object_new (GRD_TYPE_SHELL_DIALOG, NULL);
  shell_dialog->cancellable = cancellable;
  shell_dialog->access_proxy = access_proxy;

  return shell_dialog;
}

static void
grd_shell_dialog_dispose (GObject *object)
{
  GrdShellDialog *shell_dialog = GRD_SHELL_DIALOG (object);

  ensure_dialog_closed_sync (shell_dialog);

  g_clear_object (&shell_dialog->access_proxy);

  G_OBJECT_CLASS (grd_shell_dialog_parent_class)->dispose (object);
}

static void
grd_shell_dialog_init (GrdShellDialog *shell_dialog)
{
}

static void
grd_shell_dialog_class_init (GrdShellDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_shell_dialog_dispose;

  signals[ACCEPTED] = g_signal_new ("accepted",
                                    G_TYPE_FROM_CLASS (klass),
                                    G_SIGNAL_RUN_LAST,
                                    0,
                                    NULL, NULL, NULL,
                                    G_TYPE_NONE, 0);
  signals[CANCELLED] = g_signal_new ("cancelled",
                                     G_TYPE_FROM_CLASS (klass),
                                     G_SIGNAL_RUN_LAST,
                                     0,
                                     NULL, NULL, NULL,
                                     G_TYPE_NONE, 0);
}
