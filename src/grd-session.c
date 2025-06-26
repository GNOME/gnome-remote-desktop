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
#include <libei.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>

#include "grd-clipboard.h"
#include "grd-dbus-mutter-remote-desktop.h"
#include "grd-context.h"
#include "grd-private.h"
#include "grd-stream.h"
#include "grd-utils.h"

enum
{
  PROP_0,

  PROP_CONTEXT,
};

enum
{
  STOPPED,
  TOUCH_DEVICE_ADDED,
  TOUCH_DEVICE_REMOVED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef enum _EvdevButtonType
{
  EVDEV_BUTTON_TYPE_NONE,
  EVDEV_BUTTON_TYPE_KEY,
  EVDEV_BUTTON_TYPE_BUTTON,
} EvdevButtonType;

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
  uint32_t stream_id;
  ScreenCastType screen_cast_type;
} AsyncDBusRecordCallContext;

typedef struct _GrdRegion
{
  struct ei_device *ei_device;
  struct ei_region *ei_region;
} GrdRegion;

struct _GrdTouchContact
{
  struct ei_touch *ei_touch_contact;
};

typedef struct _GrdEiPing
{
  GTask *task;
  struct ei_ping *ping;
} GrdEiPing;

typedef struct _GrdSessionPrivate
{
  GrdContext *context;

  GrdDBusMutterRemoteDesktopSession *remote_desktop_session;
  GrdDBusMutterScreenCastSession *screen_cast_session;

  GrdClipboard *clipboard;

  struct ei *ei;
  struct ei_seat *ei_seat;
  struct ei_device *ei_pointer;
  struct ei_device *ei_abs_pointer;
  struct ei_device *ei_keyboard;
  struct ei_device *ei_touch;
  uint32_t ei_sequence;
  GSource *ei_source;

  GHashTable *abs_pointer_regions;
  GHashTable *touch_regions;

  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;

  GCancellable *cancellable;

  gboolean started;

  gboolean locked_modifier_valid;
  gboolean caps_lock_state;
  gboolean num_lock_state;
  gulong caps_lock_state_changed_id;
  gulong num_lock_state_changed_id;

  GHashTable *pings;
} GrdSessionPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdSession, grd_session, G_TYPE_OBJECT)

static void
finish_and_free_ping (GrdEiPing *ping);

GrdContext *
grd_session_get_context (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  return priv->context;
}

static void
clear_ei (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  g_clear_pointer (&priv->xkb_state, xkb_state_unref);
  g_clear_pointer (&priv->xkb_keymap, xkb_keymap_unref);
  g_clear_pointer (&priv->xkb_context, xkb_context_unref);

  g_clear_pointer (&priv->touch_regions, g_hash_table_unref);
  g_clear_pointer (&priv->abs_pointer_regions, g_hash_table_unref);

  g_clear_pointer (&priv->ei_touch, ei_device_unref);
  g_clear_pointer (&priv->ei_keyboard, ei_device_unref);
  g_clear_pointer (&priv->ei_abs_pointer, ei_device_unref);
  g_clear_pointer (&priv->ei_pointer, ei_device_unref);
  g_clear_pointer (&priv->ei_seat, ei_seat_unref);
  g_clear_pointer (&priv->ei_source, g_source_destroy);
  g_clear_pointer (&priv->ei, ei_unref);
}

static void
clear_session (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  clear_ei (session);

  g_clear_signal_handler (&priv->caps_lock_state_changed_id,
                          priv->remote_desktop_session);
  g_clear_signal_handler (&priv->num_lock_state_changed_id,
                          priv->remote_desktop_session);

  g_clear_object (&priv->remote_desktop_session);
  g_clear_object (&priv->screen_cast_session);
}

void
grd_session_stop (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  if (priv->cancellable && g_cancellable_is_cancelled (priv->cancellable))
    return;

  GRD_SESSION_GET_CLASS (session)->stop (session);

  clear_ei (session);

  if (priv->remote_desktop_session && priv->started)
    {
      GrdDBusMutterRemoteDesktopSession *proxy = priv->remote_desktop_session;
      g_autoptr (GError) error = NULL;

      if (!grd_dbus_mutter_remote_desktop_session_call_stop_sync (proxy, NULL, &error))
        {
          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CLOSED) &&
              !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
            g_warning ("Failed to stop: %s", error->message);
        }
    }

  if (priv->cancellable)
    g_cancellable_cancel (priv->cancellable);

  clear_session (session);

  g_signal_emit (session, signals[STOPPED], 0);
}

