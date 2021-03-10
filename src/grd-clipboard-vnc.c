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

#include "grd-clipboard-vnc.h"

#include "grd-session-vnc.h"

struct _GrdClipboardVnc
{
  GrdClipboard parent;

  GrdSessionVnc *session_vnc;

  char *clipboard_utf8_string;
};

G_DEFINE_TYPE (GrdClipboardVnc, grd_clipboard_vnc, GRD_TYPE_CLIPBOARD);

static void
update_vnc_clipboard (GrdClipboardVnc *clipboard_vnc,
                      GrdMimeType      text_mime_type)
{
  GrdClipboard *clipboard = GRD_CLIPBOARD (clipboard_vnc);
  g_autoptr (GError) error = NULL;
  uint8_t *src_data;
  uint32_t src_size;
  char *dst_data;

  src_data = grd_clipboard_request_server_content_for_mime_type (clipboard,
                                                                 text_mime_type,
                                                                 &src_size);
  if (!src_data)
    return;

  dst_data = g_convert ((char *) src_data, src_size,
                        "iso8859-1", "utf-8",
                        NULL, NULL, &error);
  if (!dst_data)
    {
      g_warning ("[VNC.Clipboard] Failed to convert clipboard content: %s",
                 error->message);
      return;
    }

  grd_session_vnc_set_client_clipboard_text (clipboard_vnc->session_vnc,
                                             dst_data, strlen (dst_data));

  g_free (dst_data);
}

static void
grd_clipboard_vnc_update_client_mime_type_list (GrdClipboard *clipboard,
                                                GList        *mime_type_list)
{
  GrdClipboardVnc *clipboard_vnc = GRD_CLIPBOARD_VNC (clipboard);
  gboolean found_utf8_string = FALSE;
  GrdMimeType mime_type;
  GList *l;

  for (l = mime_type_list; l && !found_utf8_string; l = l->next)
    {
      mime_type = GPOINTER_TO_UINT (l->data);
      switch (mime_type)
        {
        case GRD_MIME_TYPE_TEXT_PLAIN:
          break;
        case GRD_MIME_TYPE_TEXT_PLAIN_UTF8:
        case GRD_MIME_TYPE_TEXT_UTF8_STRING:
          found_utf8_string = TRUE;
          break;
        case GRD_MIME_TYPE_TEXT_HTML:
        case GRD_MIME_TYPE_IMAGE_BMP:
        case GRD_MIME_TYPE_IMAGE_TIFF:
        case GRD_MIME_TYPE_IMAGE_GIF:
        case GRD_MIME_TYPE_IMAGE_JPEG:
        case GRD_MIME_TYPE_IMAGE_PNG:
        case GRD_MIME_TYPE_TEXT_URILIST:
        case GRD_MIME_TYPE_XS_GNOME_COPIED_FILES:
          break;
        default:
          g_assert_not_reached ();
        }
    }

  if (found_utf8_string)
    {
      g_clear_pointer (&clipboard_vnc->clipboard_utf8_string, g_free);

      update_vnc_clipboard (clipboard_vnc, mime_type);
    }

  g_list_free (mime_type_list);
}

static uint8_t *
grd_clipboard_vnc_request_client_content_for_mime_type (GrdClipboard     *clipboard,
                                                        GrdMimeTypeTable *mime_type_table,
                                                        uint32_t         *size)
{
  GrdClipboardVnc *clipboard_vnc = GRD_CLIPBOARD_VNC (clipboard);

  *size = strlen (clipboard_vnc->clipboard_utf8_string);

  return g_memdup2 (clipboard_vnc->clipboard_utf8_string, *size);
}

void
grd_clipboard_vnc_maybe_enable_clipboard (GrdClipboardVnc *clipboard_vnc)
{
  grd_clipboard_maybe_enable_clipboard (GRD_CLIPBOARD (clipboard_vnc));
}

void
grd_clipboard_vnc_set_clipboard_text (GrdClipboardVnc *clipboard_vnc,
                                      char            *text,
                                      int              text_length)
{
  GrdClipboard *clipboard = GRD_CLIPBOARD (clipboard_vnc);
  g_autoptr (GError) error = NULL;
  GrdMimeTypeTable *mime_type_table;
  GList *mime_type_tables = NULL;

  g_clear_pointer (&clipboard_vnc->clipboard_utf8_string, g_free);

  clipboard_vnc->clipboard_utf8_string = g_convert (text, text_length,
                                                    "utf-8", "iso8859-1",
                                                    NULL, NULL, &error);
  if (!clipboard_vnc->clipboard_utf8_string)
    {
      g_warning ("[VNC.Clipboard] Failed to convert clipboard content: %s",
                 error->message);
      return;
    }

  mime_type_table = g_malloc0 (sizeof (GrdMimeTypeTable));
  mime_type_table->mime_type = GRD_MIME_TYPE_TEXT_PLAIN_UTF8;
  mime_type_tables = g_list_append (mime_type_tables, mime_type_table);

  mime_type_table = g_malloc0 (sizeof (GrdMimeTypeTable));
  mime_type_table->mime_type = GRD_MIME_TYPE_TEXT_UTF8_STRING;
  mime_type_tables = g_list_append (mime_type_tables, mime_type_table);

  grd_clipboard_update_server_mime_type_list (clipboard, mime_type_tables);
}

GrdClipboardVnc *
grd_clipboard_vnc_new (GrdSessionVnc *session_vnc)
{
  GrdClipboardVnc *clipboard_vnc;

  clipboard_vnc = g_object_new (GRD_TYPE_CLIPBOARD_VNC, NULL);
  clipboard_vnc->session_vnc = session_vnc;

  grd_clipboard_initialize (GRD_CLIPBOARD (clipboard_vnc),
                            GRD_SESSION (session_vnc));

  return clipboard_vnc;
}

static void
grd_clipboard_vnc_dispose (GObject *object)
{
  GrdClipboardVnc *clipboard_vnc = GRD_CLIPBOARD_VNC (object);

  grd_clipboard_disable_clipboard (GRD_CLIPBOARD (clipboard_vnc));
  g_clear_pointer (&clipboard_vnc->clipboard_utf8_string, g_free);

  G_OBJECT_CLASS (grd_clipboard_vnc_parent_class)->dispose (object);
}

static void
grd_clipboard_vnc_init (GrdClipboardVnc *clipboard_vnc)
{
}

static void
grd_clipboard_vnc_class_init (GrdClipboardVncClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdClipboardClass *clipboard_class = GRD_CLIPBOARD_CLASS (klass);

  object_class->dispose = grd_clipboard_vnc_dispose;

  clipboard_class->update_client_mime_type_list =
    grd_clipboard_vnc_update_client_mime_type_list;
  clipboard_class->request_client_content_for_mime_type =
    grd_clipboard_vnc_request_client_content_for_mime_type;
}
