/*
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
 */

#include "config.h"

#include "grd-clipboard.h"

#include <gio/gio.h>
#include <gio/gunixinputstream.h>

#include "grd-session.h"

#define MAX_READ_TIME 4000

typedef struct _ReadMimeTypeContentContext
{
  GrdClipboard *clipboard;
  int fd;
  GCancellable *cancellable;
} ReadMimeTypeContentContext;

typedef struct _ReadMimeTypeContentResult
{
  uint8_t *data;
  uint32_t size;
} ReadMimeTypeContentResult;

typedef struct _GrdClipboardPrivate
{
  GrdSession *session;

  gboolean enabled;

  GHashTable *client_mime_type_tables;

  ReadMimeTypeContentResult *read_result;
  GCancellable *read_cancellable;
  unsigned int abort_read_source_id;
  gboolean has_pending_read_operation;

  GCond pending_read_cond;
  GMutex pending_read_mutex;
} GrdClipboardPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdClipboard, grd_clipboard, G_TYPE_OBJECT);

static void
handle_read_result (GrdClipboard              *clipboard,
                    ReadMimeTypeContentResult *read_result)
{
  GrdClipboardClass *klass = GRD_CLIPBOARD_GET_CLASS (clipboard);
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);

  priv->has_pending_read_operation = FALSE;

  /* Discard the read_result, if the clipboard is already disabled. */
  if (!priv->enabled)
    return;

  if (read_result->data)
    g_debug ("Clipboard[SelectionRead]: Request successful");
  else
    g_debug ("Clipboard[SelectionRead]: Request failed");

  klass->submit_requested_server_content (clipboard, read_result->data,
                                          read_result->size);
}

static void
flush_pending_read_result (GrdClipboard *clipboard)
{
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);
  ReadMimeTypeContentResult *read_result;

  g_mutex_lock (&priv->pending_read_mutex);
  while (!priv->read_result)
    g_cond_wait (&priv->pending_read_cond, &priv->pending_read_mutex);
  g_mutex_unlock (&priv->pending_read_mutex);

  read_result = g_steal_pointer (&priv->read_result);

  handle_read_result (clipboard, read_result);
  g_free (read_result);
}

static void
abort_current_read_operation (GrdClipboard *clipboard)
{
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);

  if (!priv->has_pending_read_operation)
    return;

  g_debug ("Clipboard[SelectionRead]: Aborting current read operation");
  g_cancellable_cancel (priv->read_cancellable);

  g_clear_object (&priv->read_cancellable);
  g_clear_handle_id (&priv->abort_read_source_id, g_source_remove);

  flush_pending_read_result (clipboard);
}

void
grd_clipboard_update_server_mime_type_list (GrdClipboard *clipboard,
                                            GList        *mime_type_tables)
{
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);
  GList *l;

  g_debug ("Clipboard[SetSelection]: Updating servers clipboard");
  for (l = mime_type_tables; l; l = l->next)
    {
      GrdMimeTypeTable *mime_type_table = l->data;
      GrdMimeType mime_type;

      mime_type = mime_type_table->mime_type;
      g_debug ("Clipboard[SetSelection]: Update contains mime type %s",
               grd_mime_type_to_string (mime_type));

      g_hash_table_insert (priv->client_mime_type_tables,
                           GUINT_TO_POINTER (mime_type), mime_type_table);
    }

  if (!priv->enabled)
    {
      g_debug ("Clipboard[EnableClipboard]: Enabling clipboard");
      priv->enabled = grd_session_enable_clipboard (priv->session,
                                                    clipboard, mime_type_tables);
      if (priv->enabled)
        g_debug ("Clipboard[EnableClipboard]: Clipboard enabled");
      else
        g_debug ("Clipboard[EnableClipboard]: Clipboard could not be enabled");
    }
  else
    {
      abort_current_read_operation (clipboard);

      if (mime_type_tables)
        grd_session_set_selection (priv->session, mime_type_tables);
    }
  g_debug ("Clipboard[SetSelection]: Update complete");

  g_list_free (mime_type_tables);
}

static void
async_read_operation_complete (GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  GrdClipboard *clipboard = user_data;
  GrdClipboardPrivate *priv;
  ReadMimeTypeContentContext *read_context =
    g_task_get_task_data (G_TASK (result));
  ReadMimeTypeContentResult *read_result;

  if (g_cancellable_is_cancelled (read_context->cancellable))
    return;

  priv = grd_clipboard_get_instance_private (clipboard);
  g_assert (priv->has_pending_read_operation);

  g_clear_object (&priv->read_cancellable);
  g_clear_handle_id (&priv->abort_read_source_id, g_source_remove);

  read_result = g_steal_pointer (&priv->read_result);

  handle_read_result (clipboard, read_result);
  g_free (read_result);
}

