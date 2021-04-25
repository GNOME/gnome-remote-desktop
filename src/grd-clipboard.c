/*
 * Copyright (C) 2020 Pascal Nowack
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

#include <glib-unix.h>

#include "grd-session.h"

typedef struct _GrdClipboardPrivate
{
  GrdSession *session;

  gboolean enabled;

  GHashTable *client_mime_type_tables;
} GrdClipboardPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdClipboard, grd_clipboard, G_TYPE_OBJECT);

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
      if (mime_type_tables)
        grd_session_set_selection (priv->session, mime_type_tables);
    }
  g_debug ("Clipboard[SetSelection]: Update complete");

  g_list_free (mime_type_tables);
}

static uint8_t *
read_mime_type_content_sync (int       fd,
                             uint32_t *size)
{
  GArray *data;

  *size = 0;
  if (fd == -1)
    return NULL;

  data = g_array_new (FALSE, TRUE, sizeof (uint8_t));
  while (TRUE)
    {
      int len;
      uint8_t buffer[1024];

      len = read (fd, buffer, G_N_ELEMENTS (buffer));
      if (len < 0)
        {
          if (errno == EAGAIN)
            continue;

          g_warning ("read() failed: %s", g_strerror (errno));
          break;
        }
      else if (len == 0)
        {
          break;
        }
      else
        {
          g_array_append_vals (data, buffer, len);
        }
    }
  close (fd);

  if (data->len <= 0)
    {
      g_array_free (data, TRUE);
      return NULL;
    }

  *size = data->len;

  return (uint8_t *) g_array_free (data, FALSE);
}

void
grd_clipboard_request_server_content_for_mime_type_async (GrdClipboard *clipboard,
                                                          GrdMimeType   mime_type)
{
  GrdClipboardClass *klass = GRD_CLIPBOARD_GET_CLASS (clipboard);
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);
  int fd;
  uint8_t *data;
  uint32_t size;

  g_return_if_fail (klass->submit_requested_server_content);

  if (!priv->enabled)
    return;

  g_debug ("Clipboard[SelectionRead]: Requesting data from servers clipboard"
           " (mime type: %s)", grd_mime_type_to_string (mime_type));
  fd = grd_session_selection_read (priv->session, mime_type);

  data = read_mime_type_content_sync (fd, &size);
  if (data)
    g_debug ("Clipboard[SelectionRead]: Request successful");
  else
    g_debug ("Clipboard[SelectionRead]: Request failed");

  klass->submit_requested_server_content (clipboard, data, size);
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

  g_clear_pointer (&priv->client_mime_type_tables, g_hash_table_destroy);

  G_OBJECT_CLASS (grd_clipboard_parent_class)->dispose (object);
}

static void
grd_clipboard_init (GrdClipboard *clipboard)
{
  GrdClipboardPrivate *priv = grd_clipboard_get_instance_private (clipboard);

  priv->client_mime_type_tables = g_hash_table_new_full (NULL, NULL, NULL,
                                                         free_mime_type_table);
}

static void
grd_clipboard_class_init (GrdClipboardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_clipboard_dispose;
}