gboolean
grd_session_flush_input_finish (GrdSession    *session,
                                GAsyncResult  *result,
                                GError       **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

void
grd_session_flush_input_async (GrdSession          *session,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdEiPing *ping;

  ping = g_new0 (GrdEiPing, 1);
  ping->task = g_task_new (session,
                           cancellable,
                           callback,
                           user_data);
  ping->ping = ei_new_ping (priv->ei);
  ei_ping (ping->ping);

  g_hash_table_insert (priv->pings, ping->ping, ping);
}

static void
on_screen_cast_stream_start_finished (GObject      *object,
                                      GAsyncResult *result,
                                      gpointer      user_data)
{
  GrdDBusMutterScreenCastStream *stream_proxy = GRD_DBUS_MUTTER_SCREEN_CAST_STREAM (object);
  g_autoptr (GError) error = NULL;

  if (!grd_dbus_mutter_screen_cast_stream_call_start_finish (stream_proxy,
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
  g_autofree AsyncDBusRecordCallContext *async_context = user_data;
  g_autoptr (GrdDBusMutterScreenCastStream) stream_proxy = NULL;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GrdSessionClass *klass;
  g_autoptr (GError) error = NULL;
  GrdStream *stream;

  stream_proxy = grd_dbus_mutter_screen_cast_stream_proxy_new_finish (result, &error);
  if (!stream_proxy)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to acquire stream proxy: %s", error->message);
      grd_session_stop (async_context->session);
      return;
    }

  session = async_context->session;
  priv = grd_session_get_instance_private (session);
  klass = GRD_SESSION_GET_CLASS (session);

  stream = grd_stream_new (async_context->stream_id, stream_proxy,
                           priv->cancellable, &error);
  if (!stream)
    {
      g_warning ("Failed to create stream: %s", error->message);
      grd_session_stop (async_context->session);
      return;
    }

  klass->on_stream_created (session, async_context->stream_id, stream);

  grd_dbus_mutter_screen_cast_stream_call_start (stream_proxy,
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
  GrdDBusMutterScreenCastSession *proxy = GRD_DBUS_MUTTER_SCREEN_CAST_SESSION (object);
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
      retval = grd_dbus_mutter_screen_cast_session_call_record_monitor_finish (
        proxy, &stream_path, result, &error);
      break;
    case SCREEN_CAST_TYPE_WINDOW:
    case SCREEN_CAST_TYPE_AREA:
      g_assert_not_reached ();
      break;
    case SCREEN_CAST_TYPE_VIRTUAL:
      retval = grd_dbus_mutter_screen_cast_session_call_record_virtual_finish (
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

  grd_dbus_mutter_screen_cast_stream_proxy_new (connection,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                MUTTER_SCREEN_CAST_BUS_NAME,
                                                stream_path,
                                                priv->cancellable,
                                                on_screen_cast_stream_proxy_acquired,
                                                g_steal_pointer (&async_context));
}

void
grd_session_record_monitor (GrdSession              *session,
                            uint32_t                 stream_id,
                            const char              *connector,
                            GrdScreenCastCursorMode  cursor_mode)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdSessionClass *klass = GRD_SESSION_GET_CLASS (session);
  GVariantBuilder properties_builder;
  AsyncDBusRecordCallContext *async_context;

  g_assert (klass->on_stream_created);

  async_context = g_malloc0 (sizeof (AsyncDBusRecordCallContext));
  async_context->session = session;
  async_context->stream_id = stream_id;
  async_context->screen_cast_type = SCREEN_CAST_TYPE_MONITOR;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "cursor-mode", g_variant_new_uint32 (cursor_mode));

  grd_dbus_mutter_screen_cast_session_call_record_monitor (priv->screen_cast_session,
                                                           connector ? connector : "",
                                                           g_variant_builder_end (&properties_builder),
                                                           priv->cancellable,
                                                           on_record_finished,
                                                           async_context);
}

void
grd_session_record_virtual (GrdSession              *session,
                            uint32_t                 stream_id,
                            GrdScreenCastCursorMode  cursor_mode,
                            gboolean                 is_platform)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdSessionClass *klass = GRD_SESSION_GET_CLASS (session);
  GVariantBuilder properties_builder;
  AsyncDBusRecordCallContext *async_context;

  g_assert (klass->on_stream_created);

  async_context = g_malloc0 (sizeof (AsyncDBusRecordCallContext));
  async_context->session = session;
  async_context->stream_id = stream_id;
  async_context->screen_cast_type = SCREEN_CAST_TYPE_VIRTUAL;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "cursor-mode", g_variant_new_uint32 (cursor_mode));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "is-platform", g_variant_new_boolean (is_platform));

  grd_dbus_mutter_screen_cast_session_call_record_virtual (priv->screen_cast_session,
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

  if (!priv->ei_keyboard)
    return;

  ei_device_keyboard_key (priv->ei_keyboard, keycode, state);
  ei_device_frame (priv->ei_keyboard, g_get_monotonic_time ());
}

static gboolean
pick_keycode_for_keysym_in_current_group (GrdSession *session,
                                          uint32_t    keysym,
                                          uint32_t   *keycode_out,
                                          uint32_t   *level_out)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  struct xkb_keymap *xkb_keymap;
  struct xkb_state  *xkb_state;
  uint32_t keycode, layout;
  xkb_keycode_t min_keycode, max_keycode;

  g_assert (keycode_out);

  xkb_keymap = priv->xkb_keymap;
  xkb_state = priv->xkb_state;

  layout = xkb_state_serialize_layout (xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
  min_keycode = xkb_keymap_min_keycode (xkb_keymap);
  max_keycode = xkb_keymap_max_keycode (xkb_keymap);
  for (keycode = min_keycode; keycode < max_keycode; keycode++)
    {
      int num_levels, level;

      num_levels = xkb_keymap_num_levels_for_key (xkb_keymap, keycode, layout);
      for (level = 0; level < num_levels; level++)
        {
          const xkb_keysym_t *syms;
          int num_syms, sym;

          num_syms = xkb_keymap_key_get_syms_by_level (xkb_keymap, keycode,
                                                       layout, level, &syms);
          for (sym = 0; sym < num_syms; sym++)
            {
              if (syms[sym] == keysym)
                {
                  *keycode_out = keycode;
                  if (level_out)
                    *level_out = level;
                  return TRUE;
                }
            }
        }
    }

  return FALSE;
}

static uint32_t
xkb_keycode_to_evdev (uint32_t xkb_keycode)
{
  return xkb_keycode - 8;
}

static void
apply_level_modifiers (GrdSession *session,
                       uint64_t    time_us,
                       uint32_t    level,
                       uint32_t    key_state)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  uint32_t keysym, keycode, evcode;

  if (level == 0)
    return;

  if (level == 1)
    {
      keysym = XKB_KEY_Shift_L;
    }
  else if (level == 2)
    {
      keysym = XKB_KEY_ISO_Level3_Shift;
    }
  else
    {
      g_warning ("Unhandled level: %d", level);
      return;
    }

  if (!pick_keycode_for_keysym_in_current_group (session, keysym,
                                                 &keycode, NULL))
    return;

  evcode = xkb_keycode_to_evdev (keycode);

  ei_device_keyboard_key (priv->ei_keyboard, evcode, key_state);
  ei_device_frame (priv->ei_keyboard, time_us);
}