static void
clear_read_context (gpointer data)
{
  ReadMimeTypeContentContext *read_context = data;

  g_object_unref (read_context->cancellable);

  g_free (data);
}

static void
read_mime_type_content_in_thread (GTask        *task,
                                  gpointer      source_object,
                                  gpointer      task_data,
                                  GCancellable *cancellable)
{
  ReadMimeTypeContentContext *read_context = task_data;
  GrdClipboard *clipboard = read_context->clipboard;
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);
  ReadMimeTypeContentResult *read_result;
  GInputStream *input_stream;
  GArray *data;
  gboolean success = FALSE;
  g_autoptr (GError) error = NULL;

  input_stream = g_unix_input_stream_new (read_context->fd, TRUE);
  data = g_array_new (FALSE, TRUE, sizeof (uint8_t));

  while (TRUE)
    {
      int len;
      uint8_t buffer[1024];

      len = g_input_stream_read (input_stream, buffer, G_N_ELEMENTS (buffer),
                                 read_context->cancellable, &error);
      if (len < 0)
        {
          g_warning ("Clipboard[SelectionRead]: Failed to read mime type "
                     "content: %s", error->message);
          break;
        }
      else if (len == 0)
        {
          success = TRUE;
          break;
        }
      else
        {
          g_array_append_vals (data, buffer, len);
        }
    }

  read_result = g_malloc0 (sizeof (ReadMimeTypeContentResult));
  if (success && data->len > 0)
    {
      read_result->size = data->len;
      read_result->data = (uint8_t *) g_array_free (data, FALSE);
    }
  else
    {
      g_array_free (data, TRUE);
    }

  g_object_unref (input_stream);

  g_mutex_lock (&priv->pending_read_mutex);
  priv->read_result = read_result;
  g_cond_signal (&priv->pending_read_cond);
  g_mutex_unlock (&priv->pending_read_mutex);
}

static gboolean
abort_mime_type_content_read (gpointer user_data)
{
  GrdClipboard *clipboard = user_data;
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);

  g_debug ("Clipboard[SelectionRead]: Aborting current read operation "
           "(Timeout reached)");

  g_assert (priv->has_pending_read_operation);
  g_assert (priv->abort_read_source_id);

  priv->abort_read_source_id = 0;
  abort_current_read_operation (clipboard);

  return G_SOURCE_REMOVE;
}

static void
read_mime_type_content_async (GrdClipboard *clipboard,
                              int           fd)
{
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);
  ReadMimeTypeContentContext *read_context;
  GTask *task;

  abort_current_read_operation (clipboard);
  priv->read_cancellable = g_cancellable_new ();
  priv->has_pending_read_operation = TRUE;
  g_assert (!priv->read_result);

  read_context = g_malloc0 (sizeof (ReadMimeTypeContentContext));
  read_context->clipboard = clipboard;
  read_context->fd = fd;
  read_context->cancellable = g_object_ref (priv->read_cancellable);

  task = g_task_new (NULL, NULL, async_read_operation_complete, clipboard);
  g_task_set_task_data (task, read_context, clear_read_context);
  g_task_run_in_thread (task, read_mime_type_content_in_thread);
  g_object_unref (task);

  priv->abort_read_source_id =
    g_timeout_add (MAX_READ_TIME, abort_mime_type_content_read, clipboard);
}

void
grd_clipboard_request_server_content_for_mime_type_async (GrdClipboard *clipboard,
                                                          GrdMimeType   mime_type)
{
  GrdClipboardClass *klass = GRD_CLIPBOARD_GET_CLASS (clipboard);
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);
  int fd;

  g_return_if_fail (klass->submit_requested_server_content);

  if (!priv->enabled)
    return;

  g_debug ("Clipboard[SelectionRead]: Requesting data from servers clipboard"
           " (mime type: %s)", grd_mime_type_to_string (mime_type));
  fd = grd_session_selection_read (priv->session, mime_type);
  if (fd == -1)
    {
      g_debug ("Clipboard[SelectionRead]: Request failed");
      klass->submit_requested_server_content (clipboard, NULL, 0);

      return;
    }

  read_mime_type_content_async (clipboard, fd);
}

