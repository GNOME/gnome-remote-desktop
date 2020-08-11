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

typedef struct _GrdClipboardPrivate
{
  GrdSession *session;

  GHashTable *client_mime_type_tables;
} GrdClipboardPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GrdClipboard, grd_clipboard, G_TYPE_OBJECT);

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

  klass->update_client_mime_type_list (clipboard, mime_type_list);
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

  mime_type_table = g_hash_table_lookup (priv->client_mime_type_tables,
                                         GUINT_TO_POINTER (mime_type));
  if (mime_type_table)
    {
      mime_type_content = klass->request_client_content_for_mime_type (
                            clipboard, mime_type_table, size);
    }

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