static EvdevButtonType
get_button_type (uint16_t code)
{
  switch (code)
    {
    case BTN_TOOL_PEN:
    case BTN_TOOL_RUBBER:
    case BTN_TOOL_BRUSH:
    case BTN_TOOL_PENCIL:
    case BTN_TOOL_AIRBRUSH:
    case BTN_TOOL_MOUSE:
    case BTN_TOOL_LENS:
    case BTN_TOOL_QUINTTAP:
    case BTN_TOOL_DOUBLETAP:
    case BTN_TOOL_TRIPLETAP:
    case BTN_TOOL_QUADTAP:
    case BTN_TOOL_FINGER:
    case BTN_TOUCH:
      return EVDEV_BUTTON_TYPE_NONE;
    }

  if (code >= KEY_ESC && code <= KEY_MICMUTE)
    return EVDEV_BUTTON_TYPE_KEY;
  if (code >= BTN_MISC && code <= BTN_GEAR_UP)
    return EVDEV_BUTTON_TYPE_BUTTON;
  if (code >= KEY_OK && code <= KEY_LIGHTS_TOGGLE)
    return EVDEV_BUTTON_TYPE_KEY;
  if (code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT)
    return EVDEV_BUTTON_TYPE_BUTTON;
  if (code >= KEY_ALS_TOGGLE && code <= KEY_KBDINPUTASSIST_CANCEL)
    return EVDEV_BUTTON_TYPE_KEY;
  if (code >= BTN_TRIGGER_HAPPY && code <= BTN_TRIGGER_HAPPY40)
    return EVDEV_BUTTON_TYPE_BUTTON;
  return EVDEV_BUTTON_TYPE_NONE;
}

void
grd_session_notify_keyboard_keysym (GrdSession *session,
                                    uint32_t    keysym,
                                    GrdKeyState state)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  int64_t now_us;
  uint32_t keycode = 0, level = 0, evcode = 0;

  if (!priv->xkb_state || !priv->ei_keyboard)
    return;

  now_us = g_get_monotonic_time ();

  if (!pick_keycode_for_keysym_in_current_group (session,
                                                 keysym,
                                                 &keycode, &level))
    {
      g_warning ("No keycode found for keyval %x in current group", keysym);
      return;
    }

  evcode = xkb_keycode_to_evdev (keycode);
  if (get_button_type (evcode) != EVDEV_BUTTON_TYPE_KEY)
    {
      g_warning ("Unknown/invalid key 0x%x pressed", evcode);
      return;
    }

  if (state)
    apply_level_modifiers (session, now_us, level, state);

  ei_device_keyboard_key (priv->ei_keyboard, evcode, state);
  ei_device_frame (priv->ei_keyboard, now_us);

  if (!state)
    apply_level_modifiers (session, now_us, level, state);
}

void
grd_session_notify_pointer_button (GrdSession     *session,
                                   int32_t         button,
                                   GrdButtonState  state)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  if (!priv->ei_abs_pointer)
    return;

  ei_device_button_button (priv->ei_abs_pointer, button, state);
  ei_device_frame (priv->ei_abs_pointer, g_get_monotonic_time ());
}

void
grd_session_notify_pointer_axis (GrdSession          *session,
                                 double               dx,
                                 double               dy,
                                 GrdPointerAxisFlags  flags)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  if (!priv->ei_abs_pointer)
    return;

  if (flags & GRD_POINTER_AXIS_FLAGS_FINISH)
    ei_device_scroll_stop (priv->ei_abs_pointer, true, true);
  else
    ei_device_scroll_delta (priv->ei_abs_pointer, dx, dy);

  ei_device_frame (priv->ei_abs_pointer, g_get_monotonic_time ());
}

void
grd_session_notify_pointer_axis_discrete (GrdSession    *session,
                                          GrdPointerAxis axis,
                                          int            steps)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  if (!priv->ei_abs_pointer)
    return;

  ei_device_scroll_discrete (priv->ei_abs_pointer,
                             axis == GRD_POINTER_AXIS_HORIZONTAL ? steps * 120 : 0,
                             axis == GRD_POINTER_AXIS_VERTICAL ? steps * 120 : 0);
  ei_device_frame (priv->ei_abs_pointer, g_get_monotonic_time ());
}

void
grd_session_notify_pointer_motion (GrdSession *session,
                                   double      dx,
                                   double      dy)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  if (!priv->ei_pointer)
    return;

  ei_device_pointer_motion (priv->ei_pointer, dx, dy);
  ei_device_frame (priv->ei_pointer, g_get_monotonic_time ());
}

static gboolean
transform_position (GrdSession               *session,
                    GHashTable               *regions,
                    GrdStream                *stream,
                    const GrdEventMotionAbs  *motion_abs,
                    struct ei_device        **ei_device,
                    double                   *x,
                    double                   *y)
{
  GrdRegion *region;
  double scale_x;
  double scale_y;
  double scaled_x;
  double scaled_y;

  g_assert (motion_abs->input_rect_width > 0);
  g_assert (motion_abs->input_rect_height > 0);

  region = g_hash_table_lookup (regions, grd_stream_get_mapping_id (stream));
  if (!region)
    return FALSE;

  scale_x = ((double) motion_abs->input_rect_width) /
            ei_region_get_width (region->ei_region);
  scale_y = ((double) motion_abs->input_rect_height) /
            ei_region_get_height (region->ei_region);
  scaled_x = motion_abs->x / scale_x;
  scaled_y = motion_abs->y / scale_y;

  *ei_device = region->ei_device;
  *x = ei_region_get_x (region->ei_region) + scaled_x;
  *y = ei_region_get_y (region->ei_region) + scaled_y;

  return TRUE;
}