void
grd_clipboard_initialize (GrdClipboard *clipboard,
                          GrdSession   *session)
{
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);

  priv->session = session;
}

void
grd_clipboard_maybe_enable_clipboard (GrdClipboard *clipboard)
{
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);

  g_debug ("Clipboard[EnableClipboard]: Enabling clipboard");
  if (priv->enabled)
    {
      g_debug ("Clipboard[EnableClipboard]: Clipboard already enabled");
      return;
    }

  priv->enabled = grd_session_enable_clipboard (priv->session, clipboard, NULL);
  if (priv->enabled)
    g_debug ("Clipboard[EnableClipboard]: Clipboard enabled");
  else
    g_debug ("Clipboard[EnableClipboard]: Clipboard could not be enabled");
}

void
grd_clipboard_disable_clipboard (GrdClipboard *clipboard)
{
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);

  if (!priv->enabled)
    return;

  g_debug ("Clipboard[DisableClipboard]: Disabling clipboard");
  grd_session_disable_clipboard (priv->session);
  priv->enabled = FALSE;
}

void
grd_clipboard_update_client_mime_type_list (GrdClipboard *clipboard,
                                            GList        *mime_type_list)
{
  GrdClipboardClass *klass = GRD_CLIPBOARD_GET_CLASS (clipboard);
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);
  GList *l;

  /**
   * Ensure that the response with the read mime type content is sent to the
   * client first, before sending the new mime type list
   */
  abort_current_read_operation (clipboard);

  if (!klass->update_client_mime_type_list)
    return;

  for (l = mime_type_list; l; l = l->next)
    g_hash_table_remove (priv->client_mime_type_tables, l->data);

  g_debug ("Clipboard[SelectionOwnerChanged]: Updating clients clipboard");
  klass->update_client_mime_type_list (clipboard, mime_type_list);
  g_debug ("Clipboard[SelectionOwnerChanged]: Update complete");
}

uint8_t *
grd_clipboard_request_client_content_for_mime_type (GrdClipboard *clipboard,
                                                    GrdMimeType   mime_type,
                                                    uint32_t     *size)
{
  GrdClipboardClass *klass = GRD_CLIPBOARD_GET_CLASS (clipboard);
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);
  GrdMimeTypeTable *mime_type_table = NULL;
  uint8_t *mime_type_content = NULL;

  *size = 0;

  if (!klass->request_client_content_for_mime_type)
    return NULL;

  g_debug ("Clipboard[SelectionTransfer]: Requesting data from clients clipboard"
           " (mime type: %s)", grd_mime_type_to_string (mime_type));
  mime_type_table = g_hash_table_lookup (priv->client_mime_type_tables,
                                         GUINT_TO_POINTER (mime_type));
  if (mime_type_table)
    {
      mime_type_content = klass->request_client_content_for_mime_type (
                            clipboard, mime_type_table, size);
    }
  if (mime_type_content)
    g_debug ("Clipboard[SelectionTransfer]: Request successful");
  else
    g_debug ("Clipboard[SelectionTransfer]: Request failed");

  return mime_type_content;
}

static void
free_mime_type_table (gpointer data)
{
  GrdMimeTypeTable *mime_type_table = data;

  g_free (mime_type_table);
}

static void
grd_clipboard_dispose (GObject *object)
{
  GrdClipboard *clipboard = GRD_CLIPBOARD (object);
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);

  abort_current_read_operation (clipboard);

  g_clear_pointer (&priv->client_mime_type_tables, g_hash_table_destroy);

  G_OBJECT_CLASS (grd_clipboard_parent_class)->dispose (object);
}

static void
grd_clipboard_finalize (GObject *object)
{
  GrdClipboard *clipboard = GRD_CLIPBOARD (object);
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);

  g_mutex_clear (&priv->pending_read_mutex);
  g_cond_clear (&priv->pending_read_cond);

  G_OBJECT_CLASS (grd_clipboard_parent_class)->finalize (object);
}

static void
grd_clipboard_init (GrdClipboard *clipboard)
{
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);

  priv->client_mime_type_tables = g_hash_table_new_full (NULL, NULL, NULL,
                                                         free_mime_type_table);

  g_cond_init (&priv->pending_read_cond);
  g_mutex_init (&priv->pending_read_mutex);
}

static void
grd_clipboard_class_init (GrdClipboardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_clipboard_dispose;
  object_class->finalize = grd_clipboard_finalize;
}
