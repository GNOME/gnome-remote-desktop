/*
 * Copyright (C) 2015 Red Hat Inc.
 * Copyright (C) 2020-2021 Pascal Nowack
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
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#include "config.h"

#include "grd-session.h"

#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib-unix.h>
#include <sys/mman.h>

#include "grd-clipboard.h"
#include "grd-dbus-remote-desktop.h"
#include "grd-context.h"
#include "grd-private.h"
#include "grd-stream.h"

enum
{
  PROP_0,

  PROP_CONTEXT,
};

enum
{
  STOPPED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

typedef enum _ScreenCastType
{
  SCREEN_CAST_TYPE_NONE,
  SCREEN_CAST_TYPE_MONITOR,
  SCREEN_CAST_TYPE_WINDOW,
  SCREEN_CAST_TYPE_AREA,
  SCREEN_CAST_TYPE_VIRTUAL,
} ScreenCastType;

typedef struct _AsyncDBusRecordCallContext
{
  GrdSession *session;
  ScreenCastType screen_cast_type;
} AsyncDBusRecordCallContext;

typedef struct _GrdSessionPrivate
{
  GrdContext *context;

  GrdDBusRemoteDesktopSession *remote_desktop_session;
  GrdDBusScreenCastSession *screen_cast_session;

  GrdStream *stream;
  GHashTable *stream_table;

  GrdClipboard *clipboard;

  GCancellable *cancellable;

  gboolean started;

  gulong caps_lock_state_changed_id;
  gulong num_lock_state_changed_id;
} GrdSessionPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdSession, grd_session, G_TYPE_OBJECT)

GrdContext *
grd_session_get_context (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  return priv->context;
}

void
grd_session_stop (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GHashTableIter iter;
  GrdStream *stream;

  if (priv->cancellable && g_cancellable_is_cancelled (priv->cancellable))
    return;

  GRD_SESSION_GET_CLASS (session)->stop (session);

  if (priv->remote_desktop_session && priv->started)
    {
      GrdDBusRemoteDesktopSession *proxy = priv->remote_desktop_session;
      GError *error = NULL;

      if (!grd_dbus_remote_desktop_session_call_stop_sync (proxy, NULL, &error))
        {
          g_warning ("Failed to stop: %s\n", error->message);
          g_error_free (error);
        }
    }

  if (priv->cancellable)
    g_cancellable_cancel (priv->cancellable);

  g_hash_table_iter_init (&iter, priv->stream_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &stream))
    grd_stream_disconnect_proxy_signals (stream);

  g_clear_signal_handler (&priv->caps_lock_state_changed_id,
                          priv->remote_desktop_session);
  g_clear_signal_handler (&priv->num_lock_state_changed_id,
                          priv->remote_desktop_session);

  if (priv->stream)
    grd_stream_disconnect_proxy_signals (priv->stream);

  g_clear_object (&priv->remote_desktop_session);
  g_clear_object (&priv->screen_cast_session);

  g_signal_emit (session, signals[STOPPED], 0);
}

static void
on_stream_ready (GrdStream  *stream,
                 GrdSession *session)
{
  GRD_SESSION_GET_CLASS (session)->stream_ready (session, stream);
}

static void
on_stream_closed (GrdStream  *stream,
                  GrdSession *session)
{
  grd_session_stop (session);
}

static void
on_screen_cast_stream_start_finished (GObject      *object,
                                      GAsyncResult *result,
                                      gpointer      user_data)
{
  GrdDBusScreenCastStream *stream_proxy = GRD_DBUS_SCREEN_CAST_STREAM (object);
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_screen_cast_stream_call_start_finish (stream_proxy,
                                                      result,
                                                      &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to start screen cast stream: %s", error->message);
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }
}

static void
on_screen_cast_stream_proxy_acquired (GObject      *object,
                                      GAsyncResult *result,
                                      gpointer      user_data)
{
  GrdDBusScreenCastStream *stream_proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  g_autoptr (GError) error = NULL;
  GrdStream *stream;

  stream_proxy = grd_dbus_screen_cast_stream_proxy_new_finish (result, &error);
  if (!stream_proxy)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to acquire stream proxy: %s", error->message);
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  session = GRD_SESSION (user_data);
  priv = grd_session_get_instance_private (session);

  stream = grd_stream_new (priv->context, stream_proxy);
  g_signal_connect (stream, "ready", G_CALLBACK (on_stream_ready),
                    session);
  g_signal_connect (stream, "closed", G_CALLBACK (on_stream_closed),
                    session);

  g_hash_table_add (priv->stream_table, stream);

  grd_dbus_screen_cast_stream_call_start (stream_proxy,
                                          priv->cancellable,
                                          on_screen_cast_stream_start_finished,
                                          session);
}

static const char *
screen_cast_type_to_string (ScreenCastType type)
{
  switch (type)
    {
    case SCREEN_CAST_TYPE_NONE:
      return "none";
    case SCREEN_CAST_TYPE_MONITOR:
      return "monitor";
    case SCREEN_CAST_TYPE_WINDOW:
      return "window";
    case SCREEN_CAST_TYPE_AREA:
      return "area";
    case SCREEN_CAST_TYPE_VIRTUAL:
      return "virtual";
    }

  g_assert_not_reached ();
}

static void
on_record_finished (GObject      *object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  GrdDBusScreenCastSession *proxy = GRD_DBUS_SCREEN_CAST_SESSION (object);
  g_autofree AsyncDBusRecordCallContext *async_context = user_data;
  ScreenCastType screen_cast_type = async_context->screen_cast_type;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GDBusConnection *connection;
  g_autofree char *stream_path = NULL;
  g_autoptr (GError) error = NULL;
  gboolean retval = FALSE;

  switch (screen_cast_type)
    {
    case SCREEN_CAST_TYPE_NONE:
      g_assert_not_reached ();
      break;
    case SCREEN_CAST_TYPE_MONITOR:
      retval = grd_dbus_screen_cast_session_call_record_monitor_finish (
        proxy, &stream_path, result, &error);
      break;
    case SCREEN_CAST_TYPE_WINDOW:
    case SCREEN_CAST_TYPE_AREA:
      g_assert_not_reached ();
      break;
    case SCREEN_CAST_TYPE_VIRTUAL:
      retval = grd_dbus_screen_cast_session_call_record_virtual_finish (
        proxy, &stream_path, result, &error);
      break;
    }

  if (!retval)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to record %s: %s",
                 screen_cast_type_to_string (screen_cast_type), error->message);
      grd_session_stop (async_context->session);
      return;
    }

  session = async_context->session;
  priv = grd_session_get_instance_private (session);
  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy));

  grd_dbus_screen_cast_stream_proxy_new (connection,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         MUTTER_SCREEN_CAST_BUS_NAME,
                                         stream_path,
                                         priv->cancellable,
                                         on_screen_cast_stream_proxy_acquired,
                                         session);
}

void
grd_session_record_monitor (GrdSession              *session,
                            const char              *connector,
                            GrdScreenCastCursorMode  cursor_mode)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GVariantBuilder properties_builder;
  AsyncDBusRecordCallContext *async_context;

  async_context = g_malloc0 (sizeof (AsyncDBusRecordCallContext));
  async_context->session = session;
  async_context->screen_cast_type = SCREEN_CAST_TYPE_MONITOR;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "cursor-mode", g_variant_new_uint32 (cursor_mode));

  grd_dbus_screen_cast_session_call_record_monitor (priv->screen_cast_session,
                                                    connector ? connector : "",
                                                    g_variant_builder_end (&properties_builder),
                                                    priv->cancellable,
                                                    on_record_finished,
                                                    async_context);
}

void
grd_session_record_virtual (GrdSession              *session,
                            GrdScreenCastCursorMode  cursor_mode,
                            gboolean                 is_platform)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GVariantBuilder properties_builder;
  AsyncDBusRecordCallContext *async_context;

  async_context = g_malloc0 (sizeof (AsyncDBusRecordCallContext));
  async_context->session = session;
  async_context->screen_cast_type = SCREEN_CAST_TYPE_VIRTUAL;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "cursor-mode", g_variant_new_uint32 (cursor_mode));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "is-platform", g_variant_new_boolean (is_platform));

  grd_dbus_screen_cast_session_call_record_virtual (priv->screen_cast_session,
                                                    g_variant_builder_end (&properties_builder),
                                                    priv->cancellable,
                                                    on_record_finished,
                                                    async_context);
}

void
grd_session_notify_keyboard_keycode (GrdSession  *session,
                                     uint32_t     keycode,
                                     GrdKeyState  state)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *session_proxy = priv->remote_desktop_session;

  grd_dbus_remote_desktop_session_call_notify_keyboard_keycode (session_proxy,
                                                                keycode,
                                                                state,
                                                                NULL,
                                                                NULL,
                                                                NULL);
}

void
grd_session_notify_keyboard_keysym (GrdSession *session,
                                    uint32_t    keysym,
                                    GrdKeyState state)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *session_proxy = priv->remote_desktop_session;

  grd_dbus_remote_desktop_session_call_notify_keyboard_keysym (session_proxy,
                                                               keysym,
                                                               state,
                                                               NULL,
                                                               NULL,
                                                               NULL);
}

void
grd_session_notify_pointer_button (GrdSession     *session,
                                   int32_t         button,
                                   GrdButtonState  state)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *session_proxy = priv->remote_desktop_session;

  grd_dbus_remote_desktop_session_call_notify_pointer_button (session_proxy,
                                                              button,
                                                              state,
                                                              NULL,
                                                              NULL,
                                                              NULL);
}

void
grd_session_notify_pointer_axis (GrdSession          *session,
                                 double               dx,
                                 double               dy,
                                 GrdPointerAxisFlags  flags)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *session_proxy = priv->remote_desktop_session;

  grd_dbus_remote_desktop_session_call_notify_pointer_axis (
    session_proxy, dx, dy, flags, NULL, NULL, NULL);
}

void
grd_session_notify_pointer_axis_discrete (GrdSession    *session,
                                          GrdPointerAxis axis,
                                          int            steps)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *session_proxy = priv->remote_desktop_session;

  grd_dbus_remote_desktop_session_call_notify_pointer_axis_discrete (
    session_proxy, axis, steps, NULL, NULL, NULL);
}

void
grd_session_notify_pointer_motion_absolute (GrdSession *session,
                                            double      x,
                                            double      y)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  const char *stream_path;

  if (!priv->stream)
    return;

  stream_path = grd_stream_get_object_path (priv->stream);

  grd_dbus_remote_desktop_session_call_notify_pointer_motion_absolute (
    priv->remote_desktop_session, stream_path, x, y, NULL, NULL, NULL);
}

static GVariant *
serialize_mime_type_tables (GList *mime_type_tables)
{
  GVariantBuilder builder;
  GList *l;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
  for (l = mime_type_tables; l; l = l->next)
    {
      GrdMimeTypeTable *mime_type_table = l->data;
      GrdMimeType mime_type;
      const char *mime_type_string;

      mime_type = mime_type_table->mime_type;
      mime_type_string = grd_mime_type_to_string (mime_type);
      g_variant_builder_add (&builder, "s", mime_type_string);
    }

  return g_variant_builder_end (&builder);
}

static GVariant *
serialize_clipboard_options (GList *mime_type_tables)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  if (mime_type_tables)
    {
      g_variant_builder_add (&builder, "{sv}", "mime-types",
                             serialize_mime_type_tables (mime_type_tables));
    }

  return g_variant_builder_end (&builder);
}

gboolean
grd_session_enable_clipboard (GrdSession   *session,
                              GrdClipboard *clipboard,
                              GList        *mime_type_tables)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GVariant *options_variant;
  g_autoptr (GError) error = NULL;

  if (!priv->remote_desktop_session)
    return FALSE;

  priv->clipboard = clipboard;

  options_variant = serialize_clipboard_options (mime_type_tables);
  if (!grd_dbus_remote_desktop_session_call_enable_clipboard_sync (
         priv->remote_desktop_session, options_variant, NULL, &error))
    {
      g_warning ("Failed to enable clipboard: %s", error->message);
      return FALSE;
    }

  return TRUE;
}

void
grd_session_disable_clipboard (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  priv->clipboard = NULL;
  if (!priv->remote_desktop_session)
    return;

  grd_dbus_remote_desktop_session_call_disable_clipboard (
    priv->remote_desktop_session, NULL, NULL, NULL);
}

void
grd_session_set_selection (GrdSession *session,
                           GList      *mime_type_tables)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GVariant *options_variant;
  g_autoptr (GError) error = NULL;

  options_variant = serialize_clipboard_options (mime_type_tables);

  if (!grd_dbus_remote_desktop_session_call_set_selection_sync (
         priv->remote_desktop_session, options_variant, NULL, &error))
    g_warning ("Failed to set selection: %s", error->message);
}

static int
acquire_fd_from_list (GUnixFDList  *fd_list,
                      int           fd_idx,
                      GError      **error)
{
  int fd;
  int fd_flags;

  fd = g_unix_fd_list_get (fd_list, fd_idx, error);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "fcntl: %s", g_strerror (errno));
      return -1;
    }

  fd_flags = fcntl (fd, F_GETFD);
  if (fd_flags == -1)
    {
      close (fd);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "fcntl: %s", g_strerror (errno));
      return -1;
    }

  if (fcntl (fd, F_SETFD, fd_flags | FD_CLOEXEC) == -1)
    {
      close (fd);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "fcntl: %s", g_strerror (errno));
      return -1;
    }

  return fd;
}

void
grd_session_selection_write (GrdSession    *session,
                             unsigned int   serial,
                             const uint8_t *data,
                             uint32_t       size)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) fd_variant = NULL;
  g_autoptr (GUnixFDList) fd_list = NULL;
  int fd_idx;
  int fd;

  if (!data || !size)
    {
      grd_dbus_remote_desktop_session_call_selection_write_done (
        priv->remote_desktop_session, serial, FALSE, NULL, NULL, NULL);
      return;
    }

  if (!grd_dbus_remote_desktop_session_call_selection_write_sync (
         priv->remote_desktop_session, serial, NULL, &fd_variant, &fd_list,
         NULL, &error))
    {
      g_warning ("Failed to write selection for serial %u: %s",
                 serial, error->message);
      return;
    }

  g_variant_get (fd_variant, "h", &fd_idx);
  fd = acquire_fd_from_list (fd_list, fd_idx, &error);
  if (fd == -1)
    {
      g_warning ("Failed to acquire file descriptor for serial %u: %s",
                 serial, error->message);
      return;
    }

  if (write (fd, data, size) < 0)
    {
      grd_dbus_remote_desktop_session_call_selection_write_done (
        priv->remote_desktop_session, serial, FALSE, NULL, NULL, NULL);

      close (fd);
      return;
    }

  grd_dbus_remote_desktop_session_call_selection_write_done (
    priv->remote_desktop_session, serial, TRUE, NULL, NULL, NULL);

  close (fd);
}

int
grd_session_selection_read (GrdSession  *session,
                            GrdMimeType  mime_type)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) fd_variant = NULL;
  g_autoptr (GUnixFDList) fd_list = NULL;
  int fd_idx;
  int fd;
  const char *mime_type_string;

  mime_type_string = grd_mime_type_to_string (mime_type);
  if (!grd_dbus_remote_desktop_session_call_selection_read_sync (
         priv->remote_desktop_session, mime_type_string, NULL, &fd_variant,
         &fd_list, NULL, &error))
    {
      g_warning ("Failed to read selection: %s", error->message);
      return -1;
    }

  g_variant_get (fd_variant, "h", &fd_idx);
  fd = acquire_fd_from_list (fd_list, fd_idx, &error);
  if (fd == -1)
    {
      g_warning ("Failed to acquire file descriptor: %s", error->message);
      return -1;
    }

  return fd;
}

static void
on_session_start_finished (GObject      *object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  GrdDBusRemoteDesktopSession *proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GrdSessionClass *klass;
  g_autoptr (GError) error = NULL;

  proxy = GRD_DBUS_REMOTE_DESKTOP_SESSION (object);
  if (!grd_dbus_remote_desktop_session_call_start_finish (proxy,
                                                          result,
                                                          &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to start session: %s", error->message);
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  session = GRD_SESSION (user_data);
  priv = grd_session_get_instance_private (session);
  klass = GRD_SESSION_GET_CLASS (session);

  priv->started = TRUE;

  if (klass->remote_desktop_session_started)
    klass->remote_desktop_session_started (session);
}

static void
start_session (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktopSession *proxy = priv->remote_desktop_session;

  grd_dbus_remote_desktop_session_call_start (proxy,
                                              priv->cancellable,
                                              on_session_start_finished,
                                              session);
}

static void
on_screen_cast_stream_proxy_acquired_ex (GObject      *object,
                                         GAsyncResult *result,
                                         gpointer      user_data)
{
  GrdDBusScreenCastStream *stream_proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  g_autoptr (GError) error = NULL;
  GrdStream *stream;

  stream_proxy = grd_dbus_screen_cast_stream_proxy_new_finish (result, &error);
  if (!stream_proxy)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to acquire stream proxy: %s", error->message);
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  session = GRD_SESSION (user_data);
  priv = grd_session_get_instance_private (session);

  stream = grd_stream_new (priv->context, stream_proxy);
  g_signal_connect (stream, "ready", G_CALLBACK (on_stream_ready),
                    session);
  g_signal_connect (stream, "closed", G_CALLBACK (on_stream_closed),
                    session);
  priv->stream = stream;

  start_session (session);
}

static void
on_record_monitor_finished (GObject      *object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  GrdDBusScreenCastSession *proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GDBusConnection *connection;
  g_autofree char *stream_path = NULL;
  g_autoptr (GError) error = NULL;

  proxy = GRD_DBUS_SCREEN_CAST_SESSION (object);
  if (!grd_dbus_screen_cast_session_call_record_monitor_finish (proxy,
                                                                &stream_path,
                                                                result,
                                                                &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to record monitor: %s", error->message);
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  session = GRD_SESSION (user_data);
  priv = grd_session_get_instance_private (session);

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy));
  grd_dbus_screen_cast_stream_proxy_new (connection,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         MUTTER_SCREEN_CAST_BUS_NAME,
                                         stream_path,
                                         priv->cancellable,
                                         on_screen_cast_stream_proxy_acquired_ex,
                                         session);
}

static void
on_screen_cast_session_proxy_acquired (GObject      *object,
                                       GAsyncResult *result,
                                       gpointer      user_data)
{
  GrdDBusScreenCastSession *session_proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GVariantBuilder properties_builder;
  g_autoptr (GError) error = NULL;

  session_proxy =
    grd_dbus_screen_cast_session_proxy_new_finish (result, &error);
  if (!session_proxy)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to acquire screen cast session proxy: %s\n",
                 error->message);
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  session = GRD_SESSION (user_data);
  priv = grd_session_get_instance_private (session);

  priv->screen_cast_session = session_proxy;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "cursor-mode",
                         g_variant_new_uint32 (GRD_SCREEN_CAST_CURSOR_MODE_METADATA));

  /* TODO: Support something other than primary monitor */
  grd_dbus_screen_cast_session_call_record_monitor (session_proxy,
                                                    "",
                                                    g_variant_builder_end (&properties_builder),
                                                    priv->cancellable,
                                                    on_record_monitor_finished,
                                                    session);
}