void
grd_session_notify_pointer_motion_absolute (GrdSession              *session,
                                            GrdStream               *stream,
                                            const GrdEventMotionAbs *motion_abs)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  struct ei_device *ei_device = NULL;
  double x = 0;
  double y = 0;

  if (!transform_position (session, priv->abs_pointer_regions,
                           stream, motion_abs, &ei_device, &x, &y))
    return;

  ei_device_pointer_motion_absolute (ei_device, x, y);
  ei_device_frame (ei_device, g_get_monotonic_time ());
}

gboolean
grd_session_has_touch_device (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  return !!priv->ei_touch;
}

GrdTouchContact *
grd_session_acquire_touch_contact (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdTouchContact *touch_contact;

  g_assert (priv->ei_touch);

  touch_contact = g_new0 (GrdTouchContact, 1);
  touch_contact->ei_touch_contact = ei_device_touch_new (priv->ei_touch);

  return touch_contact;
}

void
grd_session_release_touch_contact (GrdSession      *session,
                                   GrdTouchContact *touch_contact)
{
  g_clear_pointer (&touch_contact->ei_touch_contact, ei_touch_unref);

  g_free (touch_contact);
}

void
grd_session_notify_touch_down (GrdSession              *session,
                               const GrdTouchContact   *touch_contact,
                               GrdStream               *stream,
                               const GrdEventMotionAbs *motion_abs)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  struct ei_device *ei_device = NULL;
  double x = 0;
  double y = 0;

  if (!transform_position (session, priv->touch_regions, stream, motion_abs,
                           &ei_device, &x, &y))
    return;

  ei_touch_down (touch_contact->ei_touch_contact, x, y);
}

void
grd_session_notify_touch_motion (GrdSession              *session,
                                 const GrdTouchContact   *touch_contact,
                                 GrdStream               *stream,
                                 const GrdEventMotionAbs *motion_abs)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  struct ei_device *ei_device = NULL;
  double x = 0;
  double y = 0;

  if (!transform_position (session, priv->touch_regions, stream, motion_abs,
                           &ei_device, &x, &y))
    return;

  ei_touch_motion (touch_contact->ei_touch_contact, x, y);
}

void
grd_session_notify_touch_up (GrdSession      *session,
                             GrdTouchContact *touch_contact)
{
  ei_touch_up (touch_contact->ei_touch_contact);
}

void
grd_session_notify_touch_cancel (GrdSession      *session,
                                 GrdTouchContact *touch_contact)
{
  ei_touch_cancel (touch_contact->ei_touch_contact);
}

void
grd_session_notify_touch_device_frame (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  g_assert (priv->ei_touch);

  ei_device_frame (priv->ei_touch, g_get_monotonic_time ());
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

  options_variant = serialize_clipboard_options (mime_type_tables);
  if (!grd_dbus_mutter_remote_desktop_session_call_enable_clipboard_sync (
         priv->remote_desktop_session, options_variant, NULL, &error))
    {
      g_warning ("Failed to enable clipboard: %s", error->message);
      return FALSE;
    }
  priv->clipboard = clipboard;

  return TRUE;
}

