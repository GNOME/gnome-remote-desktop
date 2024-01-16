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
  else if (g_strcmp0 (response, "cancel") == 0 ||
           g_strcmp0 (response, "closed") == 0)
    {
      g_task_return_int (task, GRD_PROMPT_RESPONSE_CANCEL);
    }
  else
    {
      g_warning ("Unknown prompt response '%s'", response);
      g_task_return_int (task, GRD_PROMPT_RESPONSE_CANCEL);
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

  g_signal_handlers_disconnect_by_func (notification,
                                        G_CALLBACK (on_notification_closed),
                                        task);
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
                        GrdPromptDefinition *prompt_definition,
                        GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
  NotifyNotification *notification;
  GTask *task;

  task = g_task_new (G_OBJECT (prompt), cancellable, callback, user_data);

  g_assert (prompt_definition);
  g_assert (prompt_definition->summary ||
            prompt_definition->body);

  notification = notify_notification_new (prompt_definition->summary,
                                          prompt_definition->body,
                                          "preferences-desktop-remote-desktop");

  notify_notification_set_urgency (notification, NOTIFY_URGENCY_CRITICAL);

  if (prompt_definition->cancel_label)
    {
      notify_notification_add_action (notification,
                                      "cancel",
                                      prompt_definition->cancel_label,
                                      handle_notification_response,
                                      task, NULL);
    }

  if (prompt_definition->accept_label)
    {
      notify_notification_add_action (notification,
                                      "accept",
                                      prompt_definition->accept_label,
                                      handle_notification_response,
                                      task, NULL);
    }

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
  GCancellable *cancellable;
  GrdPromptResponse response;

  cancellable = g_task_get_cancellable (task);
  g_signal_handlers_disconnect_by_func (cancellable,
                                        G_CALLBACK (on_cancellable_cancelled),
                                        task);

  response = g_task_propagate_int (task, error);
  if (response == -1)
    return FALSE;

  *out_response = response;
  return TRUE;
}

void
grd_prompt_definition_free (GrdPromptDefinition *prompt_definition)
{
  g_clear_pointer (&prompt_definition->summary, g_free);
  g_clear_pointer (&prompt_definition->body, g_free);
  g_clear_pointer (&prompt_definition->accept_label, g_free);
  g_clear_pointer (&prompt_definition->cancel_label, g_free);
  g_free (prompt_definition);
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