static void
on_screen_cast_session_created (GObject      *source_object,
                                GAsyncResult *res,
                                gpointer      user_data)
{
  GrdDBusScreenCast *screen_cast_proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GDBusConnection *connection;
  g_autofree char *session_path = NULL;
  g_autoptr (GError) error = NULL;

  screen_cast_proxy = GRD_DBUS_SCREEN_CAST (source_object);
  if (!grd_dbus_screen_cast_call_create_session_finish (screen_cast_proxy,
                                                        &session_path,
                                                        res,
                                                        &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to start screen cast session: %s\n", error->message);
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  session = GRD_SESSION (user_data);
  priv = grd_session_get_instance_private (session);
  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (screen_cast_proxy));

  grd_dbus_screen_cast_session_proxy_new (connection,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          MUTTER_SCREEN_CAST_BUS_NAME,
                                          session_path,
                                          priv->cancellable,
                                          on_screen_cast_session_proxy_acquired,
                                          session);
}

static void
on_remote_desktop_session_closed (GrdDBusRemoteDesktopSession *session_proxy,
                                  GrdSession                  *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  g_clear_object (&priv->remote_desktop_session);
  g_clear_object (&priv->screen_cast_session);

  grd_session_stop (session);
}

static void
on_remote_desktop_session_selection_owner_changed (GrdDBusRemoteDesktopSession *session_proxy,
                                                   GVariant                    *options_variant,
                                                   GrdSession                  *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GVariant *is_owner_variant, *mime_types_variant;
  GVariantIter iter;
  const char *mime_string;
  GrdMimeType mime_type;
  GList *mime_type_list = NULL;

  if (!priv->clipboard)
    return;

  is_owner_variant = g_variant_lookup_value (options_variant, "session-is-owner",
                                             G_VARIANT_TYPE ("b"));
  if (is_owner_variant && g_variant_get_boolean (is_owner_variant))
    return;

  mime_types_variant = g_variant_lookup_value (options_variant, "mime-types",
                                               G_VARIANT_TYPE ("(as)"));
  if (!mime_types_variant)
    return;

  g_variant_iter_init (&iter, g_variant_get_child_value (mime_types_variant, 0));
  while (g_variant_iter_loop (&iter, "s", &mime_string))
    {
      mime_type = grd_mime_type_from_string (mime_string);
      if (mime_type != GRD_MIME_TYPE_NONE)
        {
          mime_type_list = g_list_append (mime_type_list,
                                          GUINT_TO_POINTER (mime_type));
          g_debug ("Clipboard[SelectionOwnerChanged]: Server advertises mime "
                   "type %s", mime_string);
        }
      else
        {
          g_debug ("Clipboard[SelectionOwnerChanged]: Server advertised unknown "
                   "mime type: %s", mime_string);
        }
    }

  if (mime_type_list)
    grd_clipboard_update_client_mime_type_list (priv->clipboard, mime_type_list);
}

static void
on_remote_desktop_session_selection_transfer (GrdDBusRemoteDesktopSession *session_proxy,
                                              char                        *mime_type_string,
                                              unsigned int                 serial,
                                              GrdSession                  *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdMimeType mime_type;

  if (!priv->clipboard)
    return;

  mime_type = grd_mime_type_from_string (mime_type_string);
  if (mime_type == GRD_MIME_TYPE_NONE)
    {
      grd_dbus_remote_desktop_session_call_selection_write_done (
        priv->remote_desktop_session, serial, FALSE, NULL, NULL, NULL);
      return;
    }

  grd_clipboard_request_client_content_for_mime_type (priv->clipboard,
                                                      mime_type, serial);
}

static void
on_caps_lock_state_changed (GrdDBusRemoteDesktopSession *session_proxy,
                            GParamSpec                  *param_spec,
                            GrdSession                  *session)
{
  GrdSessionClass *klass = GRD_SESSION_GET_CLASS (session);
  gboolean state;

  state = grd_dbus_remote_desktop_session_get_caps_lock_state (session_proxy);
  g_debug ("Caps lock state: %s", state ? "locked" : "unlocked");

  if (klass->on_caps_lock_state_changed)
    klass->on_caps_lock_state_changed (session, state);
}

static void
on_num_lock_state_changed (GrdDBusRemoteDesktopSession *session_proxy,
                           GParamSpec                  *param_spec,
                           GrdSession                  *session)
{
  GrdSessionClass *klass = GRD_SESSION_GET_CLASS (session);
  gboolean state;

  state = grd_dbus_remote_desktop_session_get_num_lock_state (session_proxy);
  g_debug ("Num lock state: %s", state ? "locked" : "unlocked");

  if (klass->on_num_lock_state_changed)
    klass->on_num_lock_state_changed (session, state);
}

static void
on_remote_desktop_session_proxy_acquired (GObject      *object,
                                          GAsyncResult *result,
                                          gpointer      user_data)
{
  GrdDBusRemoteDesktopSession *session_proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GrdSessionClass *klass;
  g_autoptr (GError) error = NULL;
  const char *remote_desktop_session_id;
  GrdDBusScreenCast *screen_cast_proxy;
  GVariantBuilder properties_builder;
  GVariant *properties_variant;

  session_proxy =
    grd_dbus_remote_desktop_session_proxy_new_finish (result, &error);
  if (!session_proxy)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to acquire remote desktop session proxy: %s\n",
                 error->message);
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  session = GRD_SESSION (user_data);
  priv = grd_session_get_instance_private (session);
  klass = GRD_SESSION_GET_CLASS (session);

  g_signal_connect (session_proxy, "closed",
                    G_CALLBACK (on_remote_desktop_session_closed),
                    session);
  g_signal_connect (session_proxy, "selection-owner-changed",
                    G_CALLBACK (on_remote_desktop_session_selection_owner_changed),
                    session);
  g_signal_connect (session_proxy, "selection-transfer",
                    G_CALLBACK (on_remote_desktop_session_selection_transfer),
                    session);

  priv->remote_desktop_session = session_proxy;

  remote_desktop_session_id =
    grd_dbus_remote_desktop_session_get_session_id (session_proxy);

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "remote-desktop-session-id",
                         g_variant_new_string (remote_desktop_session_id));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "disable-animations",
                         g_variant_new_boolean (TRUE));
  properties_variant = g_variant_builder_end (&properties_builder);

  screen_cast_proxy = grd_context_get_screen_cast_proxy (priv->context);
  grd_dbus_screen_cast_call_create_session (screen_cast_proxy,
                                            properties_variant,
                                            priv->cancellable,
                                            on_screen_cast_session_created,
                                            session);

  priv->caps_lock_state_changed_id =
    g_signal_connect (session_proxy, "notify::caps-lock-state",
                      G_CALLBACK (on_caps_lock_state_changed),
                      session);
  priv->num_lock_state_changed_id =
    g_signal_connect (session_proxy, "notify::num-lock-state",
                      G_CALLBACK (on_num_lock_state_changed),
                      session);

  if (klass->remote_desktop_session_ready)
    klass->remote_desktop_session_ready (session);

  on_caps_lock_state_changed (session_proxy, NULL, session);
  on_num_lock_state_changed (session_proxy, NULL, session);
}