void
grd_session_disable_clipboard (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  priv->clipboard = NULL;
  if (!priv->remote_desktop_session)
    return;

  grd_dbus_mutter_remote_desktop_session_call_disable_clipboard (
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

  if (!grd_dbus_mutter_remote_desktop_session_call_set_selection_sync (
         priv->remote_desktop_session, options_variant, NULL, &error))
    g_warning ("Failed to set selection: %s", error->message);
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
  int fd_idx = -1;
  int fd;

  if (!data || !size)
    {
      grd_dbus_mutter_remote_desktop_session_call_selection_write_done (
        priv->remote_desktop_session, serial, FALSE, NULL, NULL, NULL);
      return;
    }

  if (!grd_dbus_mutter_remote_desktop_session_call_selection_write_sync (
         priv->remote_desktop_session, serial, NULL, &fd_variant, &fd_list,
         NULL, &error))
    {
      g_warning ("Failed to write selection for serial %u: %s",
                 serial, error->message);
      return;
    }

  g_variant_get (fd_variant, "h", &fd_idx);
  if (!G_IS_UNIX_FD_LIST (fd_list) ||
      fd_idx < 0 || fd_idx >= g_unix_fd_list_get_length (fd_list))
    {
      g_warning ("Failed to acquire file descriptor for serial %u: Invalid "
                 "file descriptor list sent by display server", serial);
      return;
    }

  fd = g_unix_fd_list_get (fd_list, fd_idx, &error);
  if (fd == -1)
    {
      g_warning ("Failed to acquire file descriptor for serial %u: %s",
                 serial, error->message);
      return;
    }

  if (write (fd, data, size) < 0)
    {
      grd_dbus_mutter_remote_desktop_session_call_selection_write_done (
        priv->remote_desktop_session, serial, FALSE, NULL, NULL, NULL);

      close (fd);
      return;
    }

  grd_dbus_mutter_remote_desktop_session_call_selection_write_done (
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
  int fd_idx = -1;
  int fd;
  const char *mime_type_string;

  mime_type_string = grd_mime_type_to_string (mime_type);
  if (!grd_dbus_mutter_remote_desktop_session_call_selection_read_sync (
         priv->remote_desktop_session, mime_type_string, NULL, &fd_variant,
         &fd_list, NULL, &error))
    {
      g_warning ("Failed to read selection: %s", error->message);
      return -1;
    }

  g_variant_get (fd_variant, "h", &fd_idx);
  if (!G_IS_UNIX_FD_LIST (fd_list) ||
      fd_idx < 0 || fd_idx >= g_unix_fd_list_get_length (fd_list))
    {
      g_warning ("Failed to acquire file descriptor: Invalid file descriptor "
                 "list sent by display server");
      return -1;
    }

  fd = g_unix_fd_list_get (fd_list, fd_idx, &error);
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
  GrdDBusMutterRemoteDesktopSession *proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GrdSessionClass *klass;
  g_autoptr (GError) error = NULL;

  proxy = GRD_DBUS_MUTTER_REMOTE_DESKTOP_SESSION (object);
  if (!grd_dbus_mutter_remote_desktop_session_call_start_finish (proxy,
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
  GrdDBusMutterRemoteDesktopSession *proxy = priv->remote_desktop_session;

  grd_dbus_mutter_remote_desktop_session_call_start (proxy,
                                                     priv->cancellable,
                                                     on_session_start_finished,
                                                     session);
}

static void
on_screen_cast_session_proxy_acquired (GObject      *object,
                                       GAsyncResult *result,
                                       gpointer      user_data)
{
  GrdDBusMutterScreenCastSession *session_proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  g_autoptr (GError) error = NULL;

  session_proxy =
    grd_dbus_mutter_screen_cast_session_proxy_new_finish (result, &error);
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

  start_session (session);
}

static void
on_screen_cast_session_created (GObject      *source_object,
                                GAsyncResult *res,
                                gpointer      user_data)
{
  GrdDBusMutterScreenCast *screen_cast_proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GDBusConnection *connection;
  g_autofree char *session_path = NULL;
  g_autoptr (GError) error = NULL;

  screen_cast_proxy = GRD_DBUS_MUTTER_SCREEN_CAST (source_object);
  if (!grd_dbus_mutter_screen_cast_call_create_session_finish (screen_cast_proxy,
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

  grd_dbus_mutter_screen_cast_session_proxy_new (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 MUTTER_SCREEN_CAST_BUS_NAME,
                                                 session_path,
                                                 priv->cancellable,
                                                 on_screen_cast_session_proxy_acquired,
                                                 session);
}

static void
on_remote_desktop_session_closed (GrdDBusMutterRemoteDesktopSession *session_proxy,
                                  GrdSession                        *session)
{
  g_debug ("Remote desktop session closed, stopping session");

  clear_session (session);
  grd_session_stop (session);
}

static void
on_remote_desktop_session_selection_owner_changed (GrdDBusMutterRemoteDesktopSession *session_proxy,
                                                   GVariant                          *options_variant,
                                                   GrdSession                        *session)
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
on_remote_desktop_session_selection_transfer (GrdDBusMutterRemoteDesktopSession *session_proxy,
                                              char                              *mime_type_string,
                                              unsigned int                       serial,
                                              GrdSession                        *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  GrdMimeType mime_type;

  if (!priv->clipboard)
    return;

  mime_type = grd_mime_type_from_string (mime_type_string);
  if (mime_type == GRD_MIME_TYPE_NONE)
    {
      grd_dbus_mutter_remote_desktop_session_call_selection_write_done (
        priv->remote_desktop_session, serial, FALSE, NULL, NULL, NULL);
      return;
    }

  grd_clipboard_request_client_content_for_mime_type (priv->clipboard,
                                                      mime_type, serial);
}

static gboolean
grd_ei_source_prepare (gpointer user_data)
{
  GrdSession *session = GRD_SESSION (user_data);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  return !!ei_peek_event (priv->ei);
}

static gboolean
setup_xkb_keymap (GrdSession        *session,
                  struct ei_keymap  *keymap,
                  GError           **error)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  struct xkb_context *xkb_context = NULL;
  struct xkb_keymap *xkb_keymap = NULL;
  struct xkb_state *xkb_state = NULL;
  size_t keymap_size;
  g_autofree char *buf = NULL;

  g_clear_pointer (&priv->xkb_state, xkb_state_unref);
  g_clear_pointer (&priv->xkb_keymap, xkb_keymap_unref);
  g_clear_pointer (&priv->xkb_context, xkb_context_unref);

  xkb_context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  if (!xkb_context)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create XKB context");
      goto err;
    }

  keymap_size = ei_keymap_get_size (keymap);
  buf = g_malloc0 (keymap_size + 1);
  while (TRUE)
    {
      int ret;

      ret = read (ei_keymap_get_fd (keymap), buf, keymap_size);
      if (ret > 0)
        {
          break;
        }
      else if (ret == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Keyboard layout was empty");
          goto err;
        }
      else if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        {
          continue;
        }
      else
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "Failed to read layout: %s", g_strerror (errno));
          goto err;
        }
    }

  xkb_keymap = xkb_keymap_new_from_string (xkb_context, buf,
                                           XKB_KEYMAP_FORMAT_TEXT_V1,
                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!xkb_keymap)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create XKB keymap");
      goto err;
    }

  xkb_state = xkb_state_new (xkb_keymap);
  if (!xkb_state)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create XKB state");
      goto err;
    }

  priv->xkb_context = xkb_context;
  priv->xkb_keymap = xkb_keymap;
  priv->xkb_state = xkb_state;
  return TRUE;

err:
  g_clear_pointer (&xkb_state, xkb_state_unref);
  g_clear_pointer (&xkb_keymap, xkb_keymap_unref);
  g_clear_pointer (&xkb_context, xkb_context_unref);
  return FALSE;
}

