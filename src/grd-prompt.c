/*
 * Copyright (C) 2018 Red Hat Inc.
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
 */

#include "config.h"

#include <glib/gi18n.h>
#include <libnotify/notify.h>

#include "grd-prompt.h"

typedef struct _GrdPromptResult
{
  GrdPromptResponse response;
} GrdPromptResult;

struct _GrdPrompt
{
  GObject parent;
};

G_DEFINE_TYPE (GrdPrompt, grd_prompt, G_TYPE_OBJECT)

static void
handle_notification_response (NotifyNotification *notification,
                              char               *response,
                              gpointer            user_data)
{
  GTask *task = G_TASK (user_data);

  if (g_strcmp0 (response, "accept") == 0)
    {
      g_task_return_int (task, GRD_PROMPT_RESPONSE_ACCEPT);
    }
  else if (g_strcmp0 (response, "refuse") == 0 ||
           g_strcmp0 (response, "closed") == 0)
    {
      g_task_return_int (task, GRD_PROMPT_RESPONSE_REFUSE);
    }
  else
    {
      g_warning ("Unknown prompt response '%s'", response);
      g_task_return_int (task, GRD_PROMPT_RESPONSE_REFUSE);
    }
}

static void
on_notification_closed (NotifyNotification *notification,
                        gpointer            user_data)
{
  handle_notification_response (notification, "closed", user_data);
}

static gboolean
cancelled_idle_callback (gpointer user_data)
{
  GTask *task = G_TASK (user_data);

  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                           "Prompt was cancelled");
  return G_SOURCE_REMOVE;
}

static void
on_cancellable_cancelled (GCancellable *cancellable,
                          GTask        *task)
{
  NotifyNotification *notification =
    NOTIFY_NOTIFICATION (g_task_get_task_data (task));

  notify_notification_close (notification, NULL);
  g_idle_add (cancelled_idle_callback, task);
}

static gboolean
show_notification_idle_callback (gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  GCancellable *cancellable = g_task_get_cancellable (task);
  NotifyNotification *notification;
  GError *error = NULL;

  if (g_cancellable_is_cancelled (cancellable))
    return G_SOURCE_REMOVE;

  notification = g_task_get_task_data (task);

  if (!notify_notification_show (notification, &error))
    g_task_return_error (task, error);

  return G_SOURCE_REMOVE;
}

void
grd_prompt_query_async (GrdPrompt           *prompt,
                        const char          *remote_host,
                        GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
  NotifyNotification *notification;
  GTask *task;
  g_autofree char *summary = NULL;
  g_autofree char *body = NULL;

  task = g_task_new (G_OBJECT (prompt), cancellable, callback, user_data);

  summary = g_strdup_printf (_("Do you want to share your desktop?"));
  body = g_strdup_printf (_("A user on the computer '%s' is trying to remotely view or control your desktop."),
                          remote_host);
  notification = notify_notification_new (summary, body,
                                          "preferences-desktop-remote-desktop");
  notify_notification_add_action (notification,
                                  "refuse",
                                  _("Refuse"),
                                  handle_notification_response,
                                  task, NULL);
  notify_notification_add_action (notification,
                                  "accept",
                                  _("Accept"),
                                  handle_notification_response,
                                  task, NULL);
  g_task_set_task_data (task, notification, g_object_unref);

  g_signal_connect (notification, "closed",
                    G_CALLBACK (on_notification_closed), task);
  g_cancellable_connect (cancellable,
                         G_CALLBACK (on_cancellable_cancelled),
                         task, NULL);

  g_idle_add (show_notification_idle_callback, task);
}

gboolean
grd_prompt_query_finish (GrdPrompt          *prompt,
                         GAsyncResult       *result,
                         GrdPromptResponse  *out_response,
                         GError            **error)
{
  g_autoptr(GTask) task = G_TASK (result);
  GrdPromptResponse response;

  response = g_task_propagate_int (task, error);
  if (response == -1)
    return FALSE;

  *out_response = response;
  return TRUE;
}

static void
grd_prompt_init (GrdPrompt *prompt)
{
}

static void
grd_prompt_class_init (GrdPromptClass *klass)
{
  notify_init (g_get_application_name ());
}