static void
on_remote_desktop_session_created (GObject      *source_object,
                                   GAsyncResult *res,
                                   gpointer      user_data)
{
  GrdDBusRemoteDesktop *remote_desktop_proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GDBusConnection *connection;
  g_autofree char *session_path = NULL;
  g_autoptr (GError) error = NULL;

  remote_desktop_proxy = GRD_DBUS_REMOTE_DESKTOP (source_object);
  if (!grd_dbus_remote_desktop_call_create_session_finish (remote_desktop_proxy,
                                                           &session_path,
                                                           res,
                                                           &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to start remote desktop session: %s\n", error->message);
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  session = GRD_SESSION (user_data);
  priv = grd_session_get_instance_private (session);
  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (remote_desktop_proxy));

  grd_dbus_remote_desktop_session_proxy_new (connection,
                                             G_DBUS_PROXY_FLAGS_NONE,
                                             MUTTER_REMOTE_DESKTOP_BUS_NAME,
                                             session_path,
                                             priv->cancellable,
                                             on_remote_desktop_session_proxy_acquired,
                                             session);
}

void
grd_session_start (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdDBusRemoteDesktop *remote_desktop_proxy;

  priv->cancellable = g_cancellable_new ();

  remote_desktop_proxy = grd_context_get_remote_desktop_proxy (priv->context);
  grd_dbus_remote_desktop_call_create_session (remote_desktop_proxy,
                                               priv->cancellable,
                                               on_remote_desktop_session_created,
                                               session);
}

static void
grd_session_dispose (GObject *object)
{
  GrdSession *session = GRD_SESSION (object);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  g_clear_object (&priv->stream);

  g_hash_table_remove_all (priv->stream_table);

  G_OBJECT_CLASS (grd_session_parent_class)->dispose (object);
}

static void
grd_session_finalize (GObject *object)
{
  GrdSession *session = GRD_SESSION (object);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  g_assert (!priv->remote_desktop_session);

  if (priv->cancellable)
    g_assert (g_cancellable_is_cancelled (priv->cancellable));
  g_clear_object (&priv->cancellable);

  g_assert (g_hash_table_size (priv->stream_table) == 0);
  g_clear_pointer (&priv->stream_table, g_hash_table_unref);

  G_OBJECT_CLASS (grd_session_parent_class)->finalize (object);
}

static void
grd_session_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  GrdSession *session = GRD_SESSION (object);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      priv->context = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_session_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  GrdSession *session = GRD_SESSION (object);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, priv->context);

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_session_init (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  priv->stream_table = g_hash_table_new_full (NULL, NULL, NULL, g_object_unref);
}

static void
grd_session_class_init (GrdSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_session_dispose;
  object_class->finalize = grd_session_finalize;
  object_class->set_property = grd_session_set_property;
  object_class->get_property = grd_session_get_property;

  g_object_class_install_property (object_class,
                                   PROP_CONTEXT,
                                   g_param_spec_object ("context",
                                                        "GrdContext",
                                                        "The GrdContext instance",
                                                        GRD_TYPE_CONTEXT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  signals[STOPPED] = g_signal_new ("stopped",
                                   G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST,
                                   0,
                                   NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}