static gboolean
process_keymap (GrdSession        *session,
                struct ei_device  *device,
                GError           **error)
{
  struct ei_keymap *keymap;
  enum ei_keymap_type type;

  keymap = ei_device_keyboard_get_keymap (device);
  if (!keymap)
    return TRUE;

  type = ei_keymap_get_type (keymap);
  switch (type)
    {
    case EI_KEYMAP_TYPE_XKB:
      return setup_xkb_keymap (session, keymap, error);
    default:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unknown keyboard layout type");
      return FALSE;
    }

  return TRUE;
}

static void
grd_region_free (GrdRegion *region)
{
  ei_region_unref (region->ei_region);
  ei_device_unref (region->ei_device);
  g_free (region);
}

static void
maybe_dispose_ei_abs_pointer (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  g_hash_table_remove_all (priv->abs_pointer_regions);
  g_clear_pointer (&priv->ei_abs_pointer, ei_device_unref);
}

static void
maybe_dispose_ei_touch (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  g_signal_emit (session, signals[TOUCH_DEVICE_REMOVED], 0);
  g_hash_table_remove_all (priv->touch_regions);
  g_clear_pointer (&priv->ei_touch, ei_device_unref);
}

static void
process_regions (GrdSession       *session,
                 struct ei_device *ei_device,
                 GHashTable       *regions)
{
  size_t i = 0;
  struct ei_region *ei_region;

  while ((ei_region = ei_device_get_region (ei_device, i++)))
    {
      const char *mapping_id;
      GrdRegion *region;

      mapping_id = ei_region_get_mapping_id (ei_region);
      if (!mapping_id)
        continue;

      g_debug ("ei: New region: mapping-id: %s, %ux%u (%u, %u)",
               mapping_id,
               ei_region_get_width (ei_region), ei_region_get_height (ei_region),
               ei_region_get_x (ei_region), ei_region_get_y (ei_region));

      g_assert (ei_region_get_width (ei_region) > 0);
      g_assert (ei_region_get_height (ei_region) > 0);

      region = g_new0 (GrdRegion, 1);
      region->ei_device = ei_device_ref (ei_device);
      region->ei_region = ei_region_ref (ei_region);
      g_hash_table_insert (regions, g_strdup (mapping_id), region);
    }
}

static gboolean
grd_ei_source_dispatch (gpointer user_data)
{
  GrdSession *session = GRD_SESSION (user_data);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);
  struct ei_event *event;

  ei_dispatch (priv->ei);

  while ((event = ei_get_event (priv->ei)))
    {
      enum ei_event_type ei_event_type = ei_event_get_type (event);
      gboolean handled = TRUE;

      switch (ei_event_type)
        {
        case EI_EVENT_CONNECT:
        case EI_EVENT_DISCONNECT:
          break;
        case EI_EVENT_SEAT_ADDED:
          if (priv->ei_seat)
            break;

          priv->ei_seat = ei_seat_ref (ei_event_get_seat (event));
          ei_seat_bind_capabilities (priv->ei_seat,
                                     EI_DEVICE_CAP_POINTER,
                                     EI_DEVICE_CAP_KEYBOARD,
                                     EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                     EI_DEVICE_CAP_BUTTON,
                                     EI_DEVICE_CAP_SCROLL,
                                     EI_DEVICE_CAP_TOUCH,
                                     NULL);
          break;
        case EI_EVENT_SEAT_REMOVED:
          if (ei_event_get_seat (event) == priv->ei_seat)
            g_clear_pointer (&priv->ei_seat, ei_seat_unref);
          break;
        case EI_EVENT_DEVICE_ADDED:
          {
            struct ei_device *device = ei_event_get_device (event);

            if (ei_device_has_capability (device, EI_DEVICE_CAP_KEYBOARD))
              {
                g_autoptr (GError) error = NULL;

                g_clear_pointer (&priv->ei_keyboard, ei_device_unref);
                priv->ei_keyboard = ei_device_ref (device);
                if (!process_keymap (session, priv->ei_keyboard, &error))
                  {
                    g_warning ("Failed to handle keyboard layout: %s",
                               error->message);
                    ei_event_unref (event);
                    grd_session_stop (session);
                    return G_SOURCE_REMOVE;
                  }
              }
            if (ei_device_has_capability (device, EI_DEVICE_CAP_POINTER))
              {
                g_clear_pointer (&priv->ei_pointer, ei_device_unref);
                priv->ei_pointer = ei_device_ref (device);
              }
            if (ei_device_has_capability (device, EI_DEVICE_CAP_POINTER_ABSOLUTE))
              {
                maybe_dispose_ei_abs_pointer (session);
                priv->ei_abs_pointer = ei_device_ref (device);
                process_regions (session, device, priv->abs_pointer_regions);
              }
            if (ei_device_has_capability (device, EI_DEVICE_CAP_TOUCH))
              {
                maybe_dispose_ei_touch (session);
                priv->ei_touch = ei_device_ref (device);
                process_regions (session, device, priv->touch_regions);
              }
            break;
          }
        case EI_EVENT_DEVICE_RESUMED:
          if (ei_event_get_device (event) == priv->ei_pointer)
            ei_device_start_emulating (priv->ei_pointer, ++priv->ei_sequence);
          if (ei_event_get_device (event) == priv->ei_abs_pointer)
            ei_device_start_emulating (priv->ei_abs_pointer, ++priv->ei_sequence);
          if (ei_event_get_device (event) == priv->ei_keyboard)
            ei_device_start_emulating (priv->ei_keyboard, ++priv->ei_sequence);
          if (ei_event_get_device (event) == priv->ei_touch)
            {
              ei_device_start_emulating (priv->ei_touch, ++priv->ei_sequence);
              g_signal_emit (session, signals[TOUCH_DEVICE_ADDED], 0);
            }
          break;
        case EI_EVENT_DEVICE_PAUSED:
          break;
        case EI_EVENT_DEVICE_REMOVED:
          if (ei_event_get_device (event) == priv->ei_pointer)
            g_clear_pointer (&priv->ei_pointer, ei_device_unref);
          if (ei_event_get_device (event) == priv->ei_abs_pointer)
            maybe_dispose_ei_abs_pointer (session);
          if (ei_event_get_device (event) == priv->ei_keyboard)
            g_clear_pointer (&priv->ei_keyboard, ei_device_unref);
          if (ei_event_get_device (event) == priv->ei_touch)
            maybe_dispose_ei_touch (session);
          break;
        case EI_EVENT_KEYBOARD_MODIFIERS:
          if (priv->xkb_keymap)
            {
              GrdSessionClass *klass = GRD_SESSION_GET_CLASS (session);
              uint32_t latched_mods =
                ei_event_keyboard_get_xkb_mods_latched (event);
              uint32_t locked_mods =
                ei_event_keyboard_get_xkb_mods_locked (event);
              gboolean caps_lock_state;
              gboolean num_lock_state;

              caps_lock_state =
                !!((latched_mods | locked_mods) &
                   (1 << xkb_keymap_mod_get_index (priv->xkb_keymap,
                                                   XKB_MOD_NAME_CAPS)));
              num_lock_state =
                !!((latched_mods | locked_mods) &
                   (1 << xkb_keymap_mod_get_index (priv->xkb_keymap,
                                                   XKB_MOD_NAME_NUM)));

              if (!priv->locked_modifier_valid ||
                  caps_lock_state != priv->caps_lock_state)
                {
                  g_debug ("Caps lock state: %s",
                           caps_lock_state ? "locked" : "unlocked");
                  priv->caps_lock_state = caps_lock_state;

                  if (klass->on_caps_lock_state_changed)
                    {
                      klass->on_caps_lock_state_changed (session,
                                                         caps_lock_state);
                    }
                }

              if (!priv->locked_modifier_valid ||
                  num_lock_state != priv->num_lock_state)
                {
                  g_debug ("Num lock state: %s",
                           num_lock_state ? "locked" : "unlocked");
                  priv->num_lock_state = num_lock_state;

                  if (klass->on_num_lock_state_changed)
                    {
                      klass->on_num_lock_state_changed (session,
                                                        num_lock_state);
                    }
                }

              priv->locked_modifier_valid = TRUE;
            }
          else
            {
              g_warning ("Couldn't update caps lock / num lock state, "
                         "no keymap");
            }
          break;
        case EI_EVENT_PONG:
          {
            GrdEiPing *ping = NULL;

            if (g_hash_table_steal_extended (priv->pings,
                                             ei_event_pong_get_ping (event),
                                             NULL,
                                             (gpointer *) &ping))
              finish_and_free_ping (ping);
            break;
          }
        default:
          handled = FALSE;
          break;
        }

      if (handled)
        {
          g_debug ("ei: Handled event type %s",
                   ei_event_type_to_string (ei_event_type));
        }
      else
        {
          g_debug ("ei: Didn't handle event type %s",
                   ei_event_type_to_string (ei_event_type));
        }
      ei_event_unref (event);
    }

  return G_SOURCE_CONTINUE;
}

static void
on_eis_connected (GObject      *object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  GrdDBusMutterRemoteDesktopSession *proxy =
    GRD_DBUS_MUTTER_REMOTE_DESKTOP_SESSION (object);
  g_autoptr (GVariant) fd_variant = NULL;
  g_autoptr (GUnixFDList) fd_list = NULL;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GrdSessionClass *klass;
  int fd_idx = -1;
  int fd;
  g_autoptr (GError) error = NULL;
  const char *remote_desktop_session_id;
  GrdDBusMutterScreenCast *screen_cast_proxy;
  GVariantBuilder properties_builder;
  GVariant *properties_variant;
  int ret;

  if (!grd_dbus_mutter_remote_desktop_session_call_connect_to_eis_finish (proxy,
                                                                          &fd_variant,
                                                                          &fd_list,
                                                                          result,
                                                                          &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to connect to EIS: %s",
                 error->message);
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  session = GRD_SESSION (user_data);
  priv = grd_session_get_instance_private (session);
  klass = GRD_SESSION_GET_CLASS (session);

  fd_idx = g_variant_get_handle (fd_variant);
  if (!G_IS_UNIX_FD_LIST (fd_list) ||
      fd_idx < 0 || fd_idx >= g_unix_fd_list_get_length (fd_list))
    {
      g_warning ("Failed to acquire file descriptor for EI backend: Invalid "
                 "file descriptor list sent by display server");
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  fd = g_unix_fd_list_get (fd_list, fd_idx, &error);

  priv->ei = ei_new_sender (session);
  ei_configure_name (priv->ei, "gnome-remote-desktop");

  ret = ei_setup_backend_fd (priv->ei, fd);
  if (ret < 0)
    {
      g_warning ("Failed to setup libei backend: %s", g_strerror (-ret));

      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  priv->ei_source = grd_create_fd_source (ei_get_fd (priv->ei),
                                          "libei",
                                          grd_ei_source_prepare,
                                          grd_ei_source_dispatch,
                                          session, NULL);
  g_source_attach (priv->ei_source, NULL);
  g_source_unref (priv->ei_source);

  g_signal_connect (priv->remote_desktop_session, "closed",
                    G_CALLBACK (on_remote_desktop_session_closed),
                    session);
  g_signal_connect (priv->remote_desktop_session, "selection-owner-changed",
                    G_CALLBACK (on_remote_desktop_session_selection_owner_changed),
                    session);
  g_signal_connect (priv->remote_desktop_session, "selection-transfer",
                    G_CALLBACK (on_remote_desktop_session_selection_transfer),
                    session);

  remote_desktop_session_id =
    grd_dbus_mutter_remote_desktop_session_get_session_id (priv->remote_desktop_session);

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "remote-desktop-session-id",
                         g_variant_new_string (remote_desktop_session_id));
  g_variant_builder_add (&properties_builder, "{sv}",
                         "disable-animations",
                         g_variant_new_boolean (TRUE));
  properties_variant = g_variant_builder_end (&properties_builder);

  screen_cast_proxy = grd_context_get_mutter_screen_cast_proxy (priv->context);
  grd_dbus_mutter_screen_cast_call_create_session (screen_cast_proxy,
                                                   properties_variant,
                                                   priv->cancellable,
                                                   on_screen_cast_session_created,
                                                   session);

  if (klass->remote_desktop_session_ready)
    klass->remote_desktop_session_ready (session);
}

static void
on_remote_desktop_session_proxy_acquired (GObject      *object,
                                          GAsyncResult *result,
                                          gpointer      user_data)
{
  GrdDBusMutterRemoteDesktopSession *session_proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  g_autoptr (GError) error = NULL;
  GVariantBuilder options_builder;

  session_proxy =
    grd_dbus_mutter_remote_desktop_session_proxy_new_finish (result, &error);
  if (!session_proxy)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_warning ("Failed to acquire remote desktop session proxy: %s",
                 error->message);
      grd_session_stop (GRD_SESSION (user_data));
      return;
    }

  session = GRD_SESSION (user_data);
  priv = grd_session_get_instance_private (session);

  priv->remote_desktop_session = session_proxy;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE ("a{sv}"));

  grd_dbus_mutter_remote_desktop_session_call_connect_to_eis (
    priv->remote_desktop_session,
    g_variant_builder_end (&options_builder),
    NULL,
    priv->cancellable,
    on_eis_connected,
    session);
}

static void
on_remote_desktop_session_created (GObject      *source_object,
                                   GAsyncResult *res,
                                   gpointer      user_data)
{
  GrdDBusMutterRemoteDesktop *remote_desktop_proxy;
  GrdSession *session;
  GrdSessionPrivate *priv;
  GDBusConnection *connection;
  g_autofree char *session_path = NULL;
  g_autoptr (GError) error = NULL;

  remote_desktop_proxy = GRD_DBUS_MUTTER_REMOTE_DESKTOP (source_object);
  if (!grd_dbus_mutter_remote_desktop_call_create_session_finish (remote_desktop_proxy,
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

  grd_dbus_mutter_remote_desktop_session_proxy_new (connection,
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
  GrdDBusMutterRemoteDesktop *remote_desktop_proxy;

  priv->cancellable = g_cancellable_new ();

  remote_desktop_proxy = grd_context_get_mutter_remote_desktop_proxy (priv->context);

  g_assert (remote_desktop_proxy);

  grd_dbus_mutter_remote_desktop_call_create_session (remote_desktop_proxy,
                                                      priv->cancellable,
                                                      on_remote_desktop_session_created,
                                                      session);
}

static void
grd_session_finalize (GObject *object)
{
  GrdSession *session = GRD_SESSION (object);
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  if (priv->cancellable)
    g_assert (g_cancellable_is_cancelled (priv->cancellable));
  g_clear_object (&priv->cancellable);

  g_clear_pointer (&priv->touch_regions, g_hash_table_unref);
  g_clear_pointer (&priv->abs_pointer_regions, g_hash_table_unref);

  g_assert (!priv->xkb_state);
  g_assert (!priv->xkb_keymap);
  g_assert (!priv->xkb_context);

  g_assert (!priv->ei_touch);
  g_assert (!priv->ei_keyboard);
  g_assert (!priv->ei_abs_pointer);
  g_assert (!priv->ei_pointer);
  g_assert (!priv->ei_seat);
  g_assert (!priv->ei_source);
  g_assert (!priv->ei);

  g_assert (!priv->remote_desktop_session);
  g_assert (!priv->screen_cast_session);

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
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
grd_ei_ping_free (GrdEiPing *ping)
{
  g_object_unref (ping->task);
  g_clear_pointer (&ping->ping, ei_ping_unref);
  g_free (ping);
}

static void
cancel_and_free_ping (gpointer user_data)
{
  GrdEiPing *ping = user_data;
  GCancellable *cancellable;

  cancellable = g_task_get_cancellable (ping->task);
  if (!g_cancellable_is_cancelled (cancellable))
    g_cancellable_cancel (cancellable);
  grd_ei_ping_free (ping);
}

static void
finish_and_free_ping (GrdEiPing *ping)
{
  if (!g_task_return_error_if_cancelled (ping->task))
    g_task_return_boolean (ping->task, TRUE);
  grd_ei_ping_free (ping);
}

static void
grd_session_init (GrdSession *session)
{
  GrdSessionPrivate *priv = grd_session_get_instance_private (session);

  priv->abs_pointer_regions =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free,
                           (GDestroyNotify) grd_region_free);
  priv->touch_regions =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free,
                           (GDestroyNotify) grd_region_free);

  priv->pings = g_hash_table_new_full (NULL, NULL,
                                       NULL, cancel_and_free_ping);
}

static void
grd_session_class_init (GrdSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

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
  signals[TOUCH_DEVICE_ADDED] = g_signal_new ("touch-device-added",
                                              G_TYPE_FROM_CLASS (klass),
                                              G_SIGNAL_RUN_LAST,
                                              0,
                                              NULL, NULL, NULL,
                                              G_TYPE_NONE, 0);
  signals[TOUCH_DEVICE_REMOVED] = g_signal_new ("touch-device-removed",
                                                G_TYPE_FROM_CLASS (klass),
                                                G_SIGNAL_RUN_LAST,
                                                0,
                                                NULL, NULL, NULL,
                                                G_TYPE_NONE, 0);
}
